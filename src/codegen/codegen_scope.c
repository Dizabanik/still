#include "codegen_internal.h"

// Convert a Whisky identifier (which uses `__` as a path separator inside
// synthesized names) to a dotted path for display. The buffer is arena-
// allocated; the program only calls this from error paths so the lifetime
// is fine.
char *get_var_path(StillCompiler *c, const char *s) {
	if (!s)
		return NULL;
	size_t len = strlen(s);
	char *out = arena_alloc(c->arena, len + 1);
	size_t i = 0, j = 0;
	while (i < len) {
		if (i + 1 < len && s[i] == '_' && s[i + 1] == '_') {
			int at_start = (i == 0);
			int at_end = (i + 2 == len);
			if (!at_start && !at_end) {
				out[j++] = '.';
				i += 2;
				continue;
			}
		}
		out[j++] = s[i++];
	}
	out[j] = '\0';
	return out;
}

const char *resolve_type_name(StillCompiler *c, ASTNode *n) {
	if (n->type == NODE_VAR_REF) {
		Scope *s = scope_find(c, n->data.var_ref.name);
		if (s && s->node && s->node->data_type) {
			Type *t = s->node->data_type;
			while ((t->kind == TYPE_PTR || t->kind == TYPE_AMP) && t->inner)
				t = t->inner;
			if (t->name)
				return t->name;
		}
	}
	if (n->type == NODE_MEMBER_ACCESS)
		return n->data.member_access.member;
	if (n->type == NODE_DEREF)
		return resolve_type_name(c, n->data.deref.expr);
	return "unknown";
}

// Allocate an alloca in the entry block of the current function. Doing the
// alloca at the top of the function (instead of at the use site) is what
// lets mem2reg actually promote it to a register -- so this is a real
// optimization, not a stylistic choice.
LLVMValueRef create_entry_block_alloca(StillCompiler *c, LLVMTypeRef type,
									   const char *name) {
	LLVMBasicBlockRef current_block = LLVMGetInsertBlock(c->builder);
	LLVMBasicBlockRef entry_block = LLVMGetEntryBasicBlock(c->current_func);
	LLVMValueRef first_instr = LLVMGetFirstInstruction(entry_block);
	if (first_instr)
		LLVMPositionBuilderBefore(c->builder, first_instr);
	else
		LLVMPositionBuilderAtEnd(c->builder, entry_block);
	LLVMValueRef alloca_instr = LLVMBuildAlloca(c->builder, type, name);
	LLVMSetAlignment(alloca_instr, 16);
	LLVMPositionBuilderAtEnd(c->builder, current_block);
	return alloca_instr;
}

// --- Constant global initializers ---------------------------------------
// A global initializer is "constant" when it can be evaluated at compile
// time into an LLVM Constant: literals, arrays/structs of constants, and
// simple arithmetic over those. Anything else (calls, var refs, coroutines)
// forces a runtime init in wky_globals_init.
static LLVMValueRef const_eval_expr(StillCompiler *c, ASTNode *n,
									LLVMTypeRef dst);

// Integer literals are i32 until coerced; a binop node's data_type tells us
// how the parser typed it. `is_signed` picks sign-extension vs zero-fill and
// signed vs unsigned division/remainder -- mirroring build_int_binop().
static int init_is_signed(StillCompiler *c, ASTNode *n) {
	return n->data_type ? type_is_signed(c, n->data_type) : 0;
}

int global_init_is_constant(StillCompiler *c, ASTNode *n) {
	if (!n)
		return 1;
	switch (n->type) {
	case NODE_LITERAL:
	case NODE_STRING_LIT:
		return 1;
	case NODE_VAR_REF: {
		// Consts (incl. enum members, which desugar to consts) are
		// compile-time values when their initializer is one. The parser's
		// decl table already validated forward refs.
		Scope *sv = scope_find(c, n->data.var_ref.name);
		return sv && sv->node && sv->node->type == NODE_VAR_DECL &&
			   sv->node->data.var_decl.is_const &&
			   global_init_is_constant(c,
									   sv->node->data.var_decl.init);
	}
	case NODE_BINARY_OP:
		return global_init_is_constant(c, n->data.bin_op.left) &&
			   global_init_is_constant(c, n->data.bin_op.right);
	case NODE_CAST:
		return global_init_is_constant(c,n->data.cast.val);
	case NODE_TERNARY:
		return global_init_is_constant(c,n->data.ternary.cond) &&
			global_init_is_constant(c,n->data.ternary.then_expr) &&
			global_init_is_constant(c,n->data.ternary.else_expr);
	case NODE_STRUCT_LITERAL: {
		wky_literal_context(c,n,n->data_type);
		Type *actual=wky_resolve_type(c,n->data_type);
		StructDef *sd=actual->kind==TYPE_STRUCT ? find_struct_def_pub(c,get_llvm_type(c,actual)) : NULL;
		LiteralPlan plan={0};
		if (actual->kind==TYPE_STRUCT) plan=wky_literal_plan(c,n,get_llvm_type(c,actual));
		unsigned at=0;
		for (StructInitItem *it = n->data.struct_lit.items; it; it = it->next,++at) {
			Type *element=actual->kind==TYPE_ARRAY ? actual->inner :
				(sd && !it->spread_from ? sd->fields[plan.indices[at]].ast_type : actual);
			wky_literal_context(c,it->value,element);
			wky_literal_context(c,it->spread_from,actual);
			if (!global_init_is_constant(c, it->value) || !global_init_is_constant(c,it->spread_from))
				return 0;
		}
		if (!plan.spread) {
			for (int i=0; sd && i<sd->field_count; ++i)
				if (!(plan.provided & (UINT64_C(1)<<i)) && !global_init_is_constant(c,sd->fields[i].default_expr)) return 0;
		}
		return 1;
	}
	case NODE_CALL:
		// Comptime-callable when the evaluator can run it (pure fn over
		// integer constants). The eval itself is the authority.
		return wky_comptime_eval(c, n, 64, NULL) != NULL;
	case NODE_MEMBER_ACCESS: {
		if (n->data.member_access.object->type == NODE_VAR_REF) {
			char mangled[256];
			snprintf(mangled, sizeof(mangled), "%s__%s",
					 n->data.member_access.object->data.var_ref.name,
					 n->data.member_access.member);
			Scope *asv = scope_find(c, mangled);
			if (asv && asv->node && asv->node->type == NODE_VAR_DECL &&
				asv->node->data.var_decl.is_const)
				return global_init_is_constant(c, asv->node->data.var_decl.init);
		}
		return 0;
	}
	default:
		(void)c;
		return 0;
	}
}

// Rebuild an integer constant at `dst`'s width from its raw value, keeping
// the bit pattern for same/narrower widths and re-signing only when growing.
// Replaces the missing LLVMConstTrunc/SExt/ZExt trio with one uniform path.
static LLVMValueRef const_int_as(StillCompiler *c, LLVMValueRef v,
								 LLVMTypeRef dst, int is_signed) {
	LLVMTypeRef src = LLVMTypeOf(v);
	if (src == dst || LLVMGetTypeKind(src) != LLVMIntegerTypeKind ||
		LLVMGetTypeKind(dst) != LLVMIntegerTypeKind)
		return v;
	unsigned sw = LLVMGetIntTypeWidth(src), dw = LLVMGetIntTypeWidth(dst);
	unsigned long long raw = is_signed
								 ? (unsigned long long)LLVMConstIntGetSExtValue(v)
								 : LLVMConstIntGetZExtValue(v);
	if (dw < sw) {
		// Truncation keeps the low bits regardless of signedness.
		raw &= (dw >= 64) ? ~0ULL : ((1ULL << dw) - 1ULL);
		is_signed = 0;
	} else if (sw < dw && !is_signed) {
		// Zero-extended values must not carry garbage in the high bits.
		raw &= (sw >= 64) ? ~0ULL : ((1ULL << sw) - 1ULL);
	}
	return LLVMConstInt(dst, raw, is_signed);
}

// Fold one arithmetic op over two already-extracted operands held at the
// fold width in 64-bit two's complement. Add/sub/mul produce identical bits
// either way; division follows build_int_binop(): signed only when BOTH
// sides are signed. Division/modulo by zero leaves the op unfolded rather
// than crashing the compiler.
int fold_int_binop(int tok, unsigned long long a, unsigned long long b,
						  int lhs_signed, int rhs_signed,
						  unsigned width,
						  unsigned long long *out) {
	uint64_t mask=width==64 ? UINT64_MAX : (UINT64_C(1)<<width)-1;
	a &= mask; b &= mask;
	int div_signed = lhs_signed || rhs_signed;
	__int128 x=(a & (UINT64_C(1)<<(width-1))) ? (__int128)a-((__int128)1<<width) : a;
	__int128 y=(b & (UINT64_C(1)<<(width-1))) ? (__int128)b-((__int128)1<<width) : b;
	if (div_signed && (tok == TOK_PLUS || tok == TOK_MINUS || tok == TOK_STAR)) {
		__int128 value = tok == TOK_PLUS ? x + y : tok == TOK_MINUS ? x - y : x * y;
		__int128 bound = (__int128)1 << (width - 1);
		if (value < -bound || value >= bound) return 0;
		*out = (unsigned long long)value;
		return 1;
	}
	switch (tok) {
	case TOK_PLUS:
		*out = a + b;
		return 1;
	case TOK_MINUS:
		*out = a - b;
		return 1;
	case TOK_STAR:
		*out = a * b;
		return 1;
	case TOK_SLASH:
		if (b == 0 || (div_signed && a == (1ULL << (width-1)) && b == mask))
			return 0;
		*out = div_signed ? (unsigned long long)(x / y)
						  : a / b;
		return 1;
	case TOK_PERCENT:
		if (b == 0 || (div_signed && a == (1ULL << (width-1)) && b == mask))
			return 0;
		*out = div_signed ? (unsigned long long)(x % y)
						  : a % b;
		return 1;
	case TOK_AMP: *out=a&b; return 1;
	case TOK_PIPE: *out=a|b; return 1;
	case TOK_CARET: *out=a^b; return 1;
	case TOK_SHL: if (b>=width) return 0; *out=a<<b; return 1;
	case TOK_SHR: if (b>=width) return 0; *out=lhs_signed ? (uint64_t)(x>>b) : a>>b; return 1;
	case TOK_ISEQ: *out=a==b; return 1;
	case TOK_NOTEQ: *out=a!=b; return 1;
	case TOK_LANGLE: *out=div_signed ? x<y : a<b; return 1;
	case TOK_RANGLE: *out=div_signed ? x>y : a>b; return 1;
	case TOK_LEQ: *out=div_signed ? x<=y : a<=b; return 1;
	case TOK_REQ: *out=div_signed ? x>=y : a>=b; return 1;
	default:
		return 0;
	}
}

// Evaluate a compile-time initializer expression to an LLVM Constant of the
// requested type. Only called after global_init_is_constant() returned true.
LLVMValueRef const_eval_global_init(StillCompiler *c, ASTNode *n,
									LLVMTypeRef dst, Type *dst_ast) {
	if (n->type == NODE_STRING_LIT) {
		// One terminated storage object; lengths always count decoded bytes.
		LLVMValueRef bytes = LLVMConstStringInContext(c->context,
			n->data.str_lit.s_val, (unsigned)n->data.str_lit.len, 0);
		LLVMValueRef storage = LLVMAddGlobal(c->module, LLVMTypeOf(bytes), ".gstr");
		LLVMSetInitializer(storage, bytes);
		LLVMSetGlobalConstant(storage, 1);
		LLVMSetLinkage(storage, LLVMPrivateLinkage);
		LLVMSetUnnamedAddress(storage, LLVMGlobalUnnamedAddr);
		if (LLVMGetTypeKind(dst) == LLVMPointerTypeKind) return storage;
		if (dst_ast && dst_ast->kind == TYPE_SLICE) {
			LLVMValueRef len = LLVMConstInt(LLVMInt64TypeInContext(c->context), n->data.str_lit.len, 0);
			return LLVMConstNamedStruct(dst, (LLVMValueRef[]){storage, len}, 2);
		}
	}
	LLVMValueRef v = const_eval_expr(c, n, dst);
	if (!v) return NULL;

	// Integer literal -> FP destination (e.g. `f64 x = 5;`): the folder
	// emits i32 for int literals regardless of the destination kind.
	if (n->type == NODE_LITERAL && n->data_type &&
		n->data_type->kind != TYPE_F16 && n->data_type->kind != TYPE_BF16 &&
		n->data_type->kind != TYPE_F32 && n->data_type->kind != TYPE_F64 &&
		is_fp_kind(LLVMGetTypeKind(dst))) {
		double d = init_is_signed(c, n) ? (double)LLVMConstIntGetSExtValue(v)
										: (double)LLVMConstIntGetZExtValue(v);
		return LLVMConstReal(dst, d);
	}
	// FP -> FP width change (any of f16/bf16/f32 <-> f64): no
	// ConstFPTrunc/Ext in this API, so round-trip through the double value
	// and re-emit at `dst`.
	if (is_fp_kind(LLVMGetTypeKind(LLVMTypeOf(v))) &&
		is_fp_kind(LLVMGetTypeKind(dst)) && LLVMTypeOf(v) != dst)
		return LLVMConstReal(
			dst, LLVMConstRealGetDouble(v, &(LLVMBool){0}));

	if (n->line>0) c->source_line=n->line;
	wky_check_conversion(c, v, n->data_type, dst, dst_ast);
	if (is_fp_kind(LLVMGetTypeKind(LLVMTypeOf(v))) && LLVMGetTypeKind(dst) == LLVMIntegerTypeKind) {
		double value = LLVMConstRealGetDouble(v, &(LLVMBool){0});
		unsigned long long integer = LLVMGetIntTypeWidth(dst) == 1 ? value != 0.0 :
			type_is_signed(c, dst_ast) ? (unsigned long long)(long long)value : (unsigned long long)value;
		return LLVMConstInt(dst, integer, 0);
	}
	v = const_int_as(c, v, dst, init_is_signed(c, n));
	return v;
}

static LLVMValueRef const_eval_expr(StillCompiler *c, ASTNode *n,
									LLVMTypeRef dst) {
	switch (n->type) {
	case NODE_LITERAL:
		if (n->data_type && (n->data_type->kind == TYPE_F16 ||
							 n->data_type->kind == TYPE_BF16 ||
							 n->data_type->kind == TYPE_F32 ||
							 n->data_type->kind == TYPE_F64))
			return LLVMConstReal(get_llvm_type(c, n->data_type),
								 (double)n->data.literal.f_val);
		// Match codegen_expr: emit at the literal's own typed width with
		// i64_val authoritative; const_int_as re-homes to `dst` after.
		{
			unsigned lit_w = 32;
			if (n->data_type && (n->data_type->kind == TYPE_I64 ||
								 n->data_type->kind == TYPE_U64))
				lit_w = 64;
			return LLVMConstInt(
				LLVMIntTypeInContext(c->context, lit_w),
				(unsigned long long)n->data.literal.i64_val,
				n->data_type ? type_is_signed(c, n->data_type) : 0);
		}
	case NODE_VAR_REF: {
		Scope *sv = scope_find(c, n->data.var_ref.name);
		if (!sv || !sv->node || sv->node->type != NODE_VAR_DECL ||
			!sv->node->data.var_decl.is_const)
			return NULL;
		return const_eval_expr(c, sv->node->data.var_decl.init, dst);
	}
	case NODE_BINARY_OP: {
		LLVMValueRef integer=wky_comptime_eval(c,n,0,NULL);
		if (integer) return integer;
		// Fold manually: LLVM 21's C API has no ConstBinOp/Mul/Div family.
		// GetSExtValue/GetZExtValue already return the value carried to
		// 64 bits with the right sign, so operands fold directly; the mask
		// below re-homes the result to the wider operand's width.
		LLVMValueRef l = const_eval_expr(c, n->data.bin_op.left, NULL);
		LLVMValueRef r = const_eval_expr(c, n->data.bin_op.right, NULL);
		if (!l || !r)
			return NULL;
		if (is_fp_kind(LLVMGetTypeKind(LLVMTypeOf(l))) || is_fp_kind(LLVMGetTypeKind(LLVMTypeOf(r)))) {
			int op=n->data.bin_op.op;
			// IRBuilder folds constant FP operations with target precision,
			// preserving intermediate rounding and signed zero.
			LLVMTypeRef type=is_fp_kind(LLVMGetTypeKind(LLVMTypeOf(l))) ? LLVMTypeOf(l) : LLVMTypeOf(r);
			if (is_fp_kind(LLVMGetTypeKind(LLVMTypeOf(l))) && is_fp_kind(LLVMGetTypeKind(LLVMTypeOf(r)))) {
				if (LLVMGetTypeKind(LLVMTypeOf(l))==LLVMDoubleTypeKind || LLVMGetTypeKind(LLVMTypeOf(r))==LLVMDoubleTypeKind) type=LLVMDoubleTypeInContext(c->context);
				else if (LLVMTypeOf(l)!=LLVMTypeOf(r)) type=LLVMFloatTypeInContext(c->context);
			}
			l=coerce_value(c,l,n->data.bin_op.left->data_type,type,n->data_type);
			r=coerce_value(c,r,n->data.bin_op.right->data_type,type,n->data_type);
			if (n->data.bin_op.unary_negation) return LLVMBuildFNeg(c->builder,r,"constant_negate");
			switch(op) {
			case TOK_PLUS: return LLVMBuildFAdd(c->builder,l,r,"");
			case TOK_MINUS: return LLVMBuildFSub(c->builder,l,r,"");
			case TOK_STAR: return LLVMBuildFMul(c->builder,l,r,"");
			case TOK_SLASH: return LLVMBuildFDiv(c->builder,l,r,"");
			case TOK_ISEQ: return LLVMBuildFCmp(c->builder,LLVMRealOEQ,l,r,"");
			case TOK_NOTEQ: return LLVMBuildFCmp(c->builder,LLVMRealUNE,l,r,"");
			case TOK_LANGLE: return LLVMBuildFCmp(c->builder,LLVMRealOLT,l,r,"");
			case TOK_RANGLE: return LLVMBuildFCmp(c->builder,LLVMRealOGT,l,r,"");
			case TOK_LEQ: return LLVMBuildFCmp(c->builder,LLVMRealOLE,l,r,"");
			case TOK_REQ: return LLVMBuildFCmp(c->builder,LLVMRealOGE,l,r,"");
			default: return NULL;
			}
		}
		if (LLVMGetTypeKind(LLVMTypeOf(l)) != LLVMIntegerTypeKind || LLVMGetTypeKind(LLVMTypeOf(r)) != LLVMIntegerTypeKind) return NULL;
		int l_s = init_is_signed(c, n->data.bin_op.left);
		int r_s = init_is_signed(c, n->data.bin_op.right);
		unsigned lw=LLVMGetIntTypeWidth(LLVMTypeOf(l)), rw0=LLVMGetIntTypeWidth(LLVMTypeOf(r));
		unsigned long long a =
			l_s ? (unsigned long long)LLVMConstIntGetSExtValue(l)
				: LLVMConstIntGetZExtValue(l);
		unsigned long long b =
			r_s ? (unsigned long long)LLVMConstIntGetSExtValue(r)
				: LLVMConstIntGetZExtValue(r);
		if (l_s && !r_s && rw0>=lw) l_s=0;
		else if (!l_s && r_s && lw>=rw0) r_s=0;
		unsigned w = LLVMGetIntTypeWidth(LLVMTypeOf(l));
		unsigned rw = LLVMGetIntTypeWidth(LLVMTypeOf(r));
		if (rw > w)
			w = rw;
		unsigned long long res;
		if (!fold_int_binop(n->data.bin_op.op, a, b, l_s, r_s, w, &res))
			return NULL;
		res &= (w >= 64) ? ~0ULL : ((1ULL << w) - 1ULL);
		int op=n->data.bin_op.op;
		if (op==TOK_ISEQ || op==TOK_NOTEQ || op==TOK_LANGLE || op==TOK_RANGLE || op==TOK_LEQ || op==TOK_REQ) w=1;
		return LLVMConstInt(LLVMIntTypeInContext(c->context, w), res,
							l_s || r_s);
	}
	case NODE_STRUCT_LITERAL: {
		wky_literal_context(c,n,n->data_type);
		Type *actual=wky_resolve_type(c,n->data_type);
		LLVMTypeRef s_type =
			get_llvm_type(c, n->data_type ? n->data_type : NULL);
		if (!s_type)
			return NULL;
		if (LLVMGetTypeKind(s_type) == LLVMArrayTypeKind) {
			LLVMTypeRef elem_t = LLVMGetElementType(s_type);
			unsigned len = LLVMGetArrayLength(s_type);
			LLVMValueRef *elems =
				arena_alloc(c->arena, sizeof(LLVMValueRef) * (len ? len : 1));
			unsigned i = 0;
			for (StructInitItem *it = n->data.struct_lit.items; it;
				 i++, it = it->next) {
				if (i>=len) { still_error(STILL_E_ARITY,n,"too many elements in array initializer"); exit(1); }
				if (it->field_name || it->spread_from) { still_error(STILL_E_ARGS,n,"array initializers require positional elements"); exit(1); }
				wky_literal_context(c,it->value,actual->inner);
				elems[i] = const_eval_global_init(c, it->value, elem_t, actual->inner);
				if (!elems[i]) return NULL;
			}
			for (; i < len; i++)
				elems[i] = LLVMConstNull(elem_t);
			return LLVMConstArray(elem_t, elems, len);
		}
		if (LLVMGetTypeKind(s_type) != LLVMStructTypeKind)
			return NULL;
		unsigned fc = LLVMCountStructElementTypes(s_type);
		LLVMValueRef *fields =
			arena_alloc(c->arena, sizeof(LLVMValueRef) * (fc ? fc : 1));
		StructDef *sd=find_struct_def_pub(c,s_type);
		LiteralPlan plan=wky_literal_plan(c,n,s_type);
		for (unsigned i=0; i<fc; ++i) {
			fields[i]=sd && !plan.spread && !(plan.provided & (UINT64_C(1)<<i)) && sd->fields[i].default_expr ? const_eval_global_init(c,sd->fields[i].default_expr,
				LLVMStructGetTypeAtIndex(s_type,i),sd->fields[i].ast_type) : LLVMConstNull(LLVMStructGetTypeAtIndex(s_type,i));
			if (!fields[i]) return NULL;
		}
		for (StructInitItem *it=n->data.struct_lit.items; it; it=it->next) {
			if (!it->spread_from) continue;
			LLVMValueRef base=const_eval_expr(c,it->spread_from,s_type);
			if (!base || LLVMTypeOf(base)!=s_type) return NULL;
			for (unsigned i=0; i<fc; ++i) fields[i]=LLVMBuildExtractValue(c->builder,base,i,"");
		}
		unsigned at=0;
		for (StructInitItem *it=n->data.struct_lit.items; it; it=it->next,++at) {
			if (it->spread_from) continue;
			unsigned i=plan.indices[at];
			wky_literal_context(c,it->value,sd ? sd->fields[i].ast_type : NULL);
			fields[i]=const_eval_global_init(c,it->value,LLVMStructGetTypeAtIndex(s_type,i),sd ? sd->fields[i].ast_type : NULL);
			if (!fields[i]) return NULL;
		}
		return LLVMConstNamedStruct(s_type, fields, fc);
	}
	case NODE_CALL: {
		// Comptime (IDEAS 2.1): a call the tree-walking evaluator can run
		// (pure fn over integer constants) folds right here -- no IR is
		// generated for the callee when every use folds away.
		LLVMValueRef folded = wky_comptime_eval(c, n, 0, NULL);
		if (folded)
			return folded;
		return NULL;
	}
	case NODE_CAST: {
		LLVMValueRef value=const_eval_expr(c,n->data.cast.val,NULL);
		return value ? coerce_value(c,value,wky_expr_type(c,n->data.cast.val),get_llvm_type(c,n->data_type),n->data_type) : NULL;
	}
	case NODE_TERNARY: return wky_comptime_eval(c,n,0,NULL);
	case NODE_MEMBER_ACCESS: {
		if (n->data.member_access.object->type == NODE_VAR_REF) {
			char mangled[256];
			snprintf(mangled, sizeof(mangled), "%s__%s",
					 n->data.member_access.object->data.var_ref.name,
					 n->data.member_access.member);
			Scope *asv = scope_find(c, mangled);
			if (asv && asv->node && asv->node->type == NODE_VAR_DECL &&
				asv->node->data.var_decl.is_const)
				return const_eval_expr(c, asv->node->data.var_decl.init, dst);
		}
		return NULL;
	}
	default:
		return NULL;
	}
}

// --- end constant initializer helpers ------------------------------------

// Compile-time integer fold for case labels: literals, consts (enum members
// are consts), and const arithmetic. Sign comes from the expression itself.
int const_eval_i64(StillCompiler *c, ASTNode *n, long long *out) {
	if (!global_init_is_constant(c, n))
		return 0;
	LLVMValueRef v = const_eval_expr(c, n, NULL);
	if (!v || LLVMGetTypeKind(LLVMTypeOf(v)) != LLVMIntegerTypeKind)
		return 0;
	if (LLVMIsConstant(v) &&
		init_is_signed(c, n))
		*out = LLVMConstIntGetSExtValue(v);
	else
		*out = (long long)LLVMConstIntGetZExtValue(v);
	return 1;
}

void scope_push(StillCompiler *c, const char *name, LLVMValueRef val,
				LLVMTypeRef type, ASTNode *node) {
	Scope *s = arena_alloc(c->arena, sizeof(Scope));
	s->name = arena_strdup(c->arena, name);
	s->val = val;
	s->type = type;
	s->node = node;
	s->used = 0;
	s->all_next = c->function_locals;
	c->function_locals = s;
	s->next = c->scope_stack;
	c->scope_stack = s;
}

Scope *scope_find(StillCompiler *c, const char *name) {
	for (Scope *cur = c->scope_stack; cur; cur = cur->next) {
		if (strcmp(cur->name, name) == 0) {
			cur->used = 1;
			return cur;
		}
	}
	return NULL;
}

// Best-effort AST type for the VALUE obtained by dereferencing `n`.
// Priority:
//   1. The deref node's own data_type (set by the parser for typed exprs).
//   2. The declared type of the referenced variable (peel one pointer).
//   3. Nested derefs: the value being dereferenced is itself a deref
//		result, so recurse and peel one more pointer level.
static Type *deref_value_type(StillCompiler *c, ASTNode *n) {
	(void)c;
	if (n->data_type)
		return n->data_type;
	ASTNode *expr = n->data.deref.expr;
	Type *t = NULL;
	if (expr->type == NODE_VAR_REF) {
		Scope *s = scope_find(c, expr->data.var_ref.name);
		t = (s && s->node) ? s->node->data_type : NULL;
	} else if (expr->type == NODE_DEREF) {
		// Value of `expr` is the result of the inner deref.
		t = deref_value_type(c, expr);
	} else if (expr->data_type) {
		t = expr->data_type;
	}
	// `t` is the type of the pointer value being dereferenced; the result
	// is its pointee.
	if (t && (t->kind == TYPE_PTR || t->kind == TYPE_AMP))
		return t->inner;
	return NULL;
}

// Resolve the LLVM address of a (possibly nested) lvalue. Used by both
// value_of_lvalue (for loads) and codegen_stmt (for stores).
//
// The invariant: the returned value is always a POINTER to the storage, and
// *out_type (when requested) is the type of the value stored there. Deref
// nodes therefore return the loaded pointer itself (the address of the
// pointee), which makes chains like `(*p)->field` and `**pp` resolve
// uniformly: each deref level contributes exactly one load of a pointer.
// Lazily DEFINE the module-wide trap: `wky_trap(msg, file, line)` writes a
// diagnostic to stderr and aborts. Defined into the module itself (not
// linked from a runtime lib) so generated programs stay self-contained.
// Modules that never trap carry no definition at all.
//
// The body uses only portable libc: snprintf into a stack buffer, then
// write(2) to fd 2 -- avoids FILE*/stderr plumbing that differs per libc.
LLVMValueRef get_or_declare_trap_fn(StillCompiler *c) {
	if (c->trap_fn)
		return c->trap_fn;
	LLVMContextRef ctx = c->context;
	LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
	LLVMTypeRef i32t = LLVMInt32TypeInContext(ctx);
	LLVMTypeRef i64t = LLVMInt64TypeInContext(ctx);
	LLVMTypeRef fn_t = LLVMFunctionType(LLVMVoidTypeInContext(ctx),
										(LLVMTypeRef[]){i8ptr, i8ptr, i32t},
										3, 0);
	LLVMValueRef fn = LLVMAddFunction(c->module, "wky_trap", fn_t);
	const char *noreturn = "noreturn";
	LLVMAddAttributeAtIndex(
		fn, LLVMAttributeFunctionIndex,
		LLVMCreateEnumAttribute(
			ctx,
			LLVMGetEnumAttributeKindForName(noreturn, strlen(noreturn)),
			0));
	c->trap_fn = fn;

	// Saved builder/function position -- this runs mid-emission elsewhere.
	LLVMBasicBlockRef saved_bb = LLVMGetInsertBlock(c->builder);
	LLVMValueRef saved_func = c->current_func;

	LLVMBasicBlockRef bb = wky_append_block(fn, "entry");
	LLVMPositionBuilderAtEnd(c->builder, bb);

	// declare i32 @snprintf(ptr, i64, ptr, ...)
	LLVMTypeRef snprintf_t =
		LLVMFunctionType(i32t, (LLVMTypeRef[]){i8ptr, i64t, i8ptr}, 3, 1);
	LLVMValueRef snprintf_fn = LLVMGetNamedFunction(c->module, "snprintf");
	if (!snprintf_fn)
		snprintf_fn = LLVMAddFunction(c->module, "snprintf", snprintf_t);
	// declare i32 @write(i32, ptr, i64)
	LLVMTypeRef write_t = LLVMFunctionType(
		i64t, (LLVMTypeRef[]){i32t, i8ptr, i64t}, 3, 0);
	LLVMValueRef write_fn = LLVMGetNamedFunction(c->module, "write");
	if (!write_fn)
		write_fn = LLVMAddFunction(c->module, "write", write_t);
	// declare void @abort()
	LLVMTypeRef abort_t = LLVMFunctionType(LLVMVoidTypeInContext(ctx), NULL, 0, 0);
	LLVMValueRef abort_fn = LLVMGetNamedFunction(c->module, "abort");
	if (!abort_fn)
		abort_fn = LLVMAddFunction(c->module, "abort", abort_t);

	// char buf[512];
	LLVMValueRef buf = LLVMBuildArrayAlloca(
		c->builder, LLVMInt8TypeInContext(ctx),
		LLVMConstInt(i64t, 512, 0), "trap_buf");

	// n = snprintf(buf, 512, "Whisky: trap: %s at %s:%d\n", msg, file, line);
	// Params: msg=0, file=1, line=2.
	LLVMValueRef fmt = LLVMBuildGlobalStringPtr(
		c->builder, "Whisky: trap: %s at %s:%d\n", "trap_fmt");
	LLVMValueRef snargs[6] = {
		buf, LLVMConstInt(i64t, 512, 0), fmt,
		LLVMGetParam(fn, 0), // msg
		LLVMGetParam(fn, 1), // file
		LLVMGetParam(fn, 2)  // line (i32 vararg promotes itself)
	};
	LLVMValueRef n = LLVMBuildCall2(c->builder, snprintf_t, snprintf_fn,
									snargs, 6, "n");

	// write(2, buf, n);
	LLVMValueRef wargs[3] = {LLVMConstInt(i32t, 2, 0), buf,
							 LLVMBuildSExt(c->builder, n, i64t, "n64")};
	LLVMBuildCall2(c->builder, write_t, write_fn, wargs, 3, "");

	LLVMBuildCall2(c->builder, abort_t, abort_fn, NULL, 0, "");
	LLVMBuildUnreachable(c->builder);

	c->current_func = saved_func;
	if (saved_bb)
		LLVMPositionBuilderAtEnd(c->builder, saved_bb);
	return fn;
}

// Array bounds checks reject both negative and oversized indices. LLVM
// removes checks when it can prove the access safe.
static void emit_bounds_check(StillCompiler *c, ASTNode *idx_node,
							  LLVMValueRef idx_i64,
							  long long array_len, const char *file,
							  int line) {
	// Policy (IDEAS 4.3):
	// - bounds_check_mode == -1: never emit checks
	// - bounds_check_mode == 1: always emit checks (no elision)
	// - default/safe: omit proven constant checks; LLVM handles loop proofs.
	int check_enabled = (c->bounds_check_mode != -1);
	if (!check_enabled || c->unchecked_depth > 0)
		return;

	// Optimization & Redundancy Elimination (IDEAS 4.3):
	if (c->bounds_check_mode != 1) {
		// 1. Constant index folding:
		if (LLVMIsAConstantInt(idx_i64)) {
			unsigned long long cval = LLVMConstIntGetZExtValue(idx_i64);
            if (cval < (unsigned long long)array_len) {
                // Statically proven safe!
                still_report_site(c,idx_node,"guard","index out of bounds",
                    "Lowering proved the constant index is within the fixed array bound.");
                return;
			}
		}

		// LLVM proves induction ranges after mem2reg. Name-only range facts
		// are invalid when the body mutates or aliases the iterator.

	}

	LLVMContextRef ctx = c->context;
	LLVMValueRef len_const =
		LLVMConstInt(LLVMInt64TypeInContext(ctx), (unsigned long long)array_len, 0);
	LLVMValueRef ok = LLVMBuildICmp(c->builder, LLVMIntULT, idx_i64,
									len_const, "bounds_ok");
	LLVMBasicBlockRef cont_bb =
		wky_append_block(c->current_func, "idx_in_bounds");
	LLVMBasicBlockRef trap_bb =
		wky_append_block(c->current_func, "idx_oob");
    LLVMValueRef branch=LLVMBuildCondBr(c->builder,ok,cont_bb,trap_bb);
    LLVMMetadataRef site=still_report_site(c,idx_node,"guard","index out of bounds",NULL);
    still_report_attach(c,branch,site);

	LLVMPositionBuilderAtEnd(c->builder, trap_bb);
	LLVMValueRef msg = LLVMBuildGlobalStringPtr(
		c->builder, "index out of bounds", "trap_msg");
    still_diag_location(line,&file,&line);
    LLVMValueRef file_v =
		LLVMBuildGlobalStringPtr(c->builder, file ? file : "?", "trap_file");
	LLVMValueRef args[3] = {
		msg, file_v,
		LLVMConstInt(LLVMInt32TypeInContext(ctx), line, 1)};
    LLVMValueRef call=LLVMBuildCall2(c->builder,
                   LLVMGlobalGetValueType(get_or_declare_trap_fn(c)),
                   get_or_declare_trap_fn(c), args, 3, "");
    still_report_attach(c,call,site);
	LLVMBuildUnreachable(c->builder);

	LLVMPositionBuilderAtEnd(c->builder, cont_bb);
}

// Index into a COMPUTED slice value (`argv[0][i]`, `s.data[i]`): the pair
// is already evaluated; extract data + len, bounds-check by default,
// and return the element address. Shared by get_address's NODE_INDEX.
static LLVMValueRef slice_index_addr(StillCompiler *c, ASTNode *n,
									 LLVMValueRef pair_val,
									 LLVMTypeRef elem_t,
									 LLVMTypeRef *out_type) {
	LLVMContextRef ctx = c->context;
	LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx);
	LLVMValueRef datap =
		LLVMBuildExtractValue(c->builder, pair_val, 0, "slice_data");
	LLVMValueRef slen =
		LLVMBuildExtractValue(c->builder, pair_val, 1, "slice_len");

	LLVMValueRef idx = codegen_expr(c, n->data.index.index);
	if (!n->data.index.index->data_type &&
		n->data.index.index->type == NODE_VAR_REF) {
		Scope *sv2 = scope_find(
			c, n->data.index.index->data.var_ref.name);
		if (sv2 && sv2->node && sv2->node->data_type)
			n->data.index.index->data_type = sv2->node->data_type;
	}
	Type idx64t = {0};
	idx64t.kind = TYPE_I64;
	idx = coerce_value(c, idx, n->data.index.index->data_type, i64_t,
					   &idx64t);

	int need_check = (c->bounds_check_mode != -1);
	if (need_check && c->unchecked_depth == 0) {
		LLVMValueRef ok = LLVMBuildICmp(c->builder, LLVMIntULT, idx, slen,
										"bounds_ok");
		LLVMBasicBlockRef cont_bb =
			wky_append_block(c->current_func, "idx_in_bounds");
		LLVMBasicBlockRef trap_bb =
			wky_append_block(c->current_func, "idx_oob");
		LLVMBuildCondBr(c->builder, ok, cont_bb, trap_bb);

		LLVMPositionBuilderAtEnd(c->builder, trap_bb);
		LLVMValueRef msg = LLVMBuildGlobalStringPtr(
			c->builder, "slice index out of bounds", "trap_msg");
		LLVMValueRef file_v = LLVMBuildGlobalStringPtr(
			c->builder, c->source_filename ? c->source_filename : "?",
			"trap_file");
		LLVMValueRef args[3] = {
			msg, file_v,
			LLVMConstInt(LLVMInt32TypeInContext(ctx),
						 n->line > 0 ? n->line : 0, 1)};
		LLVMBuildCall2(
			c->builder,
			LLVMGlobalGetValueType(get_or_declare_trap_fn(c)),
			get_or_declare_trap_fn(c), args, 3, "");
		LLVMBuildUnreachable(c->builder);

		LLVMPositionBuilderAtEnd(c->builder, cont_bb);
	}

	LLVMValueRef addr = LLVMBuildGEP2(c->builder, elem_t, datap, &idx, 1,
									  "elem_addr");
	if (out_type)
		*out_type = elem_t;
	return addr;
}

LLVMValueRef get_address(StillCompiler *c, ASTNode *n, LLVMTypeRef *out_type) {
    LLVMValueRef managed=wky_memory_lvalue(c,n,out_type,NULL);
    if (managed) return managed;
	switch (n->type) {
	case NODE_VAR_REF: {
		Scope *s = scope_find(c, n->data.var_ref.name);
		if (!s) {
			char *v_path = get_var_path(c, n->data.var_ref.name);
			// Did-you-mean over everything visible in the current chain.
			const char *cands[33];
			int nc = 0;
			for (Scope *cur = c->scope_stack; cur && nc < 32; cur = cur->next)
				cands[nc++] = cur->name;
			cands[nc] = NULL;
			const char *alt = still_diag_closest(n->data.var_ref.name, cands);
			if (alt)
				still_diag_help("a variable with a similar name exists: `%s`",
						   get_var_path(c, alt));
			still_error_named(STILL_E_UNDEF, n, n->data.var_ref.name,
				  "cannot find variable `%s` in this scope", v_path);
			exit(1);
		}
		if (out_type)
			*out_type = s->type;
		return s->val;
	}

	case NODE_MEMBER_ACCESS: {
		// Slice builtins: `xs.len` (i64) and `xs.data` (T*). These live in
		// field slots 1 and 0 of the {data,len} pair -- no StructDef needed.
		// On a fixed array, `.data` decays to &arr[0] and `.len` is the
		// constant N.
		{
			// Slice builtins on ANY slice-typed object: `.len` is field 1,
			// `.data` is field 0. A bare var ref resolves through its scope
			// entry (address of the pair); a computed slice value
			// (`argv[0].len`, string literals) spills to an entry alloca so
			// the lvalue contract holds -- O(2) promotes it away.
			Type *vt = NULL;
			LLVMValueRef pair_addr = NULL;
			if (n->data.member_access.object->type == NODE_VAR_REF) {
				Scope *sv = scope_find(
					c, n->data.member_access.object->data.var_ref.name);
				vt = (sv && sv->node) ? sv->node->data_type : NULL;
				if (vt && vt->kind == TYPE_CHAN) {
					// Channel introspection: .buf/.cap/.head/.count/.mask
					// map straight onto ring-record fields. .head is the
					// monotonic read cursor (never wraps).
					int slot2 =
						strcmp(n->data.member_access.member, "buf") == 0
							? 0
						: strcmp(n->data.member_access.member, "cap") == 0
							? 1
						: strcmp(n->data.member_access.member, "head") == 0
							? 2
						: strcmp(n->data.member_access.member, "count") == 0
							? 3
						: strcmp(n->data.member_access.member, "mask") == 0
							? 4
							: -1;
					if (slot2 < 0) {
						still_diag_note(
							"channel fields: `buf`, `cap`, `head`, "
							"`count`, `mask`");
						still_error(STILL_E_UNDEF, n,
							 "chan<T> has no field `%s`",
							 n->data.member_access.member);
						exit(1);
					}
					LLVMTypeRef cllt = get_llvm_type(c, vt);
					LLVMValueRef fld2 = LLVMBuildStructGEP2(
						c->builder, cllt, sv->val, slot2, "chan_fld");
					if (out_type)
						*out_type = LLVMStructGetTypeAtIndex(
							cllt, (unsigned)slot2);
					return fld2;
				}
				if (vt && vt->kind == TYPE_SLICE)
					pair_addr = sv->val;
				else
					vt = NULL;
			} else if (n->data.member_access.object->type ==
					   NODE_INDEX) {
				// Computed slices (`argv[0].len`): peel the index's element
				// type from its base -- a ptr-to-slice indexes to a slice.
				ASTNode *io = n->data.member_access.object;
				Type *base_t = io->data.index.object->data_type;
				if (!base_t &&
					io->data.index.object->type == NODE_VAR_REF) {
					Scope *sb = scope_find(
						c,
						io->data.index.object->data.var_ref.name);
					base_t = (sb && sb->node) ? sb->node->data_type
											  : NULL;
				}
				if (base_t && (base_t->kind == TYPE_PTR ||
							   base_t->kind == TYPE_SLICE) &&
					base_t->inner && base_t->inner->kind == TYPE_SLICE) {
					vt = base_t->inner;
					LLVMValueRef pair_val =
						value_of_lvalue(c, io);
					pair_addr = create_entry_block_alloca(
						c, LLVMTypeOf(pair_val), "slice_pair_tmp");
					LLVMBuildStore(c->builder, pair_val, pair_addr);
				}
			} else if (n->data.member_access.object->data_type &&
					   n->data.member_access.object->data_type->kind ==
						   TYPE_SLICE &&
					   (strcmp(n->data.member_access.member, "len") == 0 ||
						strcmp(n->data.member_access.member, "data") ==
							0)) {
				vt = n->data.member_access.object->data_type;
				LLVMValueRef pair_val =
					codegen_expr(c, n->data.member_access.object);
				pair_addr = create_entry_block_alloca(
					c, LLVMTypeOf(pair_val), "slice_pair_tmp");
				LLVMBuildStore(c->builder, pair_val, pair_addr);
			}
			if (vt && pair_addr) {
				int slot = strcmp(n->data.member_access.member, "len") == 0
							   ? 1
							   : 0;
				LLVMTypeRef slice_t = get_llvm_type(c, vt);
				LLVMValueRef fld = LLVMBuildStructGEP2(
					c->builder, slice_t, pair_addr, slot, "slice_fld");
				if (out_type) {
					if (slot == 0) {
						LLVMTypeRef elem =
							get_llvm_type(c, vt->inner);
						*out_type = LLVMPointerType(elem, 0);
					} else {
						*out_type =
							LLVMInt64TypeInContext(c->context);
					}
				}
				return fld;
			}
		}
		{
			Scope *sva2 = NULL;
			Type *vta = n->data.member_access.object->data_type;
			if (!vta && n->data.member_access.object->type == NODE_VAR_REF) {
				sva2 = scope_find(
					c, n->data.member_access.object->data.var_ref.name);
				vta = (sva2 && sva2->node) ? sva2->node->data_type : NULL;
			}
			if (vta && vta->kind == TYPE_ARRAY &&
				(strcmp(n->data.member_access.member, "len") == 0 ||
				 strcmp(n->data.member_access.member, "data") == 0)) {
				if (strcmp(n->data.member_access.member, "data") == 0) {
					// An array has no pointer field: build one in an entry
					// alloca holding &arr[0] so the lvalue contract holds
					// (callers load through the returned address). -O2
					// promotes the alloca into a register.
					LLVMTypeRef ignored;
					LLVMValueRef arr_addr =
						get_address(c,
									n->data.member_access.object,
									&ignored);
					LLVMTypeRef elem = get_llvm_type(c, vta->inner);
					LLVMTypeRef ptr_t = LLVMPointerType(elem, 0);
					LLVMValueRef data = LLVMBuildGEP2(
						c->builder, elem, arr_addr,
						(LLVMValueRef[]){LLVMConstInt(
							LLVMInt64TypeInContext(c->context), 0,
							0)},
						1, "arr_data");
					LLVMValueRef slot = create_entry_block_alloca(
						c, ptr_t, "arr_data_slot");
					LLVMBuildStore(c->builder, data, slot);
					if (out_type)
						*out_type = ptr_t;
					return slot;
				}
				if (strcmp(n->data.member_access.member, "len") == 0) {
					// Constant length: materialize an alloca holding N so
					// the lvalue contract (return storage) holds.
					LLVMValueRef len_alloca = create_entry_block_alloca(
						c, LLVMInt64TypeInContext(c->context),
						"arr_len_tmp");
					LLVMBuildStore(
						c->builder,
						LLVMConstInt(
							LLVMInt64TypeInContext(c->context),
							(unsigned long long)vta->array_len, 0),
						len_alloca);
					if (out_type)
						*out_type =
							LLVMInt64TypeInContext(c->context);
					return len_alloca;
				}
			}
		}

		LLVMTypeRef container_type = NULL;
		LLVMValueRef ptr =
			get_address(c, n->data.member_access.object, &container_type);
		if (!ptr && n->data.member_access.object->data_type) {
            if (wky_contains_managed(c,n->data.member_access.object->data_type,1)) {
                still_error(STILL_E_OWNERSHIP,n,"bind an owned temporary before accessing its fields");
                exit(1);
            }
			// Rvalue base (`Vec.new(1,2).x`): evaluate the object to a
			// value, spill to a temp alloca, and address through it. The
			// optimizer promotes the temp away -- zero cost over direct
			// field extraction.
			LLVMTypeRef obj_t =
				get_llvm_type(c, n->data.member_access.object->data_type);
			LLVMValueRef tmp = create_entry_block_alloca(
				c, obj_t, "rval_base");
			LLVMValueRef val =
				codegen_expr(c, n->data.member_access.object);
			LLVMBuildStore(c->builder, val, tmp);
			ptr = tmp;
			container_type = obj_t;
		}
		if (!ptr && n->data.member_access.object->type == NODE_VAR_REF) {
			char mangled[256];
			snprintf(mangled, sizeof(mangled), "%s__%s",
					 n->data.member_access.object->data.var_ref.name,
					 n->data.member_access.member);
			Scope *asv = scope_find(c, mangled);
			if (asv) {
				if (out_type)
					*out_type = asv->type;
				return asv->val;
			}
		}
		if (!ptr || !container_type) {
			still_error(STILL_E_SEMANTIC, n,
				 "failed to resolve member access base"); // internal
			exit(1);
		}

		// `ptr` is a pointer to the container storage. If the container is
		// itself addressed through a pointer (e.g. `p->x` where the deref
		// already yielded the pointee address), the container type is the
		// struct and we GEP straight through `ptr`. The pointee type comes
		// from the AST -- LLVM 21 pointers are opaque, so there is no
		// element type to read off the pointer itself.
		LLVMTypeRef struct_t = container_type;
		Type *container_ast =
			n->data.member_access.object->data_type;
		if (!container_ast &&
			n->data.member_access.object->type == NODE_VAR_REF) {
			Scope *cs =
				scope_find(c, n->data.member_access.object->data.var_ref.name);
			container_ast = (cs && cs->node) ? cs->node->data_type : NULL;
		}
		if (LLVMGetTypeKind(struct_t) == LLVMPointerTypeKind) {
			ptr = LLVMBuildLoad2(c->builder, container_type, ptr, "auto_deref");
			if (container_ast &&
				(container_ast->kind == TYPE_PTR ||
				 container_ast->kind == TYPE_AMP) &&
				container_ast->inner)
				struct_t = get_llvm_type(c, container_ast->inner);
			else
				struct_t = LLVMGetElementType(struct_t);
		}

		// Direct field first. Miss falls back to embedding promotion
		// (IDEAS 3): `e.name` on `struct Employee { Person; u32 badge; }`
		// resolves as e.Person.name -- one extra constant-offset GEP per
		// hop, which the optimizer folds into a single displacement.
		// The loop handles multi-level embeds (`o.Mid.Inner`-style).
		int hops = 0;
		while (!has_direct_field(c, struct_t,
								 n->data.member_access.member)) {
			int mid_idx = -1, inner_idx = -1;
			(void)inner_idx;
			if (!try_promoted_field(c, struct_t,
									n->data.member_access.member,
									&mid_idx, &inner_idx))
				break;
			LLVMValueRef mid_addr = LLVMBuildStructGEP2(
				c->builder, struct_t, ptr, mid_idx, "emb_base");
			ptr = mid_addr;
			struct_t = LLVMStructGetTypeAtIndex(
				struct_t, (unsigned)mid_idx);
			hops++;
			if (hops > 16)
				break; // embedding cycles cannot exist in finite types
		}
		int idx = get_field_index(c, struct_t, n->data.member_access.member);
		LLVMValueRef field_addr =
			LLVMBuildStructGEP2(c->builder, struct_t, ptr, idx, "fld_addr");
		if (out_type)
			*out_type =
				get_field_type(c, struct_t, n->data.member_access.member);
		return field_addr;
	}

	case NODE_INDEX: {
		// a[i] where `a` is an array (alloca) or pointer. GEP with the
		// element type; no load of the base is needed for arrays.
        Type *array_ast=wky_expr_type(c,n->data.index.object);
        if (array_ast && array_ast->kind==TYPE_ARRAY) {
            LLVMTypeRef array_type=NULL;
            LLVMValueRef array=get_address(c,n->data.index.object,&array_type);
            if (!array) {
                if (wky_contains_managed(c,array_ast,1)) {
                    still_error(STILL_E_OWNERSHIP,n,"bind an owned temporary before indexing it");
                    exit(1);
                }
                LLVMValueRef value=codegen_expr(c,n->data.index.object);
                array_type=LLVMTypeOf(value);
                array=create_entry_block_alloca(c,array_type,"array_temporary");
                LLVMBuildStore(c->builder,value,array);
            }
            LLVMTypeRef i64=LLVMInt64TypeInContext(c->context);
            LLVMValueRef index=codegen_expr(c,n->data.index.index);
            index=coerce_value(c,index,wky_expr_type(c,n->data.index.index),i64,NULL);
            if (c->bounds_check_mode!=-1)
                emit_bounds_check(c,n->data.index.index,index,array_ast->array_len,c->source_filename,n->line);
            LLVMValueRef indices[]={LLVMConstInt(i64,0,0),index};
            n->data_type=array_ast->inner;
            if (out_type) *out_type=get_llvm_type(c,array_ast->inner);
            return LLVMBuildGEP2(c->builder,array_type,array,indices,2,"array_element");
        }
		LLVMTypeRef elem_type = NULL;
		if (n->data_type)
			elem_type = get_llvm_type(c, n->data_type);

		ASTNode *obj = n->data.index.object;
		// Indexing a SLICE-typed expression (`argv[0][0]`): the data
		// pointer is extracted from the pair and GEP-ed directly -- GEP
		// through the pair value itself would be invalid IR.
		if (obj->type != NODE_VAR_REF) {
			Type *ot = obj->data_type;
			if (!ot && obj->type == NODE_MEMBER_ACCESS)
				ot = obj->data_type;
			if (ot && ot->kind == TYPE_SLICE) {
				LLVMValueRef pair_val = value_of_lvalue(c, obj);
				return slice_index_addr(c, n, pair_val,
										get_llvm_type(c, ot->inner),
										out_type);
			}
		}
		LLVMValueRef base;
		LLVMTypeRef elem = elem_type;

		// Slice indexing xs[i]: load {data,len}, bounds-check against len
		// by default, then GEP data[i]. Proven checks are folded away --
		// the load of data and one GEP, same as a pointer index.
		Scope *s_slice =
			(obj->type == NODE_VAR_REF)
				? scope_find(c, obj->data.var_ref.name)
				: NULL;
		Type *obj_ast =
			(s_slice && s_slice->node) ? s_slice->node->data_type : NULL;
		if (obj_ast && obj_ast->kind == TYPE_SLICE) {
			LLVMContextRef ctx = c->context;
			LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx);
			LLVMTypeRef slice_t = get_llvm_type(c, obj_ast);
			LLVMTypeRef elem = get_llvm_type(c, obj_ast->inner);
			LLVMTypeRef ptr_t = LLVMPointerType(elem, 0);

			LLVMValueRef data_p = LLVMBuildStructGEP2(
				c->builder, slice_t, s_slice->val, 0, "slice_data_ptr");
			LLVMValueRef datap =
				LLVMBuildLoad2(c->builder, ptr_t, data_p, "slice_data");
			attach_tbaa(c, datap, ptr_t);

			LLVMValueRef idx = codegen_expr(c, n->data.index.index);
			if (!n->data.index.index->data_type &&
				n->data.index.index->type == NODE_VAR_REF) {
				Scope *sv2 = scope_find(
					c, n->data.index.index->data.var_ref.name);
				if (sv2 && sv2->node && sv2->node->data_type)
					n->data.index.index->data_type = sv2->node->data_type;
			}
			Type idx64t = {0};
			idx64t.kind = TYPE_I64;
			idx = coerce_value(c, idx, n->data.index.index->data_type,
							   i64_t, &idx64t);

			int need_check = (c->bounds_check_mode != -1);
			if (need_check && c->unchecked_depth == 0) {
				// Dynamic-length twin of emit_bounds_check: trap when
				// idx >= slice.len. Release never reaches this branch,
				// and `unchecked {}` suppresses it everywhere.
				LLVMValueRef len_p = LLVMBuildStructGEP2(
					c->builder, slice_t, s_slice->val, 1,
					"slice_len_ptr");
				LLVMValueRef slen =
					LLVMBuildLoad2(c->builder, i64_t, len_p, "slice_len");
				attach_tbaa(c, slen, i64_t);
				LLVMValueRef ok = LLVMBuildICmp(
					c->builder, LLVMIntULT, idx, slen, "bounds_ok");
				LLVMBasicBlockRef cont_bb = wky_append_block(
					c->current_func, "idx_in_bounds");
				LLVMBasicBlockRef trap_bb = wky_append_block(
					c->current_func, "idx_oob");
				LLVMBuildCondBr(c->builder, ok, cont_bb, trap_bb);

				LLVMPositionBuilderAtEnd(c->builder, trap_bb);
				LLVMValueRef msg = LLVMBuildGlobalStringPtr(
					c->builder, "slice index out of bounds", "trap_msg");
				LLVMValueRef file_v = LLVMBuildGlobalStringPtr(
					c->builder, c->source_filename ? c->source_filename : "?",
					"trap_file");
				LLVMValueRef args[3] = {
					msg, file_v,
					LLVMConstInt(LLVMInt32TypeInContext(ctx),
								 n->line > 0 ? n->line : 0, 1)};
				LLVMBuildCall2(
					c->builder,
					LLVMGlobalGetValueType(get_or_declare_trap_fn(c)),
					get_or_declare_trap_fn(c), args, 3, "");
				LLVMBuildUnreachable(c->builder);

				LLVMPositionBuilderAtEnd(c->builder, cont_bb);
			}

			LLVMValueRef addr = LLVMBuildGEP2(c->builder, elem, datap,
											  &idx, 1, "elem_addr");
			if (out_type)
				*out_type = elem;
			return addr;
		}

		if (obj->type == NODE_VAR_REF) {
			// Direct storage access: arrays are allocas, pointers are
			// allocas holding T*. Either way GEP works without loading the
			// base first -- LLVM lowers both forms.
			Scope *s = scope_find(c, obj->data.var_ref.name);
			if (!s) {
				char *v_path = get_var_path(c, obj->data.var_ref.name);
				const char *cands[33];
				int nc = 0;
				for (Scope *cur = c->scope_stack; cur && nc < 32;
					 cur = cur->next)
					cands[nc++] = cur->name;
				cands[nc] = NULL;
				const char *alt =
					still_diag_closest(obj->data.var_ref.name, cands);
				if (alt)
					still_diag_help(
						"a variable with a similar name exists: `%s`",
						get_var_path(c, alt));
				still_error_named(STILL_E_UNDEF, n, obj->data.var_ref.name,
					  "cannot find variable `%s` in this scope", v_path);
				exit(1);
			}
			base = s->val;
			if (!elem) {
				// Element type must come from the AST declaration:
				// LLVM 21 pointers are opaque, so LLVMGetElementType on
				// a pointer value is invalid and yields garbage.
				LLVMTypeRef st = s->type;
				Type *st_ast = (s->node && s->node->data_type)
								   ? s->node->data_type
								   : NULL;
				if (LLVMGetTypeKind(st) == LLVMArrayTypeKind)
					elem = LLVMGetElementType(st);
				else if (st_ast &&
						 (st_ast->kind == TYPE_PTR || st_ast->kind == TYPE_AMP))
					elem = st_ast->inner ? get_llvm_type(c, st_ast->inner)
										 : LLVMInt8TypeInContext(c->context);
				else if (st_ast && st_ast->kind == TYPE_ARRAY)
					elem = st_ast->inner ? get_llvm_type(c, st_ast->inner)
										 : LLVMInt32TypeInContext(c->context);
			}
			// A pointer variable's storage holds the T*; the indexed
			// object lives behind that pointer, so load it first. Array
			// allocas index directly (base stays the alloca).
			if (LLVMGetTypeKind(s->type) != LLVMArrayTypeKind) {
				LLVMTypeRef ptr_t = LLVMPointerType(elem, 0);
				base =
					LLVMBuildLoad2(c->builder, ptr_t, base, "idx_base");
			}
		} else if (obj->type == NODE_MEMBER_ACCESS) {
			// `ps.x[i]`: resolve the FIELD'S ADDRESS (a GEP into the
			// struct) and index from there. Loading the whole array value
			// first would leave a non-pointer GEP base.
			LLVMTypeRef fld_ty = NULL;
			LLVMValueRef fld_addr =
				get_address(c, obj, &fld_ty);
			if (fld_addr && LLVMGetTypeKind(fld_ty) == LLVMArrayTypeKind) {
				elem = LLVMGetElementType(fld_ty);
				base = fld_addr; // arrays index directly, like allocas
			} else {
				base = value_of_lvalue(c, obj);
			}
		} else {
			// Complex base: resolve its address, then load if it's a
			// pointer-to-pointer situation.
			LLVMTypeRef obj_val_ty = NULL;
			base = value_of_lvalue(c, obj);
			(void)obj_val_ty;
		}

		if (!elem)
			elem = LLVMInt32TypeInContext(c->context);

		LLVMValueRef idx = codegen_expr(c, n->data.index.index);
		// Bare VAR_REF indices carry no parser-side type; stamp it from
		// the declaration so sext/zext matches its signedness (a zext'd
		// `i32 i` blocks induction-variable analysis and kills
		// vectorization of the surrounding loop). The i64 destination is
		// marked signed so a signed index widens with sext.
		if (!n->data.index.index->data_type &&
			n->data.index.index->type == NODE_VAR_REF) {
			Scope *sv =
				scope_find(c, n->data.index.index->data.var_ref.name);
			if (sv && sv->node && sv->node->data_type)
				n->data.index.index->data_type = sv->node->data_type;
		}
		Type idx64 = {0};
		idx64.kind = TYPE_I64;
		idx = coerce_value(c, idx, n->data.index.index->data_type,
						   LLVMInt64TypeInContext(c->context), &idx64);

		// Debug builds trap on out-of-bounds fixed-array indexing. The
		// bound comes from the AST declaration ([N]T), not LLVM -- opaque
		// pointers carry no length.
		int need_check = (c->bounds_check_mode != -1);
		if (need_check && obj->type == NODE_VAR_REF) {
			Scope *s_chk = scope_find(c, obj->data.var_ref.name);
			if (s_chk && s_chk->node && s_chk->node->data_type &&
				s_chk->node->data_type->kind == TYPE_ARRAY) {
				emit_bounds_check(c, n->data.index.index, idx,
								  s_chk->node->data_type->array_len,
								  c->source_filename, n->line);
			}
		}

		LLVMValueRef addr =
			LLVMBuildGEP2(c->builder, elem, base, &idx, 1, "elem_addr");
		if (out_type)
			*out_type = elem;
		return addr;
	}

	case NODE_DEREF: {
		// Load the pointer value out of the inner lvalue's storage. The
		// inner storage holds a T*; loading it yields the address of the T.
		LLVMTypeRef inner_storage_type = NULL;
		LLVMValueRef inner_addr =
			get_address(c, n->data.deref.expr, &inner_storage_type);

		// The pointee type must come from AST info. LLVM 21 pointers are
		// opaque, so there is no LLVM-level pointee type to peel -- the
		// old shape-recovery path read garbage off opaque pointers.
		Type *val_ast = deref_value_type(c, n);
		LLVMTypeRef pointee = val_ast ? get_llvm_type(c, val_ast) : NULL;
		if (!pointee)
			pointee = LLVMInt8TypeInContext(c->context);
		if (!pointee)
			pointee = LLVMInt32TypeInContext(c->context);

        LLVMTypeRef ptr_t = LLVMPointerType(pointee, 0);
        LLVMValueRef loaded;
        if (inner_addr) {
            loaded=LLVMBuildLoad2(c->builder,ptr_t,inner_addr,"deref_ptr");
            attach_tbaa(c,loaded,ptr_t);
        } else {
            loaded=codegen_expr(c,n->data.deref.expr);
            if (!loaded || LLVMGetTypeKind(LLVMTypeOf(loaded))!=LLVMPointerTypeKind) {
                still_error(STILL_E_TYPE,n,"dereference requires a pointer value"); exit(1);
            }
        }

		if (out_type)
			*out_type = pointee;
		return loaded;
	}

	default:
		return NULL;
	}
}

// Rvalue evaluation for lvalue-shaped nodes. Loads exactly once from the
// resolved address; `&x` (NODE_AMP) is the address itself, no load.
LLVMValueRef value_of_lvalue(StillCompiler *c, ASTNode *n) {
    if (wky_contains_managed(c,wky_expr_type(c, n),1)) {
		still_error(STILL_E_TYPE, n, "an owner cannot be copied; use move, ref_of, or clone");
		exit(1);
	}
	if (n->type == NODE_AMP) {
		ASTNode *base = n->data.deref.expr;
		while (base && (base->type == NODE_INDEX || base->type == NODE_DEREF || base->type == NODE_MEMBER_ACCESS)) {
			base = base->type == NODE_INDEX ? base->data.index.object :
				base->type == NODE_DEREF ? base->data.deref.expr : base->data.member_access.object;
			if (wky_is_managed(wky_expr_type(c, base))) break;
		}
		if (wky_contains_managed(c, wky_expr_type(c, base), 0)) {
			still_error(STILL_E_TYPE, n, "managed storage cannot decay to a raw pointer; use ref_of or ref_slice");
			exit(1);
		}
		LLVMTypeRef addr_type = NULL;
		LLVMValueRef addr = get_address(c, n->data.deref.expr, &addr_type);
		if (!addr) {
			still_error(STILL_E_ARGS, n, "cannot take address of a non-lvalue");
			exit(1);
		}
		return addr;
	}

	LLVMTypeRef val_type = NULL;
	LLVMValueRef addr = get_address(c, n, &val_type);
	if (!addr || !val_type) {
		still_error(STILL_E_SEMANTIC, n, "failed to resolve lvalue"); // internal
		exit(1);
	}

    if (wky_contains_managed(c,wky_expr_type(c,n),1)) {
        still_error(STILL_E_TYPE,n,"an owned value cannot be copied; use move, ref_of, or clone");
        exit(1);
    }
    LLVMValueRef val = LLVMBuildLoad2(c->builder, val_type, addr, "load");
	attach_tbaa(c, val, val_type);
	return val;
}

// Normalize a condition value to i1. Integer conditions wider than one bit
// become `!= 0`; pointers become null checks.
LLVMValueRef cond_to_bool(StillCompiler *c, LLVMValueRef cond) {
	LLVMTypeRef t = LLVMTypeOf(cond);
	switch (LLVMGetTypeKind(t)) {
	case LLVMIntegerTypeKind:
		if (LLVMGetIntTypeWidth(t) == 1)
			return cond;
		return LLVMBuildICmp(c->builder, LLVMIntNE, cond, LLVMConstInt(t, 0, 0),
							 "to_bool");
	case LLVMPointerTypeKind:
		return LLVMBuildIsNotNull(c->builder, cond, "ptr_to_bool");
	default:
		still_diag_error_at(STILL_E_TYPE, "<wky>", NULL, 0,
					   "condition must be bool, integer or pointer");
		exit(1);
	}
}

// Coerce `v` (produced from AST type `src_ast`, may be NULL) to `dst`.
// Sign-aware: signed->signed widening is SExt, everything else ZExt, so
// negative values survive integer promotion.
LLVMValueRef coerce_value(StillCompiler *c, LLVMValueRef v, Type *src_ast,
						  LLVMTypeRef dst, Type *dst_ast) {
	LLVMTypeRef src = LLVMTypeOf(v);
	wky_check_conversion(c, v, src_ast, dst, dst_ast);
    Type *source_type=wky_concrete_type(c,src_ast), *destination_type=wky_concrete_type(c,dst_ast);
    if (source_type && destination_type && source_type->kind==TYPE_ENUM &&
        destination_type->kind==TYPE_ENUM && !wky_types_same(source_type,destination_type)) {
        still_diag_error_at(STILL_E_TYPE,c->source_filename,NULL,0,
            "cannot implicitly convert between distinct enum types");
        exit(1);
    }
    if (wky_contains_managed(c,source_type,0) || wky_contains_managed(c,destination_type,0)) {
        if ((destination_type && (!source_type || !wky_types_same(source_type,destination_type))) || src != dst) {
			still_diag_error_at(STILL_E_TYPE, c->source_filename, NULL, 0,
				"managed references cannot be cast, forged, or implicitly converted");
			exit(1);
		}
	}
	if (src == dst)
		return v;

	LLVMTypeKind sk = LLVMGetTypeKind(src);
	LLVMTypeKind dk = LLVMGetTypeKind(dst);

	if (sk == LLVMIntegerTypeKind && dk == LLVMIntegerTypeKind) {
		unsigned sw = LLVMGetIntTypeWidth(src);
		unsigned dw = LLVMGetIntTypeWidth(dst);
		if (dw == 1 && sw > 1)
			// Anything -> bool is a != 0 test. A plain trunc would keep
			// only the low bit: (bool)42 must be true, not false.
			return LLVMBuildICmp(c->builder, LLVMIntNE, v,
								 LLVMConstInt(src, 0, 0), "to_bool");
		if (dw < sw)
			return LLVMBuildTrunc(c->builder, v, dst, "trunc");
		if (sw < dw) {
			// Widen by the SOURCE's own signedness. When the destination
			// AST is unknown (extern params arrive as bare LLVM types),
			// the source type is all we have -- and a negative i32 must
			// sext even when heading into an untyped i64 slot.
			int signed_ext = type_is_signed(c, src_ast);
			return signed_ext ? LLVMBuildSExt(c->builder, v, dst, "sext")
							  : LLVMBuildZExt(c->builder, v, dst, "zext");
		}
		return v;
	}
	if (sk == LLVMIntegerTypeKind && is_fp_kind(dk))
		return type_is_signed(c, src_ast)
				   ? LLVMBuildSIToFP(c->builder, v, dst, "sitofp")
				   : LLVMBuildUIToFP(c->builder, v, dst, "uitofp");
	if (is_fp_kind(sk) && dk == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(dst)==1)
		return LLVMBuildFCmp(c->builder, LLVMRealUNE, v, LLVMConstReal(src, 0.0), "float_to_bool");
	if (is_fp_kind(sk) && dk == LLVMIntegerTypeKind)
		return type_is_signed(c, dst_ast)
				   ? LLVMBuildFPToSI(c->builder, v, dst, "fptosi")
				   : LLVMBuildFPToUI(c->builder, v, dst, "fptoui");
	if (is_fp_kind(sk) && is_fp_kind(dk)) {
		if (sk == dk)
			return v;
		// Width decides direction; equal widths of different encodings
		// (f16 <-> bf16) go through f32 as the common meeting ground.
		unsigned sw_ = sk == LLVMHalfTypeKind     ? 16
					   : sk == LLVMBFloatTypeKind ? 16
					   : sk == LLVMFloatTypeKind  ? 32
												  : 64;
		unsigned dw_ = dk == LLVMHalfTypeKind     ? 16
					   : dk == LLVMBFloatTypeKind ? 16
					   : dk == LLVMFloatTypeKind  ? 32
												  : 64;
		if (sw_ < dw_)
			return LLVMBuildFPExt(c->builder, v, dst, "fpext");
		if (sw_ > dw_)
			return LLVMBuildFPTrunc(c->builder, v, dst, "fptrunc");
		LLVMTypeRef f32_t = LLVMFloatTypeInContext(c->context);
		LLVMValueRef up = LLVMBuildFPExt(c->builder, v, f32_t, "fpmeet");
		return LLVMBuildFPTrunc(c->builder, up, dst, "fpnarrow");
	}
	if (sk == LLVMStructTypeKind && dk == LLVMPointerTypeKind &&
		src_ast && src_ast->kind == TYPE_SLICE)
		return LLVMBuildExtractValue(c->builder, v, 0, "slice_decay");
	if (sk == LLVMPointerTypeKind && dk == LLVMPointerTypeKind)
		return LLVMBuildPointerCast(c->builder, v, dst, "ptr_cast");
	if (sk == LLVMArrayTypeKind && dk == LLVMStructTypeKind &&
		src_ast && src_ast->kind == TYPE_ARRAY &&
		dst_ast && dst_ast->kind == TYPE_SLICE) {
		// Array value -> slice view: spill the array so the data pointer
		// has an address, then pair it with the constant length. The
		// optimizer promotes the spill away in release builds.
		LLVMValueRef slot =
			create_entry_block_alloca(c, src, "to_slice_arr");
		LLVMBuildStore(c->builder, v, slot);
		LLVMTypeRef elem = get_llvm_type(c, src_ast->inner);
		LLVMValueRef data = LLVMBuildGEP2(
			c->builder, elem, slot,
			(LLVMValueRef[]){LLVMConstInt(LLVMInt64TypeInContext(c->context),
										  0, 0)},
			1, "slice_data");
		LLVMValueRef view = LLVMGetUndef(dst);
		view = LLVMBuildInsertValue(c->builder, view, data, 0,
									"slice_ins_data");
		view = LLVMBuildInsertValue(
			c->builder, view,
			LLVMConstInt(LLVMInt64TypeInContext(c->context),
						 (unsigned long long)src_ast->array_len, 0),
			1, "slice_ins_len");
		return view;
	}
	if (sk == LLVMPointerTypeKind && dk == LLVMStructTypeKind &&
		dst_ast && dst_ast->kind == TYPE_SLICE) {
		// T* -> []T: length unknown at compile time; a raw pointer makes a
		// zero-length view rather than a guess. Explicit and safe.
		LLVMValueRef view = LLVMGetUndef(dst);
		view = LLVMBuildInsertValue(c->builder, view, v, 0,
									"slice_ins_data");
		view = LLVMBuildInsertValue(
			c->builder, view,
			LLVMConstInt(LLVMInt64TypeInContext(c->context), 0, 0),
			1, "slice_ins_len");
		return view;
	}
	if (sk == LLVMPointerTypeKind && dk == LLVMStructTypeKind)
		return LLVMBuildPointerCast(c->builder, v, dst, "raw_cast");
	if (sk == LLVMStructTypeKind && dk == LLVMStructTypeKind) {
		if (src == dst)
			return v;
		LLVMValueRef slot = create_entry_block_alloca(c, src, "struct_coerce_src");
		LLVMBuildStore(c->builder, v, slot);
		LLVMValueRef cast_ptr = LLVMBuildPointerCast(c->builder, slot, LLVMPointerType(dst, 0), "struct_coerce_cast");
		return LLVMBuildLoad2(c->builder, dst, cast_ptr, "struct_coerce_val");
	}
	if (sk == dk)
		return v;
	// Last resort: bitcast between same-sized types; otherwise the value is
	// incompatible with the destination and that's a real codegen bug.
	if (LLVMGetTypeKind(dst) == LLVMPointerTypeKind ||
		LLVMGetTypeKind(src) == LLVMPointerTypeKind)
		return LLVMBuildPointerCast(c->builder, v, dst, "raw_cast");

	still_diag_error_at(STILL_E_SEMANTIC, "<wky>", NULL, 0,
				   "cannot coerce value in codegen"); // internal
	exit(1);
}

// Keep unlikely error handling out of the hot path. LLVM folds proven checks.
void emit_check_or_trap(StillCompiler *c, ASTNode *n, LLVMValueRef ok, const char *message) {
    if (LLVMIsAConstantInt(ok) && LLVMConstIntGetZExtValue(ok)) {
        still_report_site(c,n,"guard",message,"Lowering proved the safety predicate is constant true.");
        return;
    }
	LLVMBasicBlockRef cont = wky_append_block(c->current_func, "checked");
	LLVMBasicBlockRef trap = wky_append_block(c->current_func, "out_of_bounds");
    LLVMValueRef br = LLVMBuildCondBr(c->builder, ok, cont, trap);
    LLVMMetadataRef site=still_report_site(c,n,"guard",message,NULL);
    still_report_attach(c,br,site);
	set_branch_weights(c, br, 2000, 1);
	LLVMPositionBuilderAtEnd(c->builder, trap);
	LLVMValueRef fn = get_or_declare_trap_fn(c);
	const char *file=c->source_filename;
	int line=n && n->line>0 ? n->line : c->source_line;
	still_diag_location(line,&file,&line);
	LLVMValueRef args[] = {
		LLVMBuildGlobalStringPtr(c->builder, message, "trap_msg"),
		LLVMBuildGlobalStringPtr(c->builder, file ? file : "?", "trap_file"),
		LLVMConstInt(LLVMInt32TypeInContext(c->context), line, 0)};
    LLVMValueRef call=LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(fn), fn, args, 3, "");
    still_report_attach(c,call,site);
	LLVMBuildUnreachable(c->builder);
	LLVMPositionBuilderAtEnd(c->builder, cont);
}
