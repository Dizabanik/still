// Built-in reductions: sum(x), max(x), min(x), dot(a, b).
//
// These are recognized at the call site and lowered to freshly-emitted
// private functions whose loops have the canonical reduction shape LLVM's
// vectorizer wants (induction GEP + wide loads + one recurrence add), so
// they vectorize at -O1+ without pragmas. Arrays decay to slice views at
// the call site via the ordinary coercion path -- no copies.
//
// Type rules:
//   sum    -> i64 for integer elements (no mid-loop overflow), f64 for FP
//   max/min-> the element type itself
//   dot    -> i64 for integers; f64 for FP (widen once, then FMA-shaped
//             a*b+acc which the backend contracts)
#include "codegen_internal.h"

static int fp_kind_of(Type *t) {
	return t && t->kind >= TYPE_F16 && t->kind <= TYPE_F64;
}

// The call-site entry point lives in codegen_expr.c; everything here is
// the function emitter plus the type-resolution helper it shares.
//
// Build `define internal <ret> @wky.<op>(ptr data, i64 len[, ptr d2, i64 len2])`
// with the canonical reduction loop. arity is 1 or 2 (dot).
LLVMValueRef wky_emit_reduction_fn(StillCompiler *c, const char *op,
									Type *elem_ast, int arity) {
	LLVMContextRef ctx = c->context;
	TypeKind ek = elem_ast->kind;
	int is_fp = fp_kind_of(elem_ast);
	int keep_elem =
		(strcmp(op, "max") == 0 || strcmp(op, "min") == 0);

	LLVMTypeRef elem_t = get_llvm_type(c, elem_ast);
	LLVMTypeRef acc_t = keep_elem ? elem_t : LLVMInt64TypeInContext(ctx);
	if (!keep_elem && is_fp)
		acc_t = LLVMDoubleTypeInContext(ctx);

	char fname[64];
	snprintf(fname, sizeof(fname), "wky.%s.%s", op,
			 ek == TYPE_F16	 ? "f16"
			 : ek == TYPE_BF16 ? "bf16"
			 : ek == TYPE_F32  ? "f32"
			 : ek == TYPE_F64  ? "f64"
			 : ek == TYPE_I8   ? "i8"
			 : ek == TYPE_U8   ? "u8"
			 : ek == TYPE_I16  ? "i16"
			 : ek == TYPE_U16  ? "u16"
			 : ek == TYPE_I32  ? "i32"
			 : ek == TYPE_U32  ? "u32"
			 : ek == TYPE_I64  ? "i64"
							   : "u64");
	LLVMTypeRef ptr_t = LLVMPointerTypeInContext(ctx, 0);
	LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx);
	LLVMTypeRef fn_t = LLVMFunctionType(
		acc_t, (LLVMTypeRef[]){ptr_t, i64_t, ptr_t, i64_t},
		(unsigned)(arity * 2), 0);
	LLVMValueRef fn = LLVMAddFunction(c->module, fname, fn_t);
	LLVMSetLinkage(fn, LLVMInternalLinkage);

	LLVMBasicBlockRef entry = wky_append_block(fn, "entry");
	LLVMBasicBlockRef loop = wky_append_block(fn, "loop");
	LLVMBasicBlockRef body = wky_append_block(fn, "body");
	LLVMBasicBlockRef exit = wky_append_block(fn, "exit");
	LLVMPositionBuilderAtEnd(c->builder, entry);

	LLVMValueRef len0 = LLVMGetParam(fn, 1);
	LLVMValueRef trip = len0;
	if (arity == 2) {
		// dot: iterate the shorter of the two lengths.
		LLVMValueRef l2 = LLVMGetParam(fn, 3);
		trip = LLVMBuildSelect(
			c->builder,
			LLVMBuildICmp(c->builder, LLVMIntULT, len0, l2, "lt"), len0, l2,
			"trip");
	}

	// Seed. sum/dot start at zero; max/min load element 0 (an empty input
	// returns that load -- callers pass non-empty data; the load is safe
	// because the pointer is valid even for len==0 views of real arrays).
	LLVMValueRef seed;
	if (keep_elem) {
		LLVMValueRef dp0 =
			LLVMBuildBitCast(c->builder, LLVMGetParam(fn, 0), ptr_t, "d0");
		LLVMValueRef e0 =
			LLVMBuildLoad2(c->builder, elem_t, dp0, "seed");
		attach_tbaa(c, e0, elem_t);
		seed = e0;
	} else {
		seed = LLVMConstNull(acc_t);
	}
	// Empty input: skip straight to the seed without entering the loop.
	LLVMValueRef has_any =
		LLVMBuildICmp(c->builder, LLVMIntUGT, trip,
					  LLVMConstInt(i64_t, 0, 0), "has_any");
	LLVMBuildCondBr(c->builder, has_any, loop, exit);

	LLVMPositionBuilderAtEnd(c->builder, loop);
	LLVMValueRef idx = LLVMBuildPhi(c->builder, i64_t, "i");
	LLVMValueRef acc = LLVMBuildPhi(c->builder, acc_t, "acc");

	LLVMBasicBlockRef preheader = entry;
	LLVMValueRef cont =
		LLVMBuildICmp(c->builder, LLVMIntULT, idx, trip, "cont");
	LLVMBuildCondBr(c->builder, cont, body, exit);

	LLVMPositionBuilderAtEnd(c->builder, body);
	LLVMValueRef dp =
		LLVMBuildBitCast(c->builder, LLVMGetParam(fn, 0), ptr_t, "dp");
	LLVMValueRef ep = LLVMBuildGEP2(c->builder, elem_t, dp, &idx, 1, "ep");
	LLVMValueRef ev = LLVMBuildLoad2(c->builder, elem_t, ep, "e");
	attach_tbaa(c, ev, elem_t);

	LLVMValueRef next_acc;
	if (strcmp(op, "dot") == 0) {
		LLVMValueRef dp2 =
			LLVMBuildBitCast(c->builder, LLVMGetParam(fn, 2), ptr_t, "dq");
		LLVMValueRef ep2 =
			LLVMBuildGEP2(c->builder, elem_t, dp2, &idx, 1, "eq");
		LLVMValueRef ev2 = LLVMBuildLoad2(c->builder, elem_t, ep2, "f");
		attach_tbaa(c, ev2, elem_t);
		if (is_fp) {
			// Widen before multiply so precision matches the f64
			// accumulator; the backend forms extending FMAs from this.
			LLVMValueRef a =
				is_fp_kind(LLVMGetTypeKind(acc_t)) &&
									LLVMTypeOf(ev) != acc_t
					? LLVMBuildFPExt(c->builder, ev, acc_t, "wa")
					: ev;
			LLVMValueRef b = LLVMTypeOf(ev2) != acc_t
								 ? LLVMBuildFPExt(c->builder, ev2, acc_t, "wb")
								 : ev2;
			next_acc =
				LLVMBuildFAdd(c->builder, acc,
							  LLVMBuildFMul(c->builder, a, b, "prod"),
							  "red");
		} else {
			int sg = type_is_signed(c, elem_ast) &&
					 ek != TYPE_U8 && ek != TYPE_U16 && ek != TYPE_U32 &&
					 ek != TYPE_U64;
			LLVMValueRef a =
				sg ? LLVMBuildSExt(c->builder, ev,
								   LLVMInt64TypeInContext(ctx), "wa")
				   : LLVMBuildZExt(c->builder, ev,
								   LLVMInt64TypeInContext(ctx), "wa");
			LLVMValueRef b =
				sg ? LLVMBuildSExt(c->builder, ev2,
								   LLVMInt64TypeInContext(ctx), "wb")
				   : LLVMBuildZExt(c->builder, ev2,
								   LLVMInt64TypeInContext(ctx), "wb");
			next_acc = LLVMBuildAdd(
				c->builder, acc, LLVMBuildMul(c->builder, a, b, "prod"),
				"red");
		}
	} else if (strcmp(op, "sum") == 0) {
		if (is_fp) {
			LLVMValueRef w = LLVMTypeOf(ev) != acc_t
								 ? LLVMBuildFPExt(c->builder, ev, acc_t, "w")
								 : ev;
			next_acc = LLVMBuildFAdd(c->builder, acc, w, "red");
		} else {
			int sg = type_is_signed(c, elem_ast) &&
					 ek != TYPE_U8 && ek != TYPE_U16 && ek != TYPE_U32 &&
					 ek != TYPE_U64;
			LLVMValueRef w =
				sg ? LLVMBuildSExt(c->builder, ev,
								   LLVMInt64TypeInContext(ctx), "w")
				   : LLVMBuildZExt(c->builder, ev,
								   LLVMInt64TypeInContext(ctx), "w");
			next_acc = LLVMBuildAdd(c->builder, acc, w, "red");
		}
	} else {
		// max / min: strict compare against the running best, select.
		// Vectorizes to smax/umax/fmax on every ARM/x86 backend.
		LLVMValueRef cmp;
		if (is_fp) {
			cmp = LLVMBuildFCmp(
				c->builder,
				op[1] == 'a' ? LLVMRealOGT : LLVMRealOLT, ev, acc, "cmp");
		} else {
			int sg = type_is_signed(c, elem_ast) &&
					 ek != TYPE_U8 && ek != TYPE_U16 && ek != TYPE_U32 &&
					 ek != TYPE_U64;
			cmp = LLVMBuildICmp(c->builder,
								sg ? (op[1] == 'a' ? LLVMIntSGT
												   : LLVMIntSLT)
								   : (op[1] == 'a' ? LLVMIntUGT : LLVMIntULT),
								ev, acc, "cmp");
		}
		next_acc = LLVMBuildSelect(c->builder, cmp, ev, acc, "best");
	}

	LLVMBasicBlockRef body_end = LLVMGetInsertBlock(c->builder);
	LLVMValueRef idx_next =
		LLVMBuildAdd(c->builder, idx, LLVMConstInt(i64_t, 1, 0), "inext");
	LLVMBuildBr(c->builder, loop);

	// Both phi edges for the header phis: preheader (entry) and back
	// edge (body end).
	{
		LLVMValueRef zero_v = LLVMConstInt(i64_t, 0, 0);
		LLVMAddIncoming(idx, &zero_v, &preheader, 1);
	}
	LLVMAddIncoming(acc, &seed, &preheader, 1);
	LLVMAddIncoming(idx, &idx_next, &body_end, 1);
	LLVMAddIncoming(acc, &next_acc, &body_end, 1);

	LLVMPositionBuilderAtEnd(c->builder, exit);
	// Result merge over exit's two predecessors: entry (empty input ->
	// seed) and loop (condition false -> acc).
	LLVMValueRef result = LLVMBuildPhi(c->builder, acc_t, "res");
	LLVMAddIncoming(result, &seed, &entry, 1);
	LLVMAddIncoming(result, &acc, &loop, 1);
	LLVMBuildRet(c->builder, result);
	return fn;
}

// Saturating arithmetic (IDEAS 1.2): qadd/qsub/qmul for the narrow int
// types (i8/u8/i16/u16). Computed in the widened i32 domain and clamped
// with two branchless selects -- the same shape LLVM expands the
// llvm.*.sat intrinsics into on arm64/x86 for scalar operands.
LLVMValueRef wky_build_sat_op(StillCompiler *c, const char *op,
							   Type *elem_ast, LLVMValueRef l, LLVMValueRef r) {
	TypeKind k = elem_ast->kind;
	if (k != TYPE_I8 && k != TYPE_U8 && k != TYPE_I16 && k != TYPE_U16)
		return NULL;
	const char *body =
		strcmp(op, "qadd") == 0 ? "add"
		: strcmp(op, "qsub") == 0 ? "sub"
		: strcmp(op, "qmul") == 0 ? "mul"
		: NULL;
	if (!body)
		return NULL;

	LLVMContextRef ctx = c->context;
	LLVMTypeRef t = get_llvm_type(c, elem_ast);
	LLVMTypeRef w_t = LLVMInt32TypeInContext(ctx);
	int sg = (k == TYPE_I8 || k == TYPE_I16);
	unsigned bits = LLVMGetIntTypeWidth(t);
	long long lo = sg ? -(1LL << (bits - 1)) : 0;
	long long hi = sg ? (1LL << (bits - 1)) - 1
					  : (1LL << bits) - 1;
	(void)lo;

	// Widen both operands (sign- or zero-extend), compute in i32.
	LLVMValueRef wl =
		sg ? LLVMBuildSExt(c->builder, l, w_t, "qw_l")
		   : LLVMBuildZExt(c->builder, l, w_t, "qw_l");
	LLVMValueRef wr =
		sg ? LLVMBuildSExt(c->builder, r, w_t, "qw_r")
		   : LLVMBuildZExt(c->builder, r, w_t, "qw_r");
	LLVMValueRef wide;
	if (body[0] == 'a')
		wide = sg ? LLVMBuildNSWAdd(c->builder, wl, wr, "qwide")
				  : LLVMBuildAdd(c->builder, wl, wr, "qwide");
	else if (body[0] == 's')
		wide = sg ? LLVMBuildNSWSub(c->builder, wl, wr, "qwide")
				  : LLVMBuildSub(c->builder, wl, wr, "qwide");
	else
		wide = sg ? LLVMBuildNSWMul(c->builder, wl, wr, "qwide")
				  : LLVMBuildMul(c->builder, wl, wr, "qwide");

	// Clamp to [lo, hi] in the widened domain; truncation is then exact.
	LLVMValueRef lo_v = LLVMConstInt(w_t, (unsigned long long)lo, sg);
	LLVMValueRef hi_v = LLVMConstInt(w_t, (unsigned long long)hi, sg);
	LLVMValueRef c1 =
		sg ? LLVMBuildICmp(c->builder, LLVMIntSLT, wide, lo_v, "qc1")
		   : LLVMBuildICmp(c->builder, LLVMIntULT, wide, lo_v, "qc1");
	LLVMValueRef clamped_lo =
		LLVMBuildSelect(c->builder, c1, lo_v, wide, "qcl");
	LLVMValueRef c2 =
		sg ? LLVMBuildICmp(c->builder, LLVMIntSGT, clamped_lo, hi_v, "qc2")
		   : LLVMBuildICmp(c->builder, LLVMIntUGT, clamped_lo, hi_v, "qc2");
	LLVMValueRef clamped =
		LLVMBuildSelect(c->builder, c2, hi_v, clamped_lo, "qclamp");
	return sg ? LLVMBuildTrunc(c->builder, clamped, t, "qret")
			  : LLVMBuildTrunc(c->builder, clamped, t, "qret");
}
