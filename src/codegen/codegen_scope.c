#include "codegen_internal.h"

// Convert a Kawa identifier (which uses `__` as a path separator inside
// synthesized names) to a dotted path for display. The buffer is arena-
// allocated; the program only calls this from error paths so the lifetime
// is fine.
char *get_var_path(KawaCompiler *c, const char *s) {
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

const char *resolve_type_name(KawaCompiler *c, ASTNode *n) {
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
LLVMValueRef create_entry_block_alloca(KawaCompiler *c, LLVMTypeRef type,
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
// forces a runtime init in kawa_globals_init.
static LLVMValueRef const_eval_expr(KawaCompiler *c, ASTNode *n,
									LLVMTypeRef dst);

// Integer literals are i32 until coerced; a binop node's data_type tells us
// how the parser typed it. `is_signed` picks sign-extension vs zero-fill and
// signed vs unsigned division/remainder -- mirroring build_int_binop().
static int init_is_signed(KawaCompiler *c, ASTNode *n) {
	return n->data_type ? type_is_signed(c, n->data_type) : 0;
}

int global_init_is_constant(KawaCompiler *c, ASTNode *n) {
	if (!n)
		return 1;
	switch (n->type) {
	case NODE_LITERAL:
	case NODE_STRING_LIT:
		return 1;
	case NODE_BINARY_OP:
		return global_init_is_constant(c, n->data.bin_op.left) &&
			   global_init_is_constant(c, n->data.bin_op.right);
	case NODE_STRUCT_LITERAL:
		for (StructInitItem *it = n->data.struct_lit.items; it; it = it->next)
			if (!global_init_is_constant(c, it->value))
				return 0;
		return 1;
	default:
		(void)c;
		return 0;
	}
}

// Rebuild an integer constant at `dst`'s width from its raw value, keeping
// the bit pattern for same/narrower widths and re-signing only when growing.
// Replaces the missing LLVMConstTrunc/SExt/ZExt trio with one uniform path.
static LLVMValueRef const_int_as(KawaCompiler *c, LLVMValueRef v,
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
static int fold_int_binop(int tok, unsigned long long a, unsigned long long b,
						  int lhs_signed, int rhs_signed,
						  unsigned long long *out) {
	int div_signed = lhs_signed && rhs_signed;
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
		if (b == 0)
			return 0;
		*out = div_signed ? (unsigned long long)((long long)a / (long long)b)
						  : a / b;
		return 1;
	case TOK_PERCENT:
		if (b == 0)
			return 0;
		*out = div_signed ? (unsigned long long)((long long)a % (long long)b)
						  : a % b;
		return 1;
	default:
		return 0;
	}
}

// Evaluate a compile-time initializer expression to an LLVM Constant of the
// requested type. Only called after global_init_is_constant() returned true.
LLVMValueRef const_eval_global_init(KawaCompiler *c, ASTNode *n,
									LLVMTypeRef dst, Type *dst_ast) {
	LLVMValueRef v = const_eval_expr(c, n, dst);
	if (!v)
		return LLVMConstNull(dst);

	// String literal in pointer position: build the @.str global directly
	// (no builder available on this path).
	if (n->type == NODE_STRING_LIT &&
		LLVMGetTypeKind(dst) == LLVMPointerTypeKind) {
		size_t len = strlen(n->data.str_lit.s_val);
		LLVMValueRef data =
			LLVMConstString(n->data.str_lit.s_val, (unsigned)len, 1);
		LLVMValueRef str_g = LLVMAddGlobal(
			c->module, LLVMArrayType(LLVMInt8TypeInContext(c->context),
									 (unsigned)len + 1),
			"gstr");
		str_g = LLVMConstBitCast(str_g, dst);
		LLVMSetInitializer(str_g, data);
		LLVMSetGlobalConstant(str_g, 1);
		LLVMSetAlignment(str_g, 1);
		return str_g;
	}

	// Integer literal -> FP destination (e.g. `f64 x = 5;`): the folder
	// emits i32 for int literals regardless of the destination kind.
	if (n->type == NODE_LITERAL && n->data_type &&
		n->data_type->kind != TYPE_F32 && n->data_type->kind != TYPE_F64 &&
		(LLVMGetTypeKind(dst) == LLVMFloatTypeKind ||
		 LLVMGetTypeKind(dst) == LLVMDoubleTypeKind)) {
		double d = init_is_signed(c, n) ? (double)LLVMConstIntGetSExtValue(v)
										: (double)LLVMConstIntGetZExtValue(v);
		return LLVMConstReal(dst, d);
	}
	// FP -> FP width change (f32 <-> f64): no ConstFPTrunc/Ext in this API,
	// so round-trip through the double value and re-emit at `dst`.
	if ((LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMFloatTypeKind ||
		 LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMDoubleTypeKind) &&
		(LLVMGetTypeKind(dst) == LLVMFloatTypeKind ||
		 LLVMGetTypeKind(dst) == LLVMDoubleTypeKind) &&
		LLVMTypeOf(v) != dst)
		return LLVMConstReal(
			dst, LLVMConstRealGetDouble(v, &(LLVMBool){0}));

	v = const_int_as(c, v, dst, init_is_signed(c, n));
	(void)dst_ast;
	return v;
}

static LLVMValueRef const_eval_expr(KawaCompiler *c, ASTNode *n,
									LLVMTypeRef dst) {
	switch (n->type) {
	case NODE_LITERAL:
		if (n->data_type &&
			(n->data_type->kind == TYPE_F32 || n->data_type->kind == TYPE_F64))
			return LLVMConstReal(get_llvm_type(c, n->data_type),
								 (double)n->data.literal.f_val);
		return LLVMConstInt(LLVMInt32TypeInContext(c->context),
							n->data.literal.i_val,
							n->data_type ? type_is_signed(c, n->data_type) : 0);
	case NODE_BINARY_OP: {
		// Fold manually: LLVM 21's C API has no ConstBinOp/Mul/Div family.
		// GetSExtValue/GetZExtValue already return the value carried to
		// 64 bits with the right sign, so operands fold directly; the mask
		// below re-homes the result to the wider operand's width.
		LLVMValueRef l = const_eval_expr(c, n->data.bin_op.left, NULL);
		LLVMValueRef r = const_eval_expr(c, n->data.bin_op.right, NULL);
		if (!l || !r)
			return NULL;
		if (LLVMGetTypeKind(LLVMTypeOf(l)) != LLVMIntegerTypeKind ||
			LLVMGetTypeKind(LLVMTypeOf(r)) != LLVMIntegerTypeKind)
			return NULL; // FP folding stays with the IR constant folder
		int l_s = init_is_signed(c, n->data.bin_op.left);
		int r_s = init_is_signed(c, n->data.bin_op.right);
		unsigned long long a =
			l_s ? (unsigned long long)LLVMConstIntGetSExtValue(l)
				: LLVMConstIntGetZExtValue(l);
		unsigned long long b =
			r_s ? (unsigned long long)LLVMConstIntGetSExtValue(r)
				: LLVMConstIntGetZExtValue(r);
		unsigned w = LLVMGetIntTypeWidth(LLVMTypeOf(l));
		unsigned rw = LLVMGetIntTypeWidth(LLVMTypeOf(r));
		if (rw > w)
			w = rw;
		unsigned long long res;
		if (!fold_int_binop(n->data.bin_op.op, a, b, l_s, r_s, &res))
			return NULL;
		res &= (w >= 64) ? ~0ULL : ((1ULL << w) - 1ULL);
		return LLVMConstInt(LLVMIntTypeInContext(c->context, w), res,
							l_s || r_s);
	}
	case NODE_STRUCT_LITERAL: {
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
			for (StructInitItem *it = n->data.struct_lit.items; it && i < len;
				 i++, it = it->next)
				elems[i] = const_eval_global_init(c, it->value, elem_t, NULL);
			for (; i < len; i++)
				elems[i] = LLVMConstNull(elem_t);
			return LLVMConstArray(elem_t, elems, len);
		}
		if (LLVMGetTypeKind(s_type) != LLVMStructTypeKind)
			return NULL;
		unsigned fc = LLVMCountStructElementTypes(s_type);
		LLVMValueRef *fields =
			arena_alloc(c->arena, sizeof(LLVMValueRef) * (fc ? fc : 1));
		unsigned i = 0;
		for (StructInitItem *it = n->data.struct_lit.items; it && i < fc;
			 i++, it = it->next)
			fields[i] = const_eval_global_init(
				c, it->value, LLVMStructGetTypeAtIndex(s_type, i), NULL);
		for (; i < fc; i++)
			fields[i] = LLVMConstNull(LLVMStructGetTypeAtIndex(s_type, i));
		return LLVMConstNamedStruct(s_type, fields, fc);
	}
	default:
		return NULL;
	}
}

// --- end constant initializer helpers ------------------------------------

void scope_push(KawaCompiler *c, const char *name, LLVMValueRef val,
				LLVMTypeRef type, ASTNode *node) {
	Scope *s = arena_alloc(c->arena, sizeof(Scope));
	s->name = arena_strdup(c->arena, name);
	s->val = val;
	s->type = type;
	s->node = node;
	s->next = c->scope_stack;
	c->scope_stack = s;
}

Scope *scope_find(KawaCompiler *c, const char *name) {
	for (Scope *cur = c->scope_stack; cur; cur = cur->next) {
		if (strcmp(cur->name, name) == 0)
			return cur;
	}
	return NULL;
}

// Best-effort AST type for the VALUE obtained by dereferencing `n`.
// Priority:
//   1. The deref node's own data_type (set by the parser for typed exprs).
//   2. The declared type of the referenced variable (peel one pointer).
//   3. Nested derefs: the value being dereferenced is itself a deref
//		result, so recurse and peel one more pointer level.
static Type *deref_value_type(KawaCompiler *c, ASTNode *n) {
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
LLVMValueRef get_address(KawaCompiler *c, ASTNode *n, LLVMTypeRef *out_type) {
	switch (n->type) {
	case NODE_VAR_REF: {
		Scope *s = scope_find(c, n->data.var_ref.name);
		if (!s) {
			char *v_path = get_var_path(c, n->data.var_ref.name);
			timbr_err("Undefined variable '%s'\n", v_path);
			exit(1);
		}
		if (out_type)
			*out_type = s->type;
		return s->val;
	}

	case NODE_MEMBER_ACCESS: {
		LLVMTypeRef container_type = NULL;
		LLVMValueRef ptr =
			get_address(c, n->data.member_access.object, &container_type);
		if (!ptr || !container_type) {
			timbr_err("Internal error: failed to resolve member access base\n");
			exit(1);
		}

		// `ptr` is a pointer to the container storage. If the container is
		// itself addressed through a pointer (e.g. `p->x` where the deref
		// already yielded the pointee address), the container type is the
		// struct and we GEP straight through `ptr`.
		LLVMTypeRef struct_t = container_type;
		if (LLVMGetTypeKind(struct_t) == LLVMPointerTypeKind)
			struct_t = LLVMGetElementType(struct_t);

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
		LLVMTypeRef elem_type = NULL;
		if (n->data_type)
			elem_type = get_llvm_type(c, n->data_type);

		ASTNode *obj = n->data.index.object;
		LLVMValueRef base;
		LLVMTypeRef elem = elem_type;

		if (obj->type == NODE_VAR_REF) {
			// Direct storage access: arrays are allocas, pointers are
			// allocas holding T*. Either way GEP works without loading the
			// base first -- LLVM lowers both forms.
			Scope *s = scope_find(c, obj->data.var_ref.name);
			if (!s) {
				char *v_path = get_var_path(c, obj->data.var_ref.name);
				timbr_err("Undefined variable '%s'\n", v_path);
				exit(1);
			}
			base = s->val;
			if (!elem) {
				LLVMTypeRef st = s->type;
				if (LLVMGetTypeKind(st) == LLVMArrayTypeKind)
					elem = LLVMGetElementType(st);
				else if (LLVMGetTypeKind(st) == LLVMPointerTypeKind)
					elem = LLVMGetElementType(st);
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
		idx = coerce_value(c, idx, n->data.index.index->data_type,
						   LLVMInt64TypeInContext(c->context), NULL);
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

		// The pointee type: prefer AST info, fall back to peeling the LLVM
		// pointer type of the loaded value.
		Type *val_ast = deref_value_type(c, n);
		LLVMTypeRef pointee = val_ast ? get_llvm_type(c, val_ast) : NULL;
		if (!pointee) {
			// Shape recovery: peel one pointer layer from the storage type
			// (storage is T**, loaded value is T*, pointee is T).
			LLVMTypeRef probe = inner_storage_type;
			if (probe && LLVMGetTypeKind(probe) == LLVMPointerTypeKind) {
				LLVMTypeRef loaded = LLVMGetElementType(probe);
				if (LLVMGetTypeKind(loaded) == LLVMPointerTypeKind)
					pointee = LLVMGetElementType(loaded);
			}
		}
		if (!pointee)
			pointee = LLVMInt32TypeInContext(c->context);

		LLVMTypeRef ptr_t = LLVMPointerType(pointee, 0);
		LLVMValueRef loaded =
			LLVMBuildLoad2(c->builder, ptr_t, inner_addr, "deref_ptr");
		attach_tbaa(c, loaded, ptr_t);

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
LLVMValueRef value_of_lvalue(KawaCompiler *c, ASTNode *n) {
	if (n->type == NODE_AMP) {
		LLVMTypeRef addr_type = NULL;
		LLVMValueRef addr = get_address(c, n->data.deref.expr, &addr_type);
		if (!addr) {
			timbr_err("Cannot take address of a non-lvalue\n");
			exit(1);
		}
		return addr;
	}

	LLVMTypeRef val_type = NULL;
	LLVMValueRef addr = get_address(c, n, &val_type);
	if (!addr || !val_type) {
		timbr_err("Internal error: failed to resolve lvalue\n");
		exit(1);
	}

	LLVMValueRef val = LLVMBuildLoad2(c->builder, val_type, addr, "load");
	attach_tbaa(c, val, val_type);
	return val;
}

// Normalize a condition value to i1. Integer conditions wider than one bit
// become `!= 0`; pointers become null checks.
LLVMValueRef cond_to_bool(KawaCompiler *c, LLVMValueRef cond) {
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
		timbr_err("Condition must be bool, integer or pointer\n");
		exit(1);
	}
}

// Coerce `v` (produced from AST type `src_ast`, may be NULL) to `dst`.
// Sign-aware: signed->signed widening is SExt, everything else ZExt, so
// negative values survive integer promotion.
LLVMValueRef coerce_value(KawaCompiler *c, LLVMValueRef v, Type *src_ast,
						  LLVMTypeRef dst, Type *dst_ast) {
	LLVMTypeRef src = LLVMTypeOf(v);
	if (src == dst)
		return v;

	LLVMTypeKind sk = LLVMGetTypeKind(src);
	LLVMTypeKind dk = LLVMGetTypeKind(dst);

	if (sk == LLVMIntegerTypeKind && dk == LLVMIntegerTypeKind) {
		unsigned sw = LLVMGetIntTypeWidth(src);
		unsigned dw = LLVMGetIntTypeWidth(dst);
		if (dw < sw)
			return LLVMBuildTrunc(c->builder, v, dst, "trunc");
		if (sw < dw) {
			int signed_ext =
				type_is_signed(c, src_ast) && type_is_signed(c, dst_ast);
			return signed_ext ? LLVMBuildSExt(c->builder, v, dst, "sext")
							  : LLVMBuildZExt(c->builder, v, dst, "zext");
		}
		return v;
	}
	if (sk == LLVMIntegerTypeKind &&
		(dk == LLVMFloatTypeKind || dk == LLVMDoubleTypeKind))
		return type_is_signed(c, src_ast)
				   ? LLVMBuildSIToFP(c->builder, v, dst, "sitofp")
				   : LLVMBuildUIToFP(c->builder, v, dst, "uitofp");
	if ((sk == LLVMFloatTypeKind || sk == LLVMDoubleTypeKind) &&
		dk == LLVMIntegerTypeKind)
		return type_is_signed(c, dst_ast)
				   ? LLVMBuildFPToSI(c->builder, v, dst, "fptosi")
				   : LLVMBuildFPToUI(c->builder, v, dst, "fptoui");
	if ((sk == LLVMFloatTypeKind || sk == LLVMDoubleTypeKind) &&
		(dk == LLVMFloatTypeKind || dk == LLVMDoubleTypeKind))
		return sk == dk
				   ? v
				   : (sk == LLVMFloatTypeKind
						  ? LLVMBuildFPExt(c->builder, v, dst, "fpext")
						  : LLVMBuildFPTrunc(c->builder, v, dst, "fptrunc"));
	if (sk == LLVMPointerTypeKind && dk == LLVMPointerTypeKind)
		return LLVMBuildPointerCast(c->builder, v, dst, "ptr_cast");
	if (sk == dk)
		return v;
	// Last resort: bitcast between same-sized types; otherwise the value is
	// incompatible with the destination and that's a real codegen bug.
	if (LLVMGetTypeKind(dst) == LLVMPointerTypeKind ||
		LLVMGetTypeKind(src) == LLVMPointerTypeKind)
		return LLVMBuildPointerCast(c->builder, v, dst, "raw_cast");

	timbr_err("Internal error: cannot coerce value in codegen\n");
	exit(1);
}
