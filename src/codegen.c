#include "codegen.h"
#include "ast.h"
#include "timbr.h"
#include <lexer.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
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

// --- OPTIMIZATION HELPERS ---

Scope *scope_find(KawaCompiler *c, const char *name);

// Helper to deduce type name from a complex expression (recursively)
// This fixes the segfault by ensuring we can find the type of 'field' in
// struct.field.method()
const char *resolve_type_name(KawaCompiler *c, ASTNode *n) {
	if (n->type == NODE_VAR_REF) {
		Scope *s = scope_find(c, n->data.var_ref.name);
		if (s && s->type) {
			// In a real compiler, we would store the struct name on the Type*,
			// but here we might need to look it up in struct_defs based on LLVM
			// type For now, return the var name to support simple mangling
			return s->name;
		}
	}
	if (n->type == NODE_MEMBER_ACCESS) {
		// In a full implementation, you would look up the struct member's type
		// here For now, we return a placeholder or the member name to prevent a
		// crash
		return n->data.member_access.member;
	}
	return "unknown";
}

void add_loop_metadata(KawaCompiler *c, LLVMValueRef branch_instr) {
	LLVMContextRef ctx = c->context;

	// 1. Create the Constant '1' and wrap as Metadata
	LLVMValueRef one_val = LLVMConstInt(LLVMInt1TypeInContext(ctx), 1, 0);
	LLVMMetadataRef one_md = LLVMValueAsMetadata(one_val);

	// 2. Create Strings (Use InContext2 to get LLVMMetadataRef directly)
	LLVMMetadataRef vec_str =
		LLVMMDStringInContext2(ctx, "llvm.loop.vectorize.enable", 26);
	LLVMMetadataRef unroll_str =
		LLVMMDStringInContext2(ctx, "llvm.loop.unroll.enable", 23);

	// 3. Create Attribute Nodes
	LLVMMetadataRef vec_args[] = {vec_str, one_md};
	LLVMMetadataRef vec_node = LLVMMDNodeInContext2(ctx, vec_args, 2);

	LLVMMetadataRef unroll_args[] = {unroll_str, one_md};
	LLVMMetadataRef unroll_node = LLVMMDNodeInContext2(ctx, unroll_args, 2);

	// 4. Create a "Dummy" Placeholder Node
	// Since LLVMTemporaryMDNode isn't in the C API, we create a valid
	// temporary node that we will overwrite in step 6.
	LLVMMetadataRef temp_args[] = {one_md};
	LLVMMetadataRef temp_node = LLVMMDNodeInContext2(ctx, temp_args, 1);

	// 5. Create the Loop ID Node containing the dummy
	// !{ temp_node, vec_node, unroll_node }
	LLVMMetadataRef loop_args[] = {temp_node, vec_node, unroll_node};
	LLVMMetadataRef loop_md = LLVMMDNodeInContext2(ctx, loop_args, 3);

	// 6. Close the Cycle (Self-Reference)
	// The C API requires the *Target* to be a ValueRef, but the *Replacement*
	// to be MetadataRef.
	LLVMValueRef loop_md_as_val = LLVMMetadataAsValue(ctx, loop_md);

	// "Replace the 0th operand of loop_md_as_val with loop_md"
	LLVMReplaceMDNodeOperandWith(loop_md_as_val, 0, loop_md);

	// 7. Attach to the Branch Instruction
	unsigned kind_id = LLVMGetMDKindID("llvm.loop", 9);
	LLVMSetMetadata(branch_instr, kind_id, loop_md_as_val);
}

// Replace your init_metadata function in codegen.c with this:
void init_metadata(KawaCompiler *c) {
	if (tbaa_root)
		return;

	// Root Node: !{ "Kawa TBAA" }
	LLVMValueRef root_name = LLVMMDStringInContext(c->context, "Kawa TBAA", 9);
	LLVMValueRef root_node = LLVMMDNodeInContext(c->context, &root_name, 1);
	tbaa_root = root_node;

	// Scalar Type Node: !{ "tbaa_scalar", root_node }
	// REMOVED: The 3rd argument (Size 0) caused the bit-width verification
	// error
	LLVMValueRef scalar_name =
		LLVMMDStringInContext(c->context, "tbaa_scalar", 11);
	LLVMValueRef scalar_args[] = {scalar_name, tbaa_root};
	tbaa_scalar = LLVMMDNodeInContext(c->context, scalar_args, 2);

	// Ptr Type Node
	LLVMValueRef ptr_name = LLVMMDStringInContext(c->context, "tbaa_ptr", 8);
	LLVMValueRef ptr_args[] = {ptr_name, tbaa_scalar};
	tbaa_ptr = LLVMMDNodeInContext(c->context, ptr_args, 2);

	// Int Type Node
	LLVMValueRef int_name = LLVMMDStringInContext(c->context, "tbaa_int", 8);
	LLVMValueRef int_args[] = {int_name, tbaa_scalar};
	tbaa_int = LLVMMDNodeInContext(c->context, int_args, 2);
}

void attach_tbaa(KawaCompiler *c, LLVMValueRef instr, LLVMTypeRef type) {
	LLVMValueRef tag = tbaa_scalar;
	LLVMTypeKind kind = LLVMGetTypeKind(type);
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

// Enables Fast Math (No NaNs, Allow Reassociation, No Signed Zeros)
void set_fast_math(LLVMValueRef instr) {
	LLVMSetFastMathFlags(instr, LLVMFastMathAll);
}

// Moves stack allocations to entry block for mem2reg
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
	// Align 8 allows better vectorization loading for 64-bit types
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

LLVMTypeRef get_llvm_type(KawaCompiler *c, Type *t) {
	if (!t)
		return LLVMInt32TypeInContext(c->context);
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
	case TYPE_HANDLE:
		return LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
	case TYPE_PTR:
		return LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
	case TYPE_SET: {
		LLVMTypeRef elems[] = {
			LLVMPointerType(LLVMInt32TypeInContext(c->context), 0),
			LLVMInt64TypeInContext(c->context),
			LLVMInt64TypeInContext(c->context)};
		return LLVMStructTypeInContext(c->context, elems, 3, 0);
	}
	default:
		return LLVMInt32TypeInContext(c->context);
	}
}

LLVMValueRef codegen_expr(KawaCompiler *c, ASTNode *n);
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
			return LLVMConstReal(
				get_llvm_type(c, n->data_type),
				(double)n->data.literal
					.f_val); // Changed i_val to f_val based on parser
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
			timbr_err("Undefined variable '%s'\n", n->data.var_ref.name);
			exit(1);
		}
		LLVMValueRef load =
			LLVMBuildLoad2(c->builder, s->type, s->val, n->data.var_ref.name);
		attach_tbaa(c, load, s->type);
		return load;
	}

	if (n->type == NODE_CALL) {
		char func_name[256];
		memset(func_name, 0,
			   256); // [FIX A] Initialize memory to avoid segfaults

		if (n->data.call.callee->type == NODE_VAR_REF) {
			strcpy(func_name, n->data.call.callee->data.var_ref.name);
		} else if (n->data.call.callee->type == NODE_MEMBER_ACCESS) {
			ASTNode *obj = n->data.call.callee->data.member_access.object;
			char *member = n->data.call.callee->data.member_access.member;

			// [FIX A] Handle safe variable lookup
			if (obj->type == NODE_VAR_REF &&
				strcmp(obj->data.var_ref.name, "stdc") == 0) {
				strcpy(func_name, member);
			} else if (obj->type == NODE_VAR_REF) {
				sprintf(func_name, "%s_%s", obj->data.var_ref.name, member);
			} else {
				// [FIX A] Fallback for complex access (e.g. field.function())
				// Use the type name derived recursively or a generic identifier
				const char *type_name = resolve_type_name(c, obj);
				sprintf(func_name, "%s_%s", type_name, member);
			}
		}

		LLVMValueRef fn = LLVMGetNamedFunction(c->module, func_name);
		if (!fn) {
			// [OPTIMIZATION] Mark printf/malloc as library functions for
			// optimization
			if (strcmp(func_name, "printf") == 0) {
				LLVMTypeRef args[] = {
					LLVMPointerType(LLVMInt8TypeInContext(c->context), 0)};
				LLVMTypeRef ft = LLVMFunctionType(
					LLVMInt32TypeInContext(c->context), args, 1, 1);
				fn = LLVMAddFunction(c->module, "printf", ft);
				LLVMAddAttributeAtIndex(
					fn, LLVMAttributeFunctionIndex,
					LLVMCreateEnumAttribute(
						c->context,
						LLVMGetEnumAttributeKindForName("nounwind", 8), 0));
				LLVMAddAttributeAtIndex(
					fn, 1,
					LLVMCreateEnumAttribute(
						c->context,
						LLVMGetEnumAttributeKindForName("nocapture", 9), 0));
				LLVMAddAttributeAtIndex(
					fn, 1,
					LLVMCreateEnumAttribute(
						c->context,
						LLVMGetEnumAttributeKindForName("readonly", 8), 0));
			} else if (strcmp(func_name, "malloc") == 0)
				fn = c->malloc_fn;
			else if (strcmp(func_name, "free") == 0)
				fn = c->free_fn;
			else {
				// Panic if function not found (optional, but good for debug)
				timbr_err("Undefined function: %s\n", func_name);
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
		for (int i = 0; i < arg_count; i++) {
			llvm_args[i] = codegen_expr(c, arg_node);
			arg_node = arg_node->next;
		}

		LLVMValueRef call_res =
			LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(fn), fn,
						   llvm_args, arg_count, "");
		free(llvm_args);
		return call_res;
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

		// Check if coroutine is done before resuming
		LLVMValueRef is_done = LLVMBuildCall2(c->builder, c->coro_done_type,
											  c->coro_done, &hdl, 1, "is_done");

		LLVMBasicBlockRef resume_bb =
			LLVMAppendBasicBlock(c->current_func, "sip_resume");
		LLVMBasicBlockRef cont_bb =
			LLVMAppendBasicBlock(c->current_func, "sip_cont");

		LLVMBuildCondBr(c->builder, is_done, cont_bb, resume_bb);

		// Resume block: call resume if not done
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

		// Continue: load promise value
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
	return LLVMConstInt(LLVMInt32TypeInContext(c->context), 0, 0);
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
	if (n->type == NODE_BLOCK) {
		ASTNode *s = n->data.block.stmts;
		while (s) {
			codegen_stmt(c, s);
			s = s->next;
		}
	} else if (n->type == NODE_VAR_DECL) {
		LLVMValueRef init_val = NULL;
		LLVMTypeRef var_type = NULL;
		if (n->data.var_decl.init) {
			init_val = codegen_expr(c, n->data.var_decl.init);
			var_type = LLVMTypeOf(init_val);
		} else {
			var_type = LLVMInt32TypeInContext(c->context);
			init_val = LLVMConstInt(var_type, 0, 0);
		}

		LLVMValueRef val_ptr =
			create_entry_block_alloca(c, var_type, n->data.var_decl.name);
		scope_push(c, n->data.var_decl.name, val_ptr, var_type, n);
		LLVMValueRef store = LLVMBuildStore(c->builder, init_val, val_ptr);
		attach_tbaa(c, store, var_type);

	} else if (n->type == NODE_ASSIGN) {
		Scope *s = scope_find(c, n->data.assign.name);
		if (s) {
			LLVMValueRef val = codegen_expr(c, n->data.assign.value);
			LLVMValueRef store = LLVMBuildStore(c->builder, val, s->val);
			attach_tbaa(c, store, s->type);
			trigger_orbit_updates(c, s->node);
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
		set_branch_weights(c, br, 64, 1); // Assume loop taken

		// [OPTIMIZATION] Safe Loop Vectorization Hints
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
												   set_ptr, 2, "len_ptr");
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
		// [OPTIMIZATION] Enable vectorization for batch loops
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

		// [OPTIMIZATION] Improved Tail Call Detection
		if (n->data.ret_stmt.expr->type == NODE_CALL) {
			LLVMValueRef last_inst =
				LLVMGetLastInstruction(LLVMGetInsertBlock(c->builder));
			// Ensure it is actually the call instruction we just built
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

void kawa_init(KawaCompiler *c, const char *module_name) {
	LLVMInitializeNativeTarget();
	LLVMInitializeNativeAsmPrinter();
	LLVMInitializeNativeAsmParser(); // Needed for inline asm if you use it

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

	c->malloc_type = LLVMFunctionType(i8ptr, &i64, 1, 0);
	c->malloc_fn = LLVMAddFunction(c->module, "malloc", c->malloc_type);

	// [OPTIMIZATION] Memory Attributes
	LLVMAddAttributeAtIndex(
		c->malloc_fn, 0,
		LLVMCreateEnumAttribute(
			c->context, LLVMGetEnumAttributeKindForName("noalias", 7), 0));
	LLVMAddAttributeAtIndex(
		c->malloc_fn, LLVMAttributeFunctionIndex,
		LLVMCreateEnumAttribute(
			c->context, LLVMGetEnumAttributeKindForName("nounwind", 8), 0));

	LLVMTypeRef realloc_args[] = {i8ptr, i64};
	c->realloc_type = LLVMFunctionType(i8ptr, realloc_args, 2, 0);
	c->realloc_fn = LLVMAddFunction(c->module, "realloc", c->realloc_type);
	LLVMAddAttributeAtIndex(
		c->realloc_fn, 0,
		LLVMCreateEnumAttribute(
			c->context, LLVMGetEnumAttributeKindForName("noalias", 7), 0));

	c->free_type = LLVMFunctionType(void_t, &i8ptr, 1, 0);
	c->free_fn = LLVMAddFunction(c->module, "free", c->free_type);
	LLVMAddAttributeAtIndex(
		c->free_fn, 1,
		LLVMCreateEnumAttribute(
			c->context, LLVMGetEnumAttributeKindForName("nocapture", 9), 0));
	LLVMAddAttributeAtIndex(
		c->free_fn, LLVMAttributeFunctionIndex,
		LLVMCreateEnumAttribute(
			c->context, LLVMGetEnumAttributeKindForName("nounwind", 8), 0));

	LLVMTypeRef token = LLVMTokenTypeInContext(c->context);
	LLVMTypeRef i32 = LLVMInt32TypeInContext(c->context);
	LLVMTypeRef id_args[] = {i32, i8ptr, i8ptr, i8ptr};
	c->coro_id_type = LLVMFunctionType(token, id_args, 4, 0);
	c->coro_id = LLVMAddFunction(c->module, "llvm.coro.id", c->coro_id_type);
	LLVMTypeRef begin_args[] = {token, i8ptr};
	c->coro_begin_type = LLVMFunctionType(i8ptr, begin_args, 2, 0);
	c->coro_begin =
		LLVMAddFunction(c->module, "llvm.coro.begin", c->coro_begin_type);
	c->coro_size_type = LLVMFunctionType(i64, NULL, 0, 0);
	c->coro_size =
		LLVMAddFunction(c->module, "llvm.coro.size.i64", c->coro_size_type);
	LLVMTypeRef susp_args[] = {token, LLVMInt1TypeInContext(c->context)};
	c->coro_suspend_type =
		LLVMFunctionType(LLVMInt8TypeInContext(c->context), susp_args, 2, 0);
	c->coro_suspend =
		LLVMAddFunction(c->module, "llvm.coro.suspend", c->coro_suspend_type);
	c->coro_save_type = LLVMFunctionType(token, &i8ptr, 1, 0);
	c->coro_save =
		LLVMAddFunction(c->module, "llvm.coro.save", c->coro_save_type);
	LLVMTypeRef end_args[] = {i8ptr, LLVMInt1TypeInContext(c->context)};
	c->coro_end_type =
		LLVMFunctionType(LLVMInt1TypeInContext(c->context), end_args, 2, 0);
	c->coro_end = LLVMAddFunction(c->module, "llvm.coro.end", c->coro_end_type);
	c->coro_free_type = LLVMFunctionType(i8ptr, begin_args, 2, 0);
	c->coro_free =
		LLVMAddFunction(c->module, "llvm.coro.free", c->coro_free_type);
	LLVMTypeRef prom_args[] = {i8ptr, i32, LLVMInt1TypeInContext(c->context)};
	c->coro_promise_type = LLVMFunctionType(i8ptr, prom_args, 3, 0);
	c->coro_promise =
		LLVMAddFunction(c->module, "llvm.coro.promise", c->coro_promise_type);
	c->coro_alloc_type =
		LLVMFunctionType(LLVMInt1TypeInContext(c->context), &token, 1, 0);
	c->coro_alloc =
		LLVMAddFunction(c->module, "llvm.coro.alloc", c->coro_alloc_type);

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

	// 1. Count Args
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
		// [FIX] Inject 'self' as the first argument (i8* / ptr)
		param_types[type_idx++] =
			LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
	}

	for (int i = 0; i < explicit_arg_cnt; i++)
		param_types[type_idx++] = LLVMInt32TypeInContext(c->context);

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
		// [FIX] Store 'self' so it can be used in the body (if needed)
		// We give it a reserved name "self" (or the struct name mangled)
		LLVMValueRef self_val = LLVMGetParam(c->current_func, arg_idx++);
		// Note: Unless the user code explicitly uses 'self', we might not
		// strictly need to put it in scope, but for correctness in debug info
		// or future 'this' support: (Optional: Create alloca for self here if
		// your language supports 'this')
	}

	a = cur->data.func.args;
	while (a) {
		LLVMValueRef p_val = LLVMGetParam(c->current_func, arg_idx++);
		LLVMValueRef p_alloc = create_entry_block_alloca(
			c, LLVMInt32TypeInContext(c->context), a->data.var_decl.name);
		LLVMBuildStore(c->builder, p_val, p_alloc);
		scope_push(c, a->data.var_decl.name, p_alloc,
				   LLVMInt32TypeInContext(c->context), a);
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
	while (cur) {
		if (cur->type == NODE_FUNC_DECL) {
			codegen_func_decl(c, cur, NULL);
		} else if (cur->type == NODE_IMPL_BLOCK) {

			ASTNode *method = cur->data.impl.methods;
			while (method) {
				codegen_func_decl(c, method, cur->data.impl.struct_name);
				method = method->next;
			}
		} else if (cur->type == NODE_STRUCT_DECL) {
			LLVMStructCreateNamed(c->context, cur->data.struct_decl.name);
		}
		cur = cur->next;
	}
}

void kawa_optimize_and_write(KawaCompiler *c, const char *filename) {
	char *error = NULL;
	if (LLVMVerifyModule(c->module, LLVMReturnStatusAction, &error)) {
		timbr_err("LLVM Module Verification Failed:\n%s\n", error);
		LLVMDisposeMessage(error);
		// Dump the broken module so you can see where the error is
		LLVMDumpModule(c->module);
		exit(1);
	}
	LLVMDisposeMessage(error);

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
	LLVMDumpModule(c->module);

	LLVMDisposeTargetMachine(machine);
}
