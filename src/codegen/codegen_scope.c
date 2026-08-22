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
