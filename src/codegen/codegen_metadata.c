#include "codegen_internal.h"

// Initialize TBAA + branch-weight + loop-metadata caches for this compiler.
// Idempotent: safe to call more than once (subsequent calls are no-ops once
// tbaa_root is set).
void init_metadata(KawaCompiler *c) {
	if (c->tbaa_root)
		return;

	LLVMContextRef ctx = c->context;

	// Root access tag. The root is a self-referential MDNode: it's the
	// identity for "any memory", so any access rooted here doesn't conflict
	// with any other rooted access by default. Disjoint fields opt in to
	// more precise tags (tbaa_scalar / tbaa_int / tbaa_ptr).
	LLVMMetadataRef root_name =
		LLVMMDStringInContext2(ctx, "Kawa TBAA", 9);
	LLVMMetadataRef root_args[] = {root_name};
	c->tbaa_root = LLVMMDNodeInContext2(ctx, root_args, 1);
	// Replace first operand with self-reference. LLVM requires the root node
	// to be a self-cycle; without this, the verifier rejects it.
	LLVMMetadataRef root_self_args[] = {c->tbaa_root};
	LLVMMetadataRef root_placeholder =
		LLVMMDNodeInContext2(ctx, root_self_args, 1);
	// LLVMReplaceMDNodeOperandWith signature (LLVM 21):
	//   (LLVMValueRef V, unsigned Index, LLVMMetadataRef Replacement)
	// so convert V via LLVMMetadataAsValue, pass Replacement as Metadata.
	LLVMReplaceMDNodeOperandWith(LLVMMetadataAsValue(ctx, c->tbaa_root), 0,
								 root_placeholder);

	// Scalar access tag (fallback for FP, vector, etc.). Parent is the root.
	LLVMMetadataRef scalar_name =
		LLVMMDStringInContext2(ctx, "tbaa_scalar", 11);
	LLVMMetadataRef scalar_args[] = {scalar_name, c->tbaa_root};
	c->tbaa_scalar = LLVMMDNodeInContext2(ctx, scalar_args, 2);

	// Pointer access tag. Parent is scalar; lets us say "two different
	// pointer loads don't conflict".
	LLVMMetadataRef ptr_name = LLVMMDStringInContext2(ctx, "tbaa_ptr", 8);
	LLVMMetadataRef ptr_args[] = {ptr_name, c->tbaa_scalar};
	c->tbaa_ptr = LLVMMDNodeInContext2(ctx, ptr_args, 2);

	// Integer access tag (parent = scalar). For ints/floats/bools/chars
	// (i1, i8, i16, i32, i64).
	LLVMMetadataRef int_name = LLVMMDStringInContext2(ctx, "tbaa_int", 8);
	LLVMMetadataRef int_args[] = {int_name, c->tbaa_scalar};
	c->tbaa_int = LLVMMDNodeInContext2(ctx, int_args, 2);
}

// Attach TBAA metadata to a load or store. For aggregate types (struct/array),
// TBAA at the field granularity is not modeled here -- we skip attaching TBAA
// entirely for those, since attaching a scalar tag to a struct load would
// produce incorrect aliasing results that defeat CSE within a struct.
void attach_tbaa(KawaCompiler *c, LLVMValueRef instr, LLVMTypeRef type) {
	if (!instr || !type)
		return;
	LLVMTypeKind kind = LLVMGetTypeKind(type);
	if (kind == LLVMStructTypeKind || kind == LLVMArrayTypeKind)
		return;

	LLVMMetadataRef tag = c->tbaa_scalar;
	if (kind == LLVMPointerTypeKind)
		tag = c->tbaa_ptr;
	else if (kind == LLVMIntegerTypeKind)
		tag = c->tbaa_int;

	// TBAA access shape: (base_tag, access_tag, offset). For our coarse
	// per-type tags we always use offset 0 -- the precision comes from the
	// tag identity, not the offset.
	LLVMMetadataRef offset_md = LLVMValueAsMetadata(
		LLVMConstInt(LLVMInt64TypeInContext(c->context), 0, 0));
	LLVMMetadataRef args[] = {tag, tag, offset_md};
	LLVMMetadataRef access_tag = LLVMMDNodeInContext2(c->context, args, 3);
	unsigned kind_id = LLVMGetMDKindID("tbaa", 4);
	LLVMSetMetadata(instr, kind_id, LLVMMetadataAsValue(c->context, access_tag));
}

// Attach branch_weights to a conditional branch.
// The shape must be (MDString "branch_weights", i32 true, i32 false).
// The earlier implementation passed an MDString as the first operand to
// LLVMMDNodeInContext (a deprecated API that returns LLVMValueRef), which
// produced a malformed prof node the optimizer silently ignored.
void set_branch_weights(KawaCompiler *c, LLVMValueRef br_instr,
						unsigned true_weight, unsigned false_weight) {
	LLVMMetadataRef name =
		LLVMMDStringInContext2(c->context, "branch_weights", 14);
	LLVMMetadataRef t_w = LLVMValueAsMetadata(
		LLVMConstInt(LLVMInt32TypeInContext(c->context), true_weight, 0));
	LLVMMetadataRef f_w = LLVMValueAsMetadata(
		LLVMConstInt(LLVMInt32TypeInContext(c->context), false_weight, 0));
	LLVMMetadataRef args[] = {name, t_w, f_w};
	LLVMMetadataRef md_node = LLVMMDNodeInContext2(c->context, args, 3);
	unsigned kind_id = LLVMGetMDKindID("prof", 4);
	LLVMSetMetadata(br_instr, kind_id,
					LLVMMetadataAsValue(c->context, md_node));
}

// NOTE: we deliberately attach NO llvm.loop metadata. Earlier versions
// forced `unroll.enable`, which made the full unroller expand large loops
// into >1M-line modules (observed 60x+ slowdowns from i-cache thrash), and
// `unroll.disable`, which blocked profitable store interleaving in fill
// loops. default<O3> makes better decisions than either hint -- the C
// front-end runs these same loops with no metadata and wins because of it.
// Branch-probability weights (set_branch_weights) stay: those are facts
// about our code shape, not guesses about the optimizer's tuning.

// Permissions are independent; strict IEEE behavior is the default.
void set_fast_math(KawaCompiler *c, LLVMValueRef instr) {
	if (!instr)
		return;
	LLVMFastMathFlags flags = LLVMFastMathNone;
	if (c->fp_permissions & 1) flags |= LLVMFastMathAllowContract;
	if (c->fp_permissions & 2) flags |= LLVMFastMathAllowReassoc;
	if (c->fp_permissions & 4) flags |= LLVMFastMathNoNaNs | LLVMFastMathNoInfs;
	LLVMSetFastMathFlags(instr, flags);
}
