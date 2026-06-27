#include "codegen_internal.h"

char *get_var_path(const char *s) {
	if (!s)
		return NULL;

	size_t len = strlen(s);
	char *out = malloc(len + 1);
	if (!out)
		return NULL;

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
			while (t->kind == TYPE_PTR || t->kind == TYPE_AMP) {
				if (t->inner)
					t = t->inner;
				else
					break;
			}
			if (t->name)
				return t->name;
		}
	}
	if (n->type == NODE_MEMBER_ACCESS) {
		return n->data.member_access.member;
	}
	if (n->type == NODE_DEREF) {
		return resolve_type_name(c, n->data.deref.expr);
	}
	return "unknown";
}

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
	LLVMSetAlignment(alloca_instr, 8);
	LLVMPositionBuilderAtEnd(c->builder, current_block);
	return alloca_instr;
}

void scope_push(KawaCompiler *c, const char *name, LLVMValueRef val,
				LLVMTypeRef type, ASTNode *node) {
	Scope *s = malloc(sizeof(Scope));
	s->name = strdup(name);
	s->val = val;
	s->type = type;
	s->node = node;
	s->next = c->scope_stack;
	c->scope_stack = s;
}

Scope *scope_find(KawaCompiler *c, const char *name) {
	Scope *cur = c->scope_stack;
	while (cur) {
		if (strcmp(cur->name, name) == 0)
			return cur;
		cur = cur->next;
	}
	return NULL;
}

// [FIXED] Correctly resolves address without recursion
LLVMValueRef get_address(KawaCompiler *c, ASTNode *n, LLVMTypeRef *out_type) {
	if (n->type == NODE_DEREF) {
		LLVMValueRef ptr = codegen_expr(c, n->data.deref.expr);
		if (out_type && n->data.deref.expr->data_type &&
			n->data.deref.expr->data_type->inner) {
			*out_type = get_llvm_type(c, n->data.deref.expr->data_type->inner);
		}
		return ptr;
	}
	if (n->type == NODE_VAR_REF) {
		Scope *s = scope_find(c, n->data.var_ref.name);
		if (s) {
			if (out_type)
				*out_type = s->type;
			return s->val;
		}
	}
	if (n->type == NODE_MEMBER_ACCESS) {
		LLVMTypeRef container_type = NULL;
		LLVMValueRef ptr =
			get_address(c, n->data.member_access.object, &container_type);
		if (ptr && container_type) {
			if (LLVMGetTypeKind(container_type) == LLVMPointerTypeKind) {
			}
			int idx = get_field_index(c, container_type,
									  n->data.member_access.member);
			LLVMValueRef field_addr = LLVMBuildStructGEP2(
				c->builder, container_type, ptr, idx, "fld_addr");
			if (out_type)
				*out_type = get_field_type(c, container_type,
										   n->data.member_access.member);
			return field_addr;
		}
	}
	return NULL;
}
