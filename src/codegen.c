#include "codegen.h"
#include "arena.h"
#include "ast.h"
#include "timbr.h"
#include <_inttypes.h>
#include <lexer.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Metadata Cache ---
static LLVMValueRef tbaa_root = NULL;
static LLVMValueRef tbaa_scalar = NULL;
static LLVMValueRef tbaa_ptr = NULL;
static LLVMValueRef tbaa_int = NULL;

typedef struct StructDef {
	char *name;
	LLVMTypeRef type;
	struct {
		char *name;
		LLVMTypeRef type;
	} fields[32];
	int field_count;
	struct StructDef *next;
} StructDef;

StructDef *struct_defs = NULL;

typedef struct AliasDef {
	char *name;
	Type *target;
	struct AliasDef *next;
} AliasDef;

AliasDef *alias_defs = NULL;

void register_alias(const char *name, Type *target) {
	AliasDef *ad = malloc(sizeof(AliasDef));
	ad->name = strdup(name);
	ad->target = target;
	ad->next = alias_defs;
	alias_defs = ad;
}

Type *resolve_alias_type(const char *name) {
	AliasDef *cur = alias_defs;
	while (cur) {
		if (strcmp(cur->name, name) == 0)
			return cur->target;
		cur = cur->next;
	}
	return NULL;
}

Scope *scope_find(KawaCompiler *c, const char *name);
LLVMValueRef codegen_expr(KawaCompiler *c, ASTNode *n);

char *get_var_path(const char *s) {
	if (!s)
		return NULL;

	size_t len = strlen(s);

	// Allocate output buffer same size or smaller
	char *out = malloc(len + 1);
	if (!out)
		return NULL;

	size_t i = 0, j = 0;

	while (i < len) {
		// Check for "__" (non-overlapping)
		if (i + 1 < len && s[i] == '_' && s[i + 1] == '_') {
			int at_start = (i == 0);
			int at_end = (i + 2 == len);

			if (!at_start && !at_end) {
				// Replace with '.'
				out[j++] = '.';
				i += 2;
				continue;
			}
		}

		// Normal copy
		out[j++] = s[i++];
	}

	out[j] = '\0';
	return out;
}

const char *resolve_type_name(KawaCompiler *c, ASTNode *n) {
	if (n->type == NODE_VAR_REF) {
		Scope *s = scope_find(c, n->data.var_ref.name);
		if (s && s->node && s->node->data_type && s->node->data_type->name) {
			return s->node->data_type->name;
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

void register_struct(const char *name, LLVMTypeRef type) {
	StructDef *sd = malloc(sizeof(StructDef));
	sd->name = strdup(name);
	sd->type = type;
	sd->field_count = 0;
	sd->next = struct_defs;
	struct_defs = sd;
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

int get_field_index(KawaCompiler *c, LLVMTypeRef struct_type,
					const char *field_name) {
	StructDef *sd = struct_defs;
	while (sd) {
		if (sd->type == struct_type) {
			for (int i = 0; i < sd->field_count; i++) {
				if (strcmp(sd->fields[i].name, field_name) == 0)
					return i;
			}
		}
		sd = sd->next;
	}
	return 0;
}

LLVMTypeRef get_field_type(KawaCompiler *c, LLVMTypeRef struct_type,
						   const char *field_name) {
	StructDef *sd = struct_defs;
	while (sd) {
		if (sd->type == struct_type) {
			for (int i = 0; i < sd->field_count; i++) {
				if (strcmp(sd->fields[i].name, field_name) == 0)
					return sd->fields[i].type;
			}
		}
		sd = sd->next;
	}
	return LLVMInt32TypeInContext(c->context);
}
LLVMTypeRef get_llvm_type(KawaCompiler *c, Type *t);
// [FIXED] Correctly resolves address without recursion
LLVMValueRef get_address(KawaCompiler *c, ASTNode *n, LLVMTypeRef *out_type) {
	if (n->type == NODE_DEREF) {
		// The "address" of *ptr is just the value of ptr
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
			// Unwrap pointer types if we have a Pointer to Struct
			if (LLVMGetTypeKind(container_type) == LLVMPointerTypeKind) {
				// Should define struct types properly, but if opaque, we rely
				// on context
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

LLVMTypeRef get_llvm_type(KawaCompiler *c, Type *t) {
	if (!t)
		return LLVMInt32TypeInContext(c->context);

	if (t->kind == TYPE_PTR) {
		// Recursively resolve inner type so Car* becomes %Car* (struct ptr),
		// not i8*
		return LLVMPointerType(get_llvm_type(c, t->inner), 0);
	}
	if (t->kind == TYPE_HANDLE) {
		return LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
	}
	switch (t->kind) {
	case TYPE_VOID:
		return LLVMVoidTypeInContext(c->context);
	case TYPE_BOOL:
		return LLVMInt1TypeInContext(c->context);
	case TYPE_CHAR:
		return LLVMInt8TypeInContext(c->context);
	case TYPE_I8:
	case TYPE_U8:
		return LLVMInt8TypeInContext(c->context);
	case TYPE_I16:
	case TYPE_U16:
		return LLVMInt16TypeInContext(c->context);
	case TYPE_I32:
	case TYPE_U32:
		return LLVMInt32TypeInContext(c->context);
	case TYPE_I64:
	case TYPE_U64:
		return LLVMInt64TypeInContext(c->context);
	case TYPE_F32:
		return LLVMFloatTypeInContext(c->context);
	case TYPE_F64:
		return LLVMDoubleTypeInContext(c->context);
	case TYPE_SET: {
		LLVMTypeRef elems[] = {
			LLVMPointerType(LLVMInt32TypeInContext(c->context), 0),
			LLVMInt64TypeInContext(c->context),
			LLVMInt64TypeInContext(c->context)};
		return LLVMStructTypeInContext(c->context, elems, 3, 0);
	}
	case TYPE_STRUCT: {
		Type *alias_target = resolve_alias_type(t->name);
		if (alias_target) {
			return get_llvm_type(c, alias_target);
		}
		LLVMTypeRef struct_t = LLVMGetTypeByName(c->module, t->name);
		if (!struct_t) {
			struct_t = LLVMStructCreateNamed(c->context, t->name);
		}
		return struct_t;
	}
	default:
		return LLVMInt32TypeInContext(c->context);
	}
}

void codegen_stmt(KawaCompiler *c, ASTNode *n);

void trigger_orbit_updates(KawaCompiler *c, ASTNode *origin_node) {
	if (!origin_node || !origin_node->dependents)
		return;
	Dependency *dep = origin_node->dependents;
	while (dep) {
		LLVMValueRef new_val = codegen_expr(c, dep->logic_expr);
		Scope *s = scope_find(c, dep->dependent_node->data.var_decl.name);
		if (s) {
			LLVMValueRef store = LLVMBuildStore(c->builder, new_val, s->val);
			attach_tbaa(c, store, s->type);
		}
		trigger_orbit_updates(c, dep->dependent_node);
		dep = dep->next;
	}
}

LLVMValueRef codegen_expr(KawaCompiler *c, ASTNode *n) {
	if (!n)
		return LLVMConstInt(LLVMInt32TypeInContext(c->context), 0, 0);

	if (n->type == NODE_LITERAL) {
		if (n->data_type && (n->data_type->kind == TYPE_F32 ||
							 n->data_type->kind == TYPE_F64)) {
			return LLVMConstReal(get_llvm_type(c, n->data_type),
								 (double)n->data.literal.f_val);
		}
		return LLVMConstInt(LLVMInt32TypeInContext(c->context),
							n->data.literal.i_val, 0);
	}
	if (n->type == NODE_STRING_LIT)
		return LLVMBuildGlobalStringPtr(c->builder, n->data.str_lit.s_val,
										"str");

	if (n->type == NODE_VAR_REF) {
		Scope *s = scope_find(c, n->data.var_ref.name);
		if (!s) {
			char *v_path = get_var_path(n->data.var_ref.name);
			timbr_err("Undefined variable '%s'\n", v_path);
			free(v_path);
			exit(1);
		}
		LLVMValueRef load =
			LLVMBuildLoad2(c->builder, s->type, s->val, n->data.var_ref.name);
		attach_tbaa(c, load, s->type);
		return load;
	}
	if (n->type == NODE_DEREF) {
		LLVMValueRef ptr = codegen_expr(c, n->data.deref.expr);
		LLVMTypeRef elem_type = get_llvm_type(c, n->data_type);
		// Fallback if AST type is missing
		if (!elem_type && n->data.deref.expr->data_type &&
			n->data.deref.expr->data_type->inner) {
			elem_type = get_llvm_type(c, n->data.deref.expr->data_type->inner);
		}
		LLVMValueRef val =
			LLVMBuildLoad2(c->builder, elem_type, ptr, "deref_val");
		attach_tbaa(c, val, elem_type);
		return val;
	}
	if (n->type == NODE_CALL) {
		char func_name[256];
		memset(func_name, 0, 256);
		if (n->data.call.callee->type == NODE_VAR_REF) {
			strcpy(func_name, n->data.call.callee->data.var_ref.name);
		} else if (n->data.call.callee->type == NODE_MEMBER_ACCESS) {
			ASTNode *obj = n->data.call.callee->data.member_access.object;
			char *member = n->data.call.callee->data.member_access.member;
			if (obj->type == NODE_VAR_REF &&
				strcmp(obj->data.var_ref.name, "stdc") == 0) {
				strcpy(func_name, member);
			} else if (obj->type == NODE_VAR_REF) {
				sprintf(func_name, "%s_%s", obj->data.var_ref.name, member);
			} else {
				const char *type_name = resolve_type_name(c, obj);
				sprintf(func_name, "%s_%s", type_name, member);
			}
		}

		LLVMValueRef fn = LLVMGetNamedFunction(c->module, func_name);
		if (!fn) {
			if (strcmp(func_name, "printf") == 0) {
				LLVMTypeRef args[] = {
					LLVMPointerType(LLVMInt8TypeInContext(c->context), 0)};
				LLVMTypeRef ft = LLVMFunctionType(
					LLVMInt32TypeInContext(c->context), args, 1, 1);
				fn = LLVMAddFunction(c->module, "printf", ft);
			} else if (strcmp(func_name, "malloc") == 0) {
				fn = c->malloc_fn;
			} else if (strcmp(func_name, "free") == 0) {
				fn = c->free_fn;
			} else {
				char *f_path = get_var_path(func_name);
				timbr_err("Undefined function: %s\n", f_path);
				free(f_path);
				exit(1);
			}
		}

		int arg_count = 0;
		ASTNode *arg_node = n->data.call.args;
		while (arg_node) {
			arg_count++;
			arg_node = arg_node->next;
		}

		LLVMValueRef *llvm_args = malloc(sizeof(LLVMValueRef) * arg_count);
		arg_node = n->data.call.args;
		LLVMTypeRef func_type = LLVMGlobalGetValueType(fn);
		int param_count = LLVMCountParamTypes(func_type);
		LLVMTypeRef *param_types = malloc(sizeof(LLVMTypeRef) * param_count);
		LLVMGetParamTypes(func_type, param_types);

		for (int i = 0; i < arg_count; i++) {
			LLVMValueRef val = codegen_expr(c, arg_node);

			// [FIX] Auto-Spill Struct to Stack if Pointer Expected (Handle
			// 'self')
			if (i < param_count) {
				LLVMTypeRef expected = param_types[i];
				LLVMTypeRef actual = LLVMTypeOf(val);

				// If function expects Ptr (e.g. i8* self) but we have
				// Struct Value (User)
				if (LLVMGetTypeKind(expected) == LLVMPointerTypeKind &&
					LLVMGetTypeKind(actual) == LLVMStructTypeKind) {

					LLVMValueRef temp_alloc =
						create_entry_block_alloca(c, actual, "self_temp");
					LLVMBuildStore(c->builder, val, temp_alloc);
					// Cast to expected pointer type (likely i8*)
					val = LLVMBuildBitCast(c->builder, temp_alloc, expected,
										   "self_ptr");
				}
			}

			if (i >= param_count) {
				// [FIX] 2. Now it is safe to check TypeOf(val)
				LLVMTypeRef val_type = LLVMTypeOf(val);
				if (LLVMGetTypeKind(val_type) == LLVMIntegerTypeKind) {
					unsigned width = LLVMGetIntTypeWidth(val_type);
					if (width < 32) {
						// bool(1) -> i32, char(8) -> i32
						val = LLVMBuildZExt(c->builder, val,
											LLVMInt32TypeInContext(c->context),
											"vararg_prom");
					}
				}
				if (LLVMGetTypeKind(val_type) == LLVMFloatTypeKind) {
					val = LLVMBuildFPExt(c->builder, val,
										 LLVMDoubleTypeInContext(c->context),
										 "float_prom");
				}
			}
			llvm_args[i] = val;
			arg_node = arg_node->next;
		}
		free(param_types);
		LLVMValueRef call_res =
			LLVMBuildCall2(c->builder, func_type, fn, llvm_args, arg_count, "");
		free(llvm_args);
		return call_res;
	}
	if (n->type == NODE_STRUCT_LITERAL) {
		LLVMTypeRef s_type = get_llvm_type(c, n->data_type);
		if (!s_type)
			return LLVMConstNull(LLVMInt32TypeInContext(c->context));

		LLVMValueRef alloca = create_entry_block_alloca(c, s_type, "lit");
		StructInitItem *item = n->data.struct_lit.items;
		int idx = 0;
		while (item) {
			LLVMValueRef val = codegen_expr(c, item->value);
			int field_idx = idx;
			if (item->field_name) {
				field_idx = get_field_index(c, s_type, item->field_name);
			}
			LLVMValueRef gep = LLVMBuildStructGEP2(c->builder, s_type, alloca,
												   field_idx, "fld");
			LLVMBuildStore(c->builder, val, gep);
			if (!item->field_name)
				idx++;
			item = item->next;
		}
		return LLVMBuildLoad2(c->builder, s_type, alloca, "lit_val");
	}
	if (n->type == NODE_BINARY_OP) {
		LLVMValueRef l = codegen_expr(c, n->data.bin_op.left);
		LLVMValueRef r = codegen_expr(c, n->data.bin_op.right);
		int is_float = (LLVMGetTypeKind(LLVMTypeOf(l)) == LLVMFloatTypeKind ||
						LLVMGetTypeKind(LLVMTypeOf(l)) == LLVMDoubleTypeKind);
		LLVMValueRef res;
		switch (n->data.bin_op.op) {
		case TOK_PLUS:
			if (is_float) {
				res = LLVMBuildFAdd(c->builder, l, r, "fadd");
				set_fast_math(res);
			} else {
				res = LLVMBuildNSWAdd(c->builder, l, r, "add");
			}
			return res;
		case TOK_MINUS:
			if (is_float) {
				res = LLVMBuildFSub(c->builder, l, r, "fsub");
				set_fast_math(res);
			} else {
				res = LLVMBuildNSWSub(c->builder, l, r, "sub");
			}
			return res;
		case TOK_STAR:
			if (is_float) {
				res = LLVMBuildFMul(c->builder, l, r, "fmul");
				set_fast_math(res);
			} else {
				res = LLVMBuildNSWMul(c->builder, l, r, "mul");
			}
			return res;
		case TOK_SLASH:
			if (is_float) {
				res = LLVMBuildFDiv(c->builder, l, r, "fdiv");
				set_fast_math(res);
				return res;
			} else {
				return LLVMBuildSDiv(c->builder, l, r, "div");
			}
		case TOK_LANGLE:
			return is_float
					   ? LLVMBuildFCmp(c->builder, LLVMRealOLT, l, r, "flt")
					   : LLVMBuildICmp(c->builder, LLVMIntSLT, l, r, "lt");
		case TOK_RANGLE:
			return is_float
					   ? LLVMBuildFCmp(c->builder, LLVMRealOGT, l, r, "fgt")
					   : LLVMBuildICmp(c->builder, LLVMIntSGT, l, r, "gt");
		case TOK_LEQ:
			return is_float
					   ? LLVMBuildFCmp(c->builder, LLVMRealOLE, l, r, "fle")
					   : LLVMBuildICmp(c->builder, LLVMIntSLE, l, r, "le");
		case TOK_REQ:
			return is_float
					   ? LLVMBuildFCmp(c->builder, LLVMRealOGE, l, r, "fge")
					   : LLVMBuildICmp(c->builder, LLVMIntSGE, l, r, "ge");
		case TOK_ISEQ:
			return is_float
					   ? LLVMBuildFCmp(c->builder, LLVMRealOEQ, l, r, "feq")
					   : LLVMBuildICmp(c->builder, LLVMIntEQ, l, r, "eq");
		default:
			return l;
		}
	}
	if (n->type == NODE_SET_POUR) {
		// 1. Resolve the address of the Set struct (the LHS of ~=)
		LLVMTypeRef ignored;
		LLVMValueRef set_ptr =
			get_address(c, n->data.set_pour.target, &ignored);
		LLVMValueRef val_to_add = codegen_expr(c, n->data.set_pour.value);

		// Reconstruct Set Struct Type: { i32*, i64, i64 }
		// Corresponds to the definition in get_llvm_type for TYPE_SET
		LLVMTypeRef i32_ptr_t =
			LLVMPointerType(LLVMInt32TypeInContext(c->context), 0);
		LLVMTypeRef i64_t = LLVMInt64TypeInContext(c->context);
		LLVMTypeRef set_struct_t = LLVMStructTypeInContext(
			c->context, (LLVMTypeRef[]){i32_ptr_t, i64_t, i64_t}, 3, 0);

		// 2. Load Buffer, Count, and Capacity pointers (GEP)
		LLVMValueRef buf_gep =
			LLVMBuildStructGEP2(c->builder, set_struct_t, set_ptr, 0, "buf_p");
		LLVMValueRef cnt_gep =
			LLVMBuildStructGEP2(c->builder, set_struct_t, set_ptr, 1, "cnt_p");
		LLVMValueRef cap_gep =
			LLVMBuildStructGEP2(c->builder, set_struct_t, set_ptr, 2, "cap_p");

		LLVMValueRef cur_cnt =
			LLVMBuildLoad2(c->builder, i64_t, cnt_gep, "cur_cnt");
		LLVMValueRef cur_cap =
			LLVMBuildLoad2(c->builder, i64_t, cap_gep, "cur_cap");

		// 3. Check if resize is needed: if (cnt >= cap)
		LLVMValueRef is_full =
			LLVMBuildICmp(c->builder, LLVMIntUGE, cur_cnt, cur_cap, "is_full");

		LLVMBasicBlockRef grow_bb =
			LLVMAppendBasicBlock(c->current_func, "set_grow");
		LLVMBasicBlockRef append_bb =
			LLVMAppendBasicBlock(c->current_func, "set_append");

		LLVMBuildCondBr(c->builder, is_full, grow_bb, append_bb);

		// --- GROW BLOCK (Realloc) ---
		LLVMPositionBuilderAtEnd(c->builder, grow_bb);

		// New Capacity = Capacity * 2
		LLVMValueRef new_cap = LLVMBuildMul(
			c->builder, cur_cap, LLVMConstInt(i64_t, 2, 0), "new_cap");
		// New Size in Bytes = new_cap * 4 (since elements are i32)
		LLVMValueRef new_bytes = LLVMBuildMul(
			c->builder, new_cap, LLVMConstInt(i64_t, 4, 0), "new_bytes");

		LLVMValueRef old_buf =
			LLVMBuildLoad2(c->builder, i32_ptr_t, buf_gep, "old_buf");
		// Cast to i8* for realloc
		LLVMValueRef old_buf_void = LLVMBuildBitCast(
			c->builder, old_buf,
			LLVMPointerType(LLVMInt8TypeInContext(c->context), 0), "void_ptr");

		// Call realloc(void* ptr, i64 size)
		LLVMValueRef new_mem = LLVMBuildCall2(
			c->builder, c->realloc_type, c->realloc_fn,
			(LLVMValueRef[]){old_buf_void, new_bytes}, 2, "new_mem");

		// Cast back to i32*
		LLVMValueRef new_buf =
			LLVMBuildBitCast(c->builder, new_mem, i32_ptr_t, "new_buf_cast");

		// Update struct fields
		LLVMBuildStore(c->builder, new_buf, buf_gep);
		LLVMBuildStore(c->builder, new_cap, cap_gep);
		LLVMBuildBr(c->builder, append_bb);

		// --- APPEND BLOCK ---
		LLVMPositionBuilderAtEnd(c->builder, append_bb);

		// Reload buffer (it might have changed in grow_bb)
		LLVMValueRef final_buf =
			LLVMBuildLoad2(c->builder, i32_ptr_t, buf_gep, "final_buf");

		// buffer[count] = value
		LLVMValueRef slot =
			LLVMBuildGEP2(c->builder, LLVMInt32TypeInContext(c->context),
						  final_buf, &cur_cnt, 1, "slot");
		LLVMBuildStore(c->builder, val_to_add, slot);

		// count++
		LLVMValueRef next_cnt = LLVMBuildAdd(
			c->builder, cur_cnt, LLVMConstInt(i64_t, 1, 0), "next_cnt");
		LLVMBuildStore(c->builder, next_cnt, cnt_gep);

		return val_to_add;
	}
	if (n->type == NODE_MEMBER_ACCESS) {
		LLVMTypeRef field_type = NULL;
		// [FIX] Use corrected get_address
		LLVMValueRef ptr = get_address(c, n, &field_type);
		if (ptr && field_type) {
			LLVMValueRef val =
				LLVMBuildLoad2(c->builder, field_type, ptr, "fld_val");
			attach_tbaa(c, val, field_type);
			return val;
		}
		return LLVMConstInt(LLVMInt32TypeInContext(c->context), 0, 0);
	}
	if (n->type == NODE_SET_LITERAL) {
		LLVMTypeRef set_t = get_llvm_type(c, n->data_type);
		LLVMValueRef set_alloca =
			create_entry_block_alloca(c, set_t, "set_tmp");
		int count = 0;
		ASTNode *cur = n->data.set_lit.items;
		while (cur) {
			count++;
			cur = cur->next;
		}
		LLVMValueRef size = LLVMConstInt(LLVMInt64TypeInContext(c->context),
										 count > 0 ? count * 4 : 4, 0);
		LLVMValueRef buf_void = LLVMBuildCall2(
			c->builder, c->malloc_type, c->malloc_fn, &size, 1, "malloc");
		LLVMValueRef buf = LLVMBuildBitCast(
			c->builder, buf_void,
			LLVMPointerType(LLVMInt32TypeInContext(c->context), 0), "buf_cast");
		cur = n->data.set_lit.items;
		int idx = 0;
		while (cur) {
			LLVMValueRef val = codegen_expr(c, cur);
			LLVMValueRef gep = LLVMBuildGEP2(
				c->builder, LLVMInt32TypeInContext(c->context), buf,
				(LLVMValueRef[]){
					LLVMConstInt(LLVMInt64TypeInContext(c->context), idx++, 0)},
				1, "ptr");
			LLVMValueRef store = LLVMBuildStore(c->builder, val, gep);
			attach_tbaa(c, store, LLVMInt32TypeInContext(c->context));
			cur = cur->next;
		}
		LLVMBuildStore(
			c->builder, buf,
			LLVMBuildStructGEP2(c->builder, set_t, set_alloca, 0, ""));
		LLVMBuildStore(
			c->builder,
			LLVMConstInt(LLVMInt64TypeInContext(c->context), count, 0),
			LLVMBuildStructGEP2(c->builder, set_t, set_alloca, 1, ""));
		LLVMBuildStore(
			c->builder,
			LLVMConstInt(LLVMInt64TypeInContext(c->context),
						 count > 0 ? count : 1, 0),
			LLVMBuildStructGEP2(c->builder, set_t, set_alloca, 2, ""));
		LLVMValueRef set_load =
			LLVMBuildLoad2(c->builder, set_t, set_alloca, "set_load");
		attach_tbaa(c, set_load, set_t);
		return set_load;
	}
	if (n->type == NODE_SIP) {
		LLVMValueRef hdl = codegen_expr(c, n->data.sip.handle);
		LLVMValueRef is_done = LLVMBuildCall2(c->builder, c->coro_done_type,
											  c->coro_done, &hdl, 1, "is_done");
		LLVMBasicBlockRef resume_bb =
			LLVMAppendBasicBlock(c->current_func, "sip_resume");
		LLVMBasicBlockRef cont_bb =
			LLVMAppendBasicBlock(c->current_func, "sip_cont");
		LLVMBuildCondBr(c->builder, is_done, cont_bb, resume_bb);
		LLVMPositionBuilderAtEnd(c->builder, resume_bb);
		LLVMValueRef resume_fn = c->coro_resume;
		if (!resume_fn) {
			LLVMTypeRef args[] = {
				LLVMPointerType(LLVMInt8TypeInContext(c->context), 0)};
			c->coro_resume_type =
				LLVMFunctionType(LLVMVoidTypeInContext(c->context), args, 1, 0);
			resume_fn = LLVMAddFunction(c->module, "llvm.coro.resume",
										c->coro_resume_type);
			c->coro_resume = resume_fn;
		}
		LLVMBuildCall2(c->builder, c->coro_resume_type, resume_fn, &hdl, 1, "");
		LLVMBuildBr(c->builder, cont_bb);
		LLVMPositionBuilderAtEnd(c->builder, cont_bb);
		int prom_index = c->drip_promise_index;
		LLVMValueRef promise_ptr_void = LLVMBuildCall2(
			c->builder, c->coro_promise_type, c->coro_promise,
			(LLVMValueRef[]){
				hdl,
				LLVMConstInt(LLVMInt32TypeInContext(c->context), prom_index, 0),
				LLVMConstInt(LLVMInt1TypeInContext(c->context), 0, 0)},
			3, "prom_ptr_void");
		LLVMValueRef promise_ptr = LLVMBuildBitCast(
			c->builder, promise_ptr_void,
			LLVMPointerType(LLVMInt32TypeInContext(c->context), 0), "prom_ptr");
		LLVMValueRef val =
			LLVMBuildLoad2(c->builder, LLVMInt32TypeInContext(c->context),
						   promise_ptr, "sip_val");
		LLVMSetVolatile(val, 1);
		attach_tbaa(c, val, LLVMInt32TypeInContext(c->context));
		return val;
	}
	if (n->type == NODE_CAST) {
		LLVMValueRef val = codegen_expr(c, n->data.cast.val);
		LLVMTypeRef dest_type = get_llvm_type(c, n->data_type);
		LLVMTypeRef src_type = LLVMTypeOf(val);
		LLVMTypeKind src_kind = LLVMGetTypeKind(src_type);
		LLVMTypeKind dest_kind = LLVMGetTypeKind(dest_type);
		if (src_kind == LLVMIntegerTypeKind &&
			dest_kind == LLVMIntegerTypeKind) {
			unsigned src_width = LLVMGetIntTypeWidth(src_type);
			unsigned dest_width = LLVMGetIntTypeWidth(dest_type);
			if (src_width == dest_width)
				return val;
			if (dest_width < src_width) {
				return LLVMBuildTrunc(c->builder, val, dest_type, "trunc");
			} else {
				return LLVMBuildZExt(c->builder, val, dest_type, "zext");
			}
		}
		if ((src_kind == LLVMFloatTypeKind || src_kind == LLVMDoubleTypeKind) &&
			dest_kind == LLVMIntegerTypeKind) {
			return LLVMBuildFPToUI(c->builder, val, dest_type, "fptoui");
		}
		if (src_kind == LLVMIntegerTypeKind &&
			(dest_kind == LLVMFloatTypeKind ||
			 dest_kind == LLVMDoubleTypeKind)) {
			return LLVMBuildUIToFP(c->builder, val, dest_type, "uitofp");
		}
		if (src_kind == LLVMPointerTypeKind &&
			dest_kind == LLVMPointerTypeKind) {
			return LLVMBuildBitCast(c->builder, val, dest_type, "ptr_cast");
		}
		return LLVMBuildBitCast(c->builder, val, dest_type, "raw_cast");
	}
	if (n->type == NODE_BREW) {
		char task_name[64];
		sprintf(task_name, "kawa_task_%d", c->lambda_counter++);
		LLVMBasicBlockRef old_block = LLVMGetInsertBlock(c->builder);
		LLVMValueRef old_func = c->current_func;
		LLVMTypeRef old_ret_type = c->current_ret_type;
		Scope *old_scope = c->scope_stack;
		int was_in_coroutine = c->in_coroutine;
		LLVMBasicBlockRef old_cleanup = c->coro_cleanup_block;
		LLVMBasicBlockRef old_suspend = c->coro_suspend_block;
		LLVMValueRef old_hdl = c->current_coro_hdl;
		LLVMValueRef old_prom = c->current_promise_ptr;
		LLVMTypeRef ret_type =
			LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
		LLVMTypeRef task_type = LLVMFunctionType(ret_type, NULL, 0, 0);
		LLVMValueRef task_func =
			LLVMAddFunction(c->module, task_name, task_type);
		unsigned kind_id =
			LLVMGetEnumAttributeKindForName("presplitcoroutine", 17);
		LLVMAddAttributeAtIndex(
			task_func, LLVMAttributeFunctionIndex,
			LLVMCreateEnumAttribute(c->context, kind_id, 0));
		c->current_func = task_func;
		c->current_ret_type = ret_type;
		c->in_coroutine = 1;
		LLVMBasicBlockRef entry = LLVMAppendBasicBlock(task_func, "entry");
		LLVMPositionBuilderAtEnd(c->builder, entry);
		LLVMValueRef promise_alloca = create_entry_block_alloca(
			c, LLVMInt32TypeInContext(c->context), "promise_storage");
		c->current_promise_ptr = promise_alloca;
		LLVMValueRef promise_void = LLVMBuildBitCast(
			c->builder, promise_alloca,
			LLVMPointerType(LLVMInt8TypeInContext(c->context), 0), "prom_void");
		LLVMValueRef null_ptr = LLVMConstNull(
			LLVMPointerType(LLVMInt8TypeInContext(c->context), 0));
		LLVMValueRef id = LLVMBuildCall2(
			c->builder, c->coro_id_type, c->coro_id,
			(LLVMValueRef[]){LLVMConstInt(LLVMInt32TypeInContext(c->context),
										  c->brew_promise_index, 0),
							 promise_void, null_ptr, null_ptr},
			4, "id");
		LLVMValueRef need_alloc =
			LLVMBuildCall2(c->builder, c->coro_alloc_type, c->coro_alloc, &id,
						   1, "need_alloc");
		LLVMValueRef size = LLVMBuildCall2(c->builder, c->coro_size_type,
										   c->coro_size, NULL, 0, "size");
		LLVMBasicBlockRef alloc_bb = LLVMAppendBasicBlock(task_func, "alloc");
		LLVMBasicBlockRef cont_bb =
			LLVMAppendBasicBlock(task_func, "alloc_cont");
		LLVMBuildCondBr(c->builder, need_alloc, alloc_bb, cont_bb);
		LLVMPositionBuilderAtEnd(c->builder, alloc_bb);
		LLVMValueRef malloc_ptr = LLVMBuildCall2(
			c->builder, c->malloc_type, c->malloc_fn, &size, 1, "coro_mem");
		LLVMBuildBr(c->builder, cont_bb);
		LLVMPositionBuilderAtEnd(c->builder, cont_bb);
		LLVMValueRef phi = LLVMBuildPhi(
			c->builder, LLVMPointerType(LLVMInt8TypeInContext(c->context), 0),
			"mem_phi");
		LLVMAddIncoming(phi, (LLVMValueRef[]){malloc_ptr, null_ptr},
						(LLVMBasicBlockRef[]){alloc_bb, entry}, 2);
		LLVMValueRef hdl =
			LLVMBuildCall2(c->builder, c->coro_begin_type, c->coro_begin,
						   (LLVMValueRef[]){id, phi}, 2, "hdl");
		c->current_coro_hdl = hdl;
		LLVMValueRef suspend = LLVMBuildCall2(
			c->builder, c->coro_suspend_type, c->coro_suspend,
			(LLVMValueRef[]){
				LLVMConstNull(LLVMTokenTypeInContext(c->context)),
				LLVMConstInt(LLVMInt1TypeInContext(c->context), 0, 0)},
			2, "suspend");
		LLVMBasicBlockRef suspend_bb =
			LLVMAppendBasicBlock(task_func, "suspend");
		LLVMBasicBlockRef resume_bb = LLVMAppendBasicBlock(task_func, "resume");
		LLVMBasicBlockRef cleanup_bb =
			LLVMAppendBasicBlock(task_func, "cleanup");
		c->coro_cleanup_block = cleanup_bb;
		c->coro_suspend_block = suspend_bb;
		LLVMValueRef sw = LLVMBuildSwitch(c->builder, suspend, suspend_bb, 2);
		LLVMAddCase(sw, LLVMConstInt(LLVMInt8TypeInContext(c->context), 0, 0),
					resume_bb);
		LLVMAddCase(sw, LLVMConstInt(LLVMInt8TypeInContext(c->context), 1, 0),
					cleanup_bb);
		LLVMPositionBuilderAtEnd(c->builder, resume_bb);
		c->scope_stack = NULL;
		codegen_stmt(c, n->data.brew.body);
		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder))) {
			LLVMValueRef final_suspend = LLVMBuildCall2(
				c->builder, c->coro_suspend_type, c->coro_suspend,
				(LLVMValueRef[]){
					LLVMConstNull(LLVMTokenTypeInContext(c->context)),
					LLVMConstInt(LLVMInt1TypeInContext(c->context), 1, 0)},
				2, "final");
			LLVMBasicBlockRef final_cleanup_bb = c->coro_cleanup_block;
			LLVMValueRef final_sw =
				LLVMBuildSwitch(c->builder, final_suspend, final_cleanup_bb, 2);
			LLVMAddCase(final_sw,
						LLVMConstInt(LLVMInt8TypeInContext(c->context), 0, 0),
						final_cleanup_bb);
			LLVMAddCase(final_sw,
						LLVMConstInt(LLVMInt8TypeInContext(c->context), 1, 0),
						final_cleanup_bb);
		}
		LLVMPositionBuilderAtEnd(c->builder, cleanup_bb);
		LLVMBuildCall2(
			c->builder, c->coro_end_type, c->coro_end,
			(LLVMValueRef[]){
				null_ptr,
				LLVMConstInt(LLVMInt1TypeInContext(c->context), 0, 0)},
			2, "");
		LLVMBuildBr(c->builder, suspend_bb);
		LLVMPositionBuilderAtEnd(c->builder, suspend_bb);
		LLVMBuildRet(c->builder, hdl);
		LLVMPositionBuilderAtEnd(c->builder, old_block);
		c->current_func = old_func;
		c->current_ret_type = old_ret_type;
		c->scope_stack = old_scope;
		c->in_coroutine = was_in_coroutine;
		c->coro_cleanup_block = old_cleanup;
		c->coro_suspend_block = old_suspend;
		c->current_coro_hdl = old_hdl;
		c->current_promise_ptr = old_prom;
		LLVMValueRef task_handle = LLVMBuildCall2(
			c->builder, task_type, task_func, NULL, 0, "task_hdl");
		LLVMValueRef md_str = LLVMMDStringInContext(c->context, "brew", 4);
		LLVMValueRef md = LLVMMDNodeInContext(c->context, &md_str, 1);
		unsigned KIND =
			LLVMGetMDKindID("kawa.coro.kind", strlen("kawa.coro.kind"));
		LLVMSetMetadata(task_handle, KIND, md);
		return task_handle;
	}
	return LLVMConstNull(LLVMInt32TypeInContext(c->context));
}

void codegen_stmt(KawaCompiler *c, ASTNode *n) {
	if (!n)
		return;
	if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder)))
		return;

	if (n->type == NODE_CALL) {
		codegen_expr(c, n);
		return;
	}
	if (n->type == NODE_SET_POUR) {
		codegen_expr(c, n);
		return;
	}
	if (n->type == NODE_BLOCK) {
		ASTNode *s = n->data.block.stmts;
		while (s) {
			codegen_stmt(c, s);
			s = s->next;
		}
	} else if (n->type == NODE_VAR_DECL) {
		LLVMTypeRef var_type = get_llvm_type(c, n->data_type);
		LLVMValueRef val_ptr =
			create_entry_block_alloca(c, var_type, n->data.var_decl.name);
		scope_push(c, n->data.var_decl.name, val_ptr, var_type, n);
		LLVMValueRef init_val = NULL;
		if (n->data.var_decl.init) {
			// [FIX] Propagate Decl Type to Struct Literal Init
			// Otherwise literal defaults to i32, causing GEP errors
			if (n->data.var_decl.init->type == NODE_STRUCT_LITERAL &&
				!n->data.var_decl.init->data_type) {
				n->data.var_decl.init->data_type = n->data_type;
			}

			init_val = codegen_expr(c, n->data.var_decl.init);
			// [FIX] Auto-truncate or extend if init type doesn't match var
			// type (e.g. u32 literal 75 to u8 char)
			LLVMTypeRef init_type = LLVMTypeOf(init_val);
			if (LLVMGetTypeKind(init_type) == LLVMIntegerTypeKind &&
				LLVMGetTypeKind(var_type) == LLVMIntegerTypeKind) {
				unsigned iw = LLVMGetIntTypeWidth(init_type);
				unsigned vw = LLVMGetIntTypeWidth(var_type);
				if (iw > vw)
					init_val =
						LLVMBuildTrunc(c->builder, init_val, var_type, "trunc");
				if (iw < vw)
					init_val =
						LLVMBuildZExt(c->builder, init_val, var_type, "zext");
			}
		} else {
			init_val = LLVMConstNull(var_type);
		}
		LLVMValueRef store = LLVMBuildStore(c->builder, init_val, val_ptr);
		attach_tbaa(c, store, var_type);
	} else if (n->type == NODE_ASSIGN) {
		LLVMTypeRef ignored;

		// [FIX] Use get_address for ALL assignments to handle nested access
		LLVMValueRef target_ptr =
			get_address(c, n->data.assign.target, &ignored);
		ASTNode *target = n->data.assign.target;

		if (target_ptr) {
			if (n->data.assign.value->type == NODE_STRUCT_LITERAL) {
				n->data.assign.value->data_type =
					(target->type == NODE_VAR_REF)
						? scope_find(c, target->data.var_ref.name)
							  ->node->data_type
						: NULL;
			}
			LLVMValueRef val = codegen_expr(c, n->data.assign.value);
			LLVMBuildStore(c->builder, val, target_ptr);
		}
	} else if (n->type == NODE_IF) {
		LLVMValueRef cond_val = codegen_expr(c, n->data.if_stmt.cond);
		if (LLVMGetTypeKind(LLVMTypeOf(cond_val)) == LLVMIntegerTypeKind &&
			LLVMGetIntTypeWidth(LLVMTypeOf(cond_val)) != 1) {
			cond_val = LLVMBuildICmp(c->builder, LLVMIntNE, cond_val,
									 LLVMConstInt(LLVMTypeOf(cond_val), 0, 0),
									 "to_bool");
		}
		LLVMValueRef func = c->current_func;
		LLVMBasicBlockRef then_bb = LLVMAppendBasicBlock(func, "then");
		LLVMBasicBlockRef else_bb = n->data.if_stmt.else_block
										? LLVMAppendBasicBlock(func, "else")
										: NULL;
		LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlock(func, "if_cont");
		LLVMValueRef br_instr = LLVMBuildCondBr(c->builder, cond_val, then_bb,
												else_bb ? else_bb : merge_bb);
		set_branch_weights(c, br_instr, 1, 1);
		LLVMPositionBuilderAtEnd(c->builder, then_bb);
		codegen_stmt(c, n->data.if_stmt.then_block);
		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder)))
			LLVMBuildBr(c->builder, merge_bb);
		if (else_bb) {
			LLVMPositionBuilderAtEnd(c->builder, else_bb);
			codegen_stmt(c, n->data.if_stmt.else_block);
			if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder)))
				LLVMBuildBr(c->builder, merge_bb);
		}
		LLVMPositionBuilderAtEnd(c->builder, merge_bb);
	} else if (n->type == NODE_WHILE) {
		LLVMBasicBlockRef cond_bb =
			LLVMAppendBasicBlock(c->current_func, "while_cond");
		LLVMBasicBlockRef body_bb =
			LLVMAppendBasicBlock(c->current_func, "while_body");
		LLVMBasicBlockRef exit_bb =
			LLVMAppendBasicBlock(c->current_func, "while_exit");
		LLVMBuildBr(c->builder, cond_bb);
		LLVMPositionBuilderAtEnd(c->builder, cond_bb);
		LLVMValueRef cond_val = codegen_expr(c, n->data.while_stmt.cond);
		if (LLVMGetTypeKind(LLVMTypeOf(cond_val)) == LLVMIntegerTypeKind &&
			LLVMGetIntTypeWidth(LLVMTypeOf(cond_val)) > 1) {
			cond_val = LLVMBuildICmp(c->builder, LLVMIntNE, cond_val,
									 LLVMConstInt(LLVMTypeOf(cond_val), 0, 0),
									 "bool_cast");
		}
		LLVMValueRef br =
			LLVMBuildCondBr(c->builder, cond_val, body_bb, exit_bb);
		set_branch_weights(c, br, 64, 1);
		add_loop_metadata(c, br);
		LLVMPositionBuilderAtEnd(c->builder, body_bb);
		codegen_stmt(c, n->data.while_stmt.body);
		LLVMBuildBr(c->builder, cond_bb);
		LLVMPositionBuilderAtEnd(c->builder, exit_bb);
	} else if (n->type == NODE_BATCH) {
		Scope *s_coll =
			scope_find(c, n->data.batch.collection->data.var_ref.name);
		if (!s_coll) {
			timbr_err("Batch on unknown var\n");
			exit(1);
		}
		LLVMValueRef set_ptr = s_coll->val;
		LLVMValueRef len_ptr = LLVMBuildStructGEP2(c->builder, s_coll->type,
												   set_ptr, 1, "len_ptr");
		LLVMValueRef len = LLVMBuildLoad2(
			c->builder, LLVMInt64TypeInContext(c->context), len_ptr, "len");
		attach_tbaa(c, len, LLVMInt64TypeInContext(c->context));
		LLVMValueRef data_ptr_ptr = LLVMBuildStructGEP2(
			c->builder, s_coll->type, set_ptr, 0, "buf_ptr");
		LLVMValueRef data_ptr = LLVMBuildLoad2(
			c->builder, LLVMPointerType(LLVMInt32TypeInContext(c->context), 0),
			data_ptr_ptr, "buf");
		attach_tbaa(c, data_ptr,
					LLVMPointerType(LLVMInt32TypeInContext(c->context), 0));
		LLVMBasicBlockRef prev_bb = LLVMGetInsertBlock(c->builder);
		LLVMBasicBlockRef loop_bb =
			LLVMAppendBasicBlock(c->current_func, "batch_loop");
		LLVMBasicBlockRef body_bb =
			LLVMAppendBasicBlock(c->current_func, "batch_body");
		LLVMBasicBlockRef exit_bb =
			LLVMAppendBasicBlock(c->current_func, "batch_exit");
		LLVMValueRef zero =
			LLVMConstInt(LLVMInt64TypeInContext(c->context), 0, 0);
		LLVMBuildBr(c->builder, loop_bb);
		LLVMPositionBuilderAtEnd(c->builder, loop_bb);
		LLVMValueRef idx =
			LLVMBuildPhi(c->builder, LLVMInt64TypeInContext(c->context), "idx");
		LLVMValueRef cmp =
			LLVMBuildICmp(c->builder, LLVMIntSLT, idx, len, "loop_cond");
		LLVMValueRef br = LLVMBuildCondBr(c->builder, cmp, body_bb, exit_bb);
		add_loop_metadata(c, br);
		LLVMPositionBuilderAtEnd(c->builder, body_bb);
		LLVMValueRef item_ptr =
			LLVMBuildGEP2(c->builder, LLVMInt32TypeInContext(c->context),
						  data_ptr, &idx, 1, "item_ptr");
		LLVMValueRef item_val = LLVMBuildLoad2(
			c->builder, LLVMInt32TypeInContext(c->context), item_ptr, "item");
		attach_tbaa(c, item_val, LLVMInt32TypeInContext(c->context));
		LLVMValueRef n_ptr = create_entry_block_alloca(
			c, LLVMInt32TypeInContext(c->context), n->data.batch.iterator_var);
		LLVMBuildStore(c->builder, item_val, n_ptr);
		Scope *old_scope = c->scope_stack;
		scope_push(c, n->data.batch.iterator_var, n_ptr,
				   LLVMInt32TypeInContext(c->context), NULL);
		codegen_stmt(c, n->data.batch.body);
		c->scope_stack = old_scope;
		LLVMBasicBlockRef body_end_bb = LLVMGetInsertBlock(c->builder);
		LLVMValueRef next_idx = LLVMBuildNUWAdd(
			c->builder, idx,
			LLVMConstInt(LLVMInt64TypeInContext(c->context), 1, 0), "next_idx");
		LLVMBuildBr(c->builder, loop_bb);
		LLVMValueRef incoming_vals[] = {zero, next_idx};
		LLVMBasicBlockRef incoming_blocks[] = {prev_bb, body_end_bb};
		LLVMAddIncoming(idx, incoming_vals, incoming_blocks, 2);
		LLVMPositionBuilderAtEnd(c->builder, exit_bb);
	} else if (n->type == NODE_RETURN) {
		LLVMValueRef ret_val = codegen_expr(c, n->data.ret_stmt.expr);
		if (n->data.ret_stmt.expr->type == NODE_CALL) {
			LLVMValueRef last_inst =
				LLVMGetLastInstruction(LLVMGetInsertBlock(c->builder));
			if (last_inst && LLVMIsACallInst(last_inst)) {
				LLVMSetTailCall(last_inst, 1);
			}
		}
		if (c->in_coroutine) {
			if (c->current_promise_ptr) {
				LLVMValueRef store =
					LLVMBuildStore(c->builder, ret_val, c->current_promise_ptr);
				LLVMSetVolatile(store, 1);
			}
			LLVMBuildBr(c->builder, c->coro_cleanup_block);
		} else {
			if (LLVMGetTypeKind(c->current_ret_type) == LLVMVoidTypeKind)
				LLVMBuildRetVoid(c->builder);
			else
				LLVMBuildRet(c->builder, ret_val);
		}
	} else if (n->type == NODE_DEFER) {
		codegen_stmt(c, n->data.defer.stmt);
	} else if (n->type == NODE_DROP) {
		LLVMValueRef val = codegen_expr(c, n->data.drop.val);
		if (c->current_promise_ptr) {
			LLVMValueRef store =
				LLVMBuildStore(c->builder, val, c->current_promise_ptr);
			LLVMSetVolatile(store, 1);
		}
		LLVMValueRef save_token =
			LLVMBuildCall2(c->builder, c->coro_save_type, c->coro_save,
						   &c->current_coro_hdl, 1, "save");
		LLVMValueRef suspend = LLVMBuildCall2(
			c->builder, c->coro_suspend_type, c->coro_suspend,
			(LLVMValueRef[]){
				save_token,
				LLVMConstInt(LLVMInt1TypeInContext(c->context), 0, 0)},
			2, "yield");
		LLVMBasicBlockRef resume_bb =
			LLVMAppendBasicBlock(c->current_func, "resume");
		LLVMValueRef sw =
			LLVMBuildSwitch(c->builder, suspend, c->coro_suspend_block, 2);
		LLVMAddCase(sw, LLVMConstInt(LLVMInt8TypeInContext(c->context), 0, 0),
					resume_bb);
		LLVMAddCase(sw, LLVMConstInt(LLVMInt8TypeInContext(c->context), 1, 0),
					c->coro_cleanup_block);
		LLVMPositionBuilderAtEnd(c->builder, resume_bb);
	}
}

// ... (Rest of the file including kawa_init, codegen_func_decl,
// kawa_compile, etc. remains as provided in previous corrected versions)
// ...

void kawa_init(KawaCompiler *c, const char *module_name) {
	LLVMInitializeNativeTarget();
	LLVMInitializeNativeAsmPrinter();
	LLVMInitializeNativeAsmParser();

	c->context = LLVMContextCreate();
	c->module = LLVMModuleCreateWithNameInContext(module_name, c->context);
	c->builder = LLVMCreateBuilderInContext(c->context);
	c->scope_stack = NULL;
	c->coro_resume = NULL;
	c->lambda_counter = 0;
	c->in_coroutine = 0;

	init_metadata(c);

	LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
	LLVMTypeRef i64 = LLVMInt64TypeInContext(c->context);
	LLVMTypeRef void_t = LLVMVoidTypeInContext(c->context);

	// Malloc
	c->malloc_type = LLVMFunctionType(i8ptr, &i64, 1, 0);
	c->malloc_fn = LLVMAddFunction(c->module, "malloc", c->malloc_type);
	// [FIX] Attributes removed for stability

	// Realloc
	LLVMTypeRef realloc_args[] = {i8ptr, i64};
	c->realloc_type = LLVMFunctionType(i8ptr, realloc_args, 2, 0);
	c->realloc_fn = LLVMAddFunction(c->module, "realloc", c->realloc_type);

	// Free
	c->free_type = LLVMFunctionType(void_t, &i8ptr, 1, 0);
	c->free_fn = LLVMAddFunction(c->module, "free", c->free_type);
	// [FIX] Attributes removed for stability

	// Coroutines (Intrinsics)
	LLVMTypeRef token = LLVMTokenTypeInContext(c->context);
	LLVMTypeRef i32 = LLVMInt32TypeInContext(c->context);

	// llvm.coro.id
	LLVMTypeRef id_args[] = {i32, i8ptr, i8ptr, i8ptr};
	c->coro_id_type = LLVMFunctionType(token, id_args, 4, 0);
	c->coro_id = LLVMAddFunction(c->module, "llvm.coro.id", c->coro_id_type);

	// llvm.coro.begin
	LLVMTypeRef begin_args[] = {token, i8ptr};
	c->coro_begin_type = LLVMFunctionType(i8ptr, begin_args, 2, 0);
	c->coro_begin =
		LLVMAddFunction(c->module, "llvm.coro.begin", c->coro_begin_type);

	// llvm.coro.size
	c->coro_size_type = LLVMFunctionType(i64, NULL, 0, 0);
	c->coro_size =
		LLVMAddFunction(c->module, "llvm.coro.size.i64", c->coro_size_type);

	// llvm.coro.suspend
	LLVMTypeRef susp_args[] = {token, LLVMInt1TypeInContext(c->context)};
	c->coro_suspend_type =
		LLVMFunctionType(LLVMInt8TypeInContext(c->context), susp_args, 2, 0);
	c->coro_suspend =
		LLVMAddFunction(c->module, "llvm.coro.suspend", c->coro_suspend_type);

	// llvm.coro.save
	c->coro_save_type = LLVMFunctionType(token, &i8ptr, 1, 0);
	c->coro_save =
		LLVMAddFunction(c->module, "llvm.coro.save", c->coro_save_type);

	// llvm.coro.end
	LLVMTypeRef end_args[] = {i8ptr, LLVMInt1TypeInContext(c->context)};
	c->coro_end_type =
		LLVMFunctionType(LLVMInt1TypeInContext(c->context), end_args, 2, 0);
	c->coro_end = LLVMAddFunction(c->module, "llvm.coro.end", c->coro_end_type);

	// llvm.coro.free
	c->coro_free_type = LLVMFunctionType(i8ptr, begin_args, 2, 0);
	c->coro_free =
		LLVMAddFunction(c->module, "llvm.coro.free", c->coro_free_type);

	// llvm.coro.promise
	LLVMTypeRef prom_args[] = {i8ptr, i32, LLVMInt1TypeInContext(c->context)};
	c->coro_promise_type = LLVMFunctionType(i8ptr, prom_args, 3, 0);
	c->coro_promise =
		LLVMAddFunction(c->module, "llvm.coro.promise", c->coro_promise_type);

	// llvm.coro.alloc
	c->coro_alloc_type =
		LLVMFunctionType(LLVMInt1TypeInContext(c->context), &token, 1, 0);
	c->coro_alloc =
		LLVMAddFunction(c->module, "llvm.coro.alloc", c->coro_alloc_type);

	// llvm.coro.done
	LLVMTypeRef done_args[] = {i8ptr};
	c->coro_done_type =
		LLVMFunctionType(LLVMInt1TypeInContext(c->context), done_args, 1, 0);
	c->coro_done =
		LLVMAddFunction(c->module, "llvm.coro.done", c->coro_done_type);

	c->drip_promise_index = 8;
	c->brew_promise_index = 8;
}

void codegen_func_decl(KawaCompiler *c, ASTNode *cur,
					   const char *implicit_self_struct) {
	LLVMTypeRef ret_t = LLVMInt32TypeInContext(c->context);
	if (cur->data.func.is_drip)
		ret_t = LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);

	int explicit_arg_cnt = 0;
	ASTNode *a = cur->data.func.args;
	while (a) {
		explicit_arg_cnt++;
		a = a->next;
	}

	int total_arg_cnt = explicit_arg_cnt + (implicit_self_struct ? 1 : 0);

	// 2. Allocate param types
	LLVMTypeRef *param_types = malloc(sizeof(LLVMTypeRef) * total_arg_cnt);

	int type_idx = 0;
	if (implicit_self_struct) {
		param_types[type_idx++] =
			LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
	}

	// [CRITICAL FIX] Use actual types from AST, do not hardcode i32!
	ASTNode *arg_node = cur->data.func.args;
	while (arg_node) {
		param_types[type_idx++] = get_llvm_type(c, arg_node->data_type);
		arg_node = arg_node->next;
	}

	// 3. Create Function
	LLVMTypeRef func_t = LLVMFunctionType(ret_t, param_types, total_arg_cnt, 0);
	free(param_types);

	c->current_func = LLVMAddFunction(c->module, cur->data.func.name, func_t);

	if (cur->data.func.is_drip) {
		LLVMAddAttributeAtIndex(
			c->current_func, LLVMAttributeFunctionIndex,
			LLVMCreateEnumAttribute(
				c->context, LLVMGetEnumAttributeKindForName("noinline", 8), 0));
	}

	LLVMAddAttributeAtIndex(
		c->current_func, LLVMAttributeFunctionIndex,
		LLVMCreateEnumAttribute(
			c->context, LLVMGetEnumAttributeKindForName("nounwind", 8), 0));

	c->current_ret_type = ret_t;

	if (cur->data.func.is_drip) {
		unsigned kind_id =
			LLVMGetEnumAttributeKindForName("presplitcoroutine", 17);
		LLVMAddAttributeAtIndex(
			c->current_func, LLVMAttributeFunctionIndex,
			LLVMCreateEnumAttribute(c->context, kind_id, 0));
	}

	LLVMBasicBlockRef entry = LLVMAppendBasicBlock(c->current_func, "entry");
	LLVMPositionBuilderAtEnd(c->builder, entry);

	// 4. Handle Argument Storage
	int arg_idx = 0;

	if (implicit_self_struct) {
		LLVMValueRef self_val = LLVMGetParam(c->current_func, arg_idx++);
		// (Optional: handle self storage if needed)
	}

	a = cur->data.func.args;
	while (a) {
		LLVMValueRef p_val = LLVMGetParam(c->current_func, arg_idx++);

		// [FIX] Use the actual AST type (e.g., %User), not i32!
		LLVMTypeRef arg_type = get_llvm_type(c, a->data_type);

		LLVMValueRef p_alloc =
			create_entry_block_alloca(c, arg_type, a->data.var_decl.name);

		LLVMBuildStore(c->builder, p_val, p_alloc);

		// [FIX] Push correct type to scope
		scope_push(c, a->data.var_decl.name, p_alloc, arg_type, a);

		a = a->next;
	}

	if (cur->data.func.is_drip) {
		int was_in_coroutine = c->in_coroutine;
		c->in_coroutine = 1;
		LLVMValueRef promise_alloca = create_entry_block_alloca(
			c, LLVMInt32TypeInContext(c->context), "promise_storage");
		c->current_promise_ptr = promise_alloca;

		LLVMValueRef promise_void = LLVMBuildBitCast(
			c->builder, promise_alloca,
			LLVMPointerType(LLVMInt8TypeInContext(c->context), 0), "prom_void");
		LLVMValueRef null_ptr = LLVMConstNull(
			LLVMPointerType(LLVMInt8TypeInContext(c->context), 0));
		LLVMValueRef id = LLVMBuildCall2(
			c->builder, c->coro_id_type, c->coro_id,
			(LLVMValueRef[]){LLVMConstInt(LLVMInt32TypeInContext(c->context),
										  c->drip_promise_index, 0),
							 promise_void, null_ptr, null_ptr},
			4, "id");
		LLVMValueRef need_alloc =
			LLVMBuildCall2(c->builder, c->coro_alloc_type, c->coro_alloc, &id,
						   1, "need_alloc");
		LLVMValueRef size = LLVMBuildCall2(c->builder, c->coro_size_type,
										   c->coro_size, NULL, 0, "size");
		LLVMBasicBlockRef alloc_bb =
			LLVMAppendBasicBlock(c->current_func, "alloc");
		LLVMBasicBlockRef cont_bb =
			LLVMAppendBasicBlock(c->current_func, "alloc_cont");
		LLVMBuildCondBr(c->builder, need_alloc, alloc_bb, cont_bb);
		LLVMPositionBuilderAtEnd(c->builder, alloc_bb);
		LLVMValueRef malloc_ptr = LLVMBuildCall2(
			c->builder, c->malloc_type, c->malloc_fn, &size, 1, "coro_mem");
		LLVMBuildBr(c->builder, cont_bb);
		LLVMPositionBuilderAtEnd(c->builder, cont_bb);
		LLVMValueRef phi = LLVMBuildPhi(
			c->builder, LLVMPointerType(LLVMInt8TypeInContext(c->context), 0),
			"mem_phi");
		LLVMAddIncoming(phi, (LLVMValueRef[]){malloc_ptr, null_ptr},
						(LLVMBasicBlockRef[]){alloc_bb, entry}, 2);
		LLVMValueRef hdl =
			LLVMBuildCall2(c->builder, c->coro_begin_type, c->coro_begin,
						   (LLVMValueRef[]){id, phi}, 2, "hdl");
		LLVMValueRef old_hdl = c->current_coro_hdl;
		c->current_coro_hdl = hdl;

		LLVMValueRef suspend = LLVMBuildCall2(
			c->builder, c->coro_suspend_type, c->coro_suspend,
			(LLVMValueRef[]){
				LLVMConstNull(LLVMTokenTypeInContext(c->context)),
				LLVMConstInt(LLVMInt1TypeInContext(c->context), 0, 0)},
			2, "suspend");
		LLVMBasicBlockRef suspend_bb =
			LLVMAppendBasicBlock(c->current_func, "suspend");
		LLVMBasicBlockRef resume_bb =
			LLVMAppendBasicBlock(c->current_func, "resume");
		LLVMBasicBlockRef cleanup_bb =
			LLVMAppendBasicBlock(c->current_func, "cleanup");
		LLVMBasicBlockRef old_cleanup = c->coro_cleanup_block;
		LLVMBasicBlockRef old_suspend = c->coro_suspend_block;
		c->coro_cleanup_block = cleanup_bb;
		c->coro_suspend_block = suspend_bb;
		LLVMValueRef sw = LLVMBuildSwitch(c->builder, suspend, suspend_bb, 2);
		LLVMAddCase(sw, LLVMConstInt(LLVMInt8TypeInContext(c->context), 0, 0),
					resume_bb);
		LLVMAddCase(sw, LLVMConstInt(LLVMInt8TypeInContext(c->context), 1, 0),
					cleanup_bb);

		LLVMPositionBuilderAtEnd(c->builder, resume_bb);

		codegen_stmt(c, cur->data.func.body);

		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder))) {
			LLVMValueRef final_suspend = LLVMBuildCall2(
				c->builder, c->coro_suspend_type, c->coro_suspend,
				(LLVMValueRef[]){
					LLVMConstNull(LLVMTokenTypeInContext(c->context)),
					LLVMConstInt(LLVMInt1TypeInContext(c->context), 1, 0)},
				2, "final");

			LLVMBasicBlockRef final_cleanup_bb = c->coro_cleanup_block;
			LLVMValueRef final_sw =
				LLVMBuildSwitch(c->builder, final_suspend, final_cleanup_bb, 2);
			LLVMAddCase(final_sw,
						LLVMConstInt(LLVMInt8TypeInContext(c->context), 0, 0),
						final_cleanup_bb);
			LLVMAddCase(final_sw,
						LLVMConstInt(LLVMInt8TypeInContext(c->context), 1, 0),
						final_cleanup_bb);
		}
		LLVMPositionBuilderAtEnd(c->builder, cleanup_bb);
		LLVMBuildCall2(
			c->builder, c->coro_end_type, c->coro_end,
			(LLVMValueRef[]){
				null_ptr,
				LLVMConstInt(LLVMInt1TypeInContext(c->context), 0, 0)},
			2, "");
		LLVMBuildBr(c->builder, suspend_bb);
		LLVMPositionBuilderAtEnd(c->builder, suspend_bb);

		LLVMValueRef md_str = LLVMMDStringInContext(c->context, "drip", 4);
		LLVMValueRef md = LLVMMDNodeInContext(c->context, &md_str, 1);
		unsigned KIND =
			LLVMGetMDKindID("kawa.coro.kind", strlen("kawa.coro.kind"));
		LLVMSetMetadata(hdl, KIND, md);
		LLVMBuildRet(c->builder, hdl);

		c->in_coroutine = was_in_coroutine;
		c->coro_cleanup_block = old_cleanup;
		c->coro_suspend_block = old_suspend;
		c->current_coro_hdl = old_hdl;
		c->current_promise_ptr = NULL;
	} else {
		codegen_stmt(c, cur->data.func.body);
		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder))) {
			LLVMBuildRet(c->builder, LLVMConstInt(ret_t, 0, 0));
		}
	}
}

void kawa_compile(KawaCompiler *c, ASTNode *root) {
	ASTNode *cur = root->next;

	// Pass 1: Register Structs AND Aliases
	ASTNode *scanner = cur;
	while (scanner) {
		if (scanner->type == NODE_STRUCT_DECL) {
			LLVMTypeRef exists =
				LLVMGetTypeByName(c->module, scanner->data.struct_decl.name);
			if (!exists) {
				LLVMStructCreateNamed(c->context,
									  scanner->data.struct_decl.name);
			}
		}
		// [FIX] Register Alias
		if (scanner->type == NODE_ALIAS) {
			register_alias(scanner->data.alias.name, scanner->data_type);
		}

		scanner = scanner->next;
	}

	// Pass 2: Define Struct Bodies
	scanner = cur;
	while (scanner) {
		if (scanner->type == NODE_STRUCT_DECL) {
			LLVMTypeRef struct_t =
				LLVMGetTypeByName(c->module, scanner->data.struct_decl.name);

			// Count fields
			int field_count = 0;
			ASTNode *field = scanner->data.struct_decl.fields;
			while (field) {
				field_count++;
				field = field->next;
			}

			// Collect types
			LLVMTypeRef *elem_types = malloc(sizeof(LLVMTypeRef) * field_count);
			field = scanner->data.struct_decl.fields;
			int idx = 0;
			while (field) {
				elem_types[idx++] = get_llvm_type(c, field->data_type);
				field = field->next;
			}

			LLVMStructSetBody(struct_t, elem_types, field_count, 0);
			free(elem_types);

			// [FIX] Register Struct Def so get_field_index works!
			register_struct(scanner->data.struct_decl.name, struct_t);

			// Fill the fields info in the newly created StructDef (it's at
			// head)
			StructDef *sd = struct_defs;
			sd->field_count = field_count;

			field = scanner->data.struct_decl.fields;
			int f_idx = 0;
			while (field) {
				sd->fields[f_idx].name = strdup(field->data.var_decl.name);
				sd->fields[f_idx].type = get_llvm_type(c, field->data_type);
				f_idx++;
				field = field->next;
			}
		}
		scanner = scanner->next;
	}

	// Pass 3: Generate Functions
	while (cur) {
		if (cur->type == NODE_FUNC_DECL) {
			codegen_func_decl(c, cur, NULL);
		} else if (cur->type == NODE_IMPL_BLOCK) {
			ASTNode *method = cur->data.impl.methods;
			while (method) {
				codegen_func_decl(c, method, cur->data.impl.struct_name);
				method = method->next;
			}
		}
		// Structs already handled above
		cur = cur->next;
	}
}

void kawa_optimize_and_write(KawaCompiler *c, const char *filename) {
	// char *error = NULL;
	// if (LLVMVerifyModule(c->module, LLVMReturnStatusAction, &error)) {
	// 	timbr_err("LLVM Module Verification Failed:\n%s\n", error);
	// 	LLVMDisposeMessage(error);
	// 	// Dump the broken module so you can see where the error is
	// 	LLVMDumpModule(c->module);
	// 	exit(1);
	// }
	// LLVMDisposeMessage(error);

	// [OPTIMIZATION] Set up Target Machine for Host CPU
	char *error_msg = NULL;
	LLVMTargetRef target;
	LLVMGetTargetFromTriple(LLVMGetDefaultTargetTriple(), &target, &error_msg);
	if (!target) {
		timbr_err("Target selection failed: %s\n", error_msg);
		return;
	}

	LLVMTargetMachineRef machine = LLVMCreateTargetMachine(
		target, LLVMGetDefaultTargetTriple(), LLVMGetHostCPUName(),
		LLVMGetHostCPUFeatures(), LLVMCodeGenLevelAggressive, LLVMRelocDefault,
		LLVMCodeModelDefault);

	// Set Data Layout (Critical for Vectorization)
	LLVMSetModuleDataLayout(c->module, LLVMCreateTargetDataLayout(machine));
	LLVMSetTarget(c->module, LLVMGetDefaultTargetTriple());

	LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();

	// [OPTIMIZATION] Cleaned up pass pipeline
	// "default<O3>" covers most cases. We prepend coro specific passes.
	LLVMRunPasses(c->module,
				  "coro-early,coro-split,coro-elide,coro-cleanup,default<O3>",
				  machine, opts);

	LLVMDisposePassBuilderOptions(opts);

	if (LLVMWriteBitcodeToFile(c->module, filename) != 0) {
		timbr_err("Error writing bitcode\n");
	}
	// char *text = LLVMPrintModuleToString(c->module);
	// LLVMDumpModule(c->module);
	if (LLVMPrintModuleToFile(c->module, "output.ll", &error_msg)) {
		timbr_err("Writing file failed: %s\n", error_msg);
	};

	LLVMDisposeTargetMachine(machine);
}
