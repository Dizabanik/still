#include "codegen_internal.h"

// --- Metadata Cache ---
LLVMValueRef tbaa_root = NULL;
LLVMValueRef tbaa_scalar = NULL;
LLVMValueRef tbaa_ptr = NULL;
LLVMValueRef tbaa_int = NULL;

void add_loop_metadata(KawaCompiler *c, LLVMValueRef branch_instr) {
	LLVMContextRef ctx = c->context;
	LLVMValueRef one_val = LLVMConstInt(LLVMInt1TypeInContext(ctx), 1, 0);
	LLVMMetadataRef one_md = LLVMValueAsMetadata(one_val);
	LLVMMetadataRef vec_str =
		LLVMMDStringInContext2(ctx, "llvm.loop.vectorize.enable", 26);
	LLVMMetadataRef unroll_str =
		LLVMMDStringInContext2(ctx, "llvm.loop.unroll.enable", 23);
	LLVMMetadataRef vec_args[] = {vec_str, one_md};
	LLVMMetadataRef vec_node = LLVMMDNodeInContext2(ctx, vec_args, 2);
	LLVMMetadataRef unroll_args[] = {unroll_str, one_md};
	LLVMMetadataRef unroll_node = LLVMMDNodeInContext2(ctx, unroll_args, 2);
	LLVMMetadataRef temp_args[] = {one_md};
	LLVMMetadataRef temp_node = LLVMMDNodeInContext2(ctx, temp_args, 1);
	LLVMMetadataRef loop_args[] = {temp_node, vec_node, unroll_node};
	LLVMMetadataRef loop_md = LLVMMDNodeInContext2(ctx, loop_args, 3);
	LLVMValueRef loop_md_as_val = LLVMMetadataAsValue(ctx, loop_md);
	LLVMReplaceMDNodeOperandWith(loop_md_as_val, 0, loop_md);
	unsigned kind_id = LLVMGetMDKindID("llvm.loop", 9);
	LLVMSetMetadata(branch_instr, kind_id, loop_md_as_val);
}

void init_metadata(KawaCompiler *c) {
	if (tbaa_root)
		return;
	LLVMValueRef root_name = LLVMMDStringInContext(c->context, "Kawa TBAA", 9);
	LLVMValueRef root_node = LLVMMDNodeInContext(c->context, &root_name, 1);
	tbaa_root = root_node;
	LLVMValueRef scalar_name =
		LLVMMDStringInContext(c->context, "tbaa_scalar", 11);
	LLVMValueRef scalar_args[] = {scalar_name, tbaa_root};
	tbaa_scalar = LLVMMDNodeInContext(c->context, scalar_args, 2);
	LLVMValueRef ptr_name = LLVMMDStringInContext(c->context, "tbaa_ptr", 8);
	LLVMValueRef ptr_args[] = {ptr_name, tbaa_scalar};
	tbaa_ptr = LLVMMDNodeInContext(c->context, ptr_args, 2);
	LLVMValueRef int_name = LLVMMDStringInContext(c->context, "tbaa_int", 8);
	LLVMValueRef int_args[] = {int_name, tbaa_scalar};
	tbaa_int = LLVMMDNodeInContext(c->context, int_args, 2);
}

void attach_tbaa(KawaCompiler *c, LLVMValueRef instr, LLVMTypeRef type) {
	LLVMTypeKind kind = LLVMGetTypeKind(type);
	if (kind == LLVMStructTypeKind || kind == LLVMArrayTypeKind)
		return;
	LLVMValueRef tag = tbaa_scalar;
	if (kind == LLVMPointerTypeKind)
		tag = tbaa_ptr;
	else if (kind == LLVMIntegerTypeKind)
		tag = tbaa_int;
	LLVMValueRef args[] = {
		tag, tag, LLVMConstInt(LLVMInt64TypeInContext(c->context), 0, 0)};
	LLVMValueRef access_tag = LLVMMDNodeInContext(c->context, args, 3);
	unsigned kind_id = LLVMGetMDKindID("tbaa", 4);
	LLVMSetMetadata(instr, kind_id, access_tag);
}

void set_branch_weights(KawaCompiler *c, LLVMValueRef br_instr, int true_weight,
						int false_weight) {
	LLVMValueRef weights[] = {
		LLVMMDStringInContext(c->context, "branch_weights", 14),
		LLVMConstInt(LLVMInt32TypeInContext(c->context), true_weight, 0),
		LLVMConstInt(LLVMInt32TypeInContext(c->context), false_weight, 0)};
	LLVMValueRef md_node = LLVMMDNodeInContext(c->context, weights, 3);
	unsigned kind_id = LLVMGetMDKindID("prof", 4);
	LLVMSetMetadata(br_instr, kind_id, md_node);
}

void set_fast_math(LLVMValueRef instr) {
	LLVMSetFastMathFlags(instr, LLVMFastMathAll);
}
