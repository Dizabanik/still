#include "codegen_internal.h"

// Normalize a condition value to i1. Integer conditions wider than one bit
// become `!= 0`; pointers become null checks.
static LLVMValueRef cond_to_bool(KawaCompiler *c, LLVMValueRef cond) {
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

void codegen_stmt(KawaCompiler *c, ASTNode *n) {
	if (!n)
		return;
	if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder)))
		return;

	switch (n->type) {

	case NODE_CALL:
	case NODE_SET_POUR:
		codegen_expr(c, n);
		return;

	case NODE_BLOCK: {
		for (ASTNode *s = n->data.block.stmts; s; s = s->next)
			codegen_stmt(c, s);
		return;
	}

	case NODE_VAR_DECL: {
		LLVMTypeRef var_type = get_llvm_type(c, n->data_type);
		LLVMValueRef val_ptr =
			create_entry_block_alloca(c, var_type, n->data.var_decl.name);
		scope_push(c, n->data.var_decl.name, val_ptr, var_type, n);

		LLVMValueRef init_val = NULL;
		if (n->data.var_decl.init) {
			// Propagate the declared type to a struct literal initializer
			// so it picks up the right field layout.
			if (n->data.var_decl.init->type == NODE_STRUCT_LITERAL &&
				!n->data.var_decl.init->data_type)
				n->data.var_decl.init->data_type = n->data_type;

			init_val = codegen_expr(c, n->data.var_decl.init);
			init_val =
				coerce_value(c, init_val, n->data.var_decl.init->data_type,
							 var_type, n->data_type);
		} else {
			init_val = LLVMConstNull(var_type);
		}
		LLVMValueRef store = LLVMBuildStore(c->builder, init_val, val_ptr);
		attach_tbaa(c, store, var_type);
		return;
	}

	case NODE_ASSIGN: {
		ASTNode *target = n->data.assign.target;
		LLVMTypeRef target_type = NULL;
		LLVMValueRef target_ptr = get_address(c, target, &target_type);
		if (!target_ptr || !target_type) {
			timbr_err("Internal error: cannot resolve assignment target\n");
			exit(1);
		}

		if (target_type &&
			LLVMGetTypeKind(target_type) == LLVMPointerTypeKind &&
			n->data.assign.value->type == NODE_STRUCT_LITERAL) {
			// Assigning a struct literal through a pointer: spill the
			// literal into a temp of the pointee type so field GEPs line up.
			ASTNode *decl_node = NULL;
			if (target->type == NODE_VAR_REF) {
				Scope *s = scope_find(c, target->data.var_ref.name);
				if (s)
					decl_node = s->node;
			}
			if (decl_node && decl_node->data_type &&
				decl_node->data_type->inner)
				n->data.assign.value->data_type = decl_node->data_type->inner;
		} else if (n->data.assign.value->type == NODE_STRUCT_LITERAL &&
				   !n->data.assign.value->data_type) {
			if (target->type == NODE_VAR_REF) {
				Scope *s = scope_find(c, target->data.var_ref.name);
				if (s && s->node && s->node->data_type)
					n->data.assign.value->data_type = s->node->data_type;
			}
		}

		LLVMValueRef val = codegen_expr(c, n->data.assign.value);
		val = coerce_value(c, val, n->data.assign.value->data_type, target_type,
						   target->data_type);
		LLVMValueRef store = LLVMBuildStore(c->builder, val, target_ptr);
		attach_tbaa(c, store, target_type);
		return;
	}

	case NODE_IF: {
		LLVMValueRef cond_val =
			cond_to_bool(c, codegen_expr(c, n->data.if_stmt.cond));
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
		return;
	}

	case NODE_WHILE: {
		LLVMBasicBlockRef cond_bb =
			LLVMAppendBasicBlock(c->current_func, "while_cond");
		LLVMBasicBlockRef body_bb =
			LLVMAppendBasicBlock(c->current_func, "while_body");
		LLVMBasicBlockRef exit_bb =
			LLVMAppendBasicBlock(c->current_func, "while_exit");
		LLVMBuildBr(c->builder, cond_bb);
		LLVMPositionBuilderAtEnd(c->builder, cond_bb);
		LLVMValueRef cond_val =
			cond_to_bool(c, codegen_expr(c, n->data.while_stmt.cond));
		LLVMValueRef br =
			LLVMBuildCondBr(c->builder, cond_val, body_bb, exit_bb);
		set_branch_weights(c, br, 64, 1); // loops iterate more often than not
		add_loop_metadata(c, br);
		LLVMPositionBuilderAtEnd(c->builder, body_bb);
		codegen_stmt(c, n->data.while_stmt.body);
		LLVMBuildBr(c->builder, cond_bb);
		LLVMPositionBuilderAtEnd(c->builder, exit_bb);
		return;
	}

	case NODE_BATCH: {
		Scope *s_coll =
			scope_find(c, n->data.batch.collection->data.var_ref.name);
		if (!s_coll) {
			timbr_err("Batch on unknown var\n");
			exit(1);
		}
		LLVMContextRef ctx = c->context;
		LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx);
		LLVMTypeRef i32_t = LLVMInt32TypeInContext(ctx);
		LLVMTypeRef i32_ptr_t = LLVMPointerType(i32_t, 0);

		LLVMValueRef set_ptr = s_coll->val;
		LLVMValueRef len_ptr = LLVMBuildStructGEP2(c->builder, s_coll->type,
												   set_ptr, 1, "len_ptr");
		LLVMValueRef len = LLVMBuildLoad2(c->builder, i64_t, len_ptr, "len");
		attach_tbaa(c, len, i64_t);
		LLVMValueRef data_ptr_ptr = LLVMBuildStructGEP2(
			c->builder, s_coll->type, set_ptr, 0, "buf_ptr");
		LLVMValueRef data_ptr =
			LLVMBuildLoad2(c->builder, i32_ptr_t, data_ptr_ptr, "buf");
		attach_tbaa(c, data_ptr, i32_ptr_t);

		LLVMBasicBlockRef prev_bb = LLVMGetInsertBlock(c->builder);
		LLVMBasicBlockRef loop_bb =
			LLVMAppendBasicBlock(c->current_func, "batch_loop");
		LLVMBasicBlockRef body_bb =
			LLVMAppendBasicBlock(c->current_func, "batch_body");
		LLVMBasicBlockRef exit_bb =
			LLVMAppendBasicBlock(c->current_func, "batch_exit");

		LLVMValueRef zero = LLVMConstInt(i64_t, 0, 0);
		LLVMBuildBr(c->builder, loop_bb);
		LLVMPositionBuilderAtEnd(c->builder, loop_bb);
		LLVMValueRef idx = LLVMBuildPhi(c->builder, i64_t, "idx");
		LLVMValueRef cmp =
			LLVMBuildICmp(c->builder, LLVMIntULT, idx, len, "loop_cond");
		LLVMValueRef br = LLVMBuildCondBr(c->builder, cmp, body_bb, exit_bb);
		add_loop_metadata(c, br);
		LLVMPositionBuilderAtEnd(c->builder, body_bb);

		LLVMValueRef item_ptr =
			LLVMBuildGEP2(c->builder, i32_t, data_ptr, &idx, 1, "item_ptr");
		LLVMValueRef item_val =
			LLVMBuildLoad2(c->builder, i32_t, item_ptr, "item");
		attach_tbaa(c, item_val, i32_t);
		LLVMValueRef n_ptr =
			create_entry_block_alloca(c, i32_t, n->data.batch.iterator_var);
		LLVMBuildStore(c->builder, item_val, n_ptr);
		Scope *old_scope = c->scope_stack;
		scope_push(c, n->data.batch.iterator_var, n_ptr, i32_t, NULL);
		codegen_stmt(c, n->data.batch.body);
		c->scope_stack = old_scope;

		LLVMBasicBlockRef body_end_bb = LLVMGetInsertBlock(c->builder);
		LLVMValueRef next_idx = LLVMBuildNUWAdd(
			c->builder, idx, LLVMConstInt(i64_t, 1, 0), "next_idx");
		LLVMBuildBr(c->builder, loop_bb);
		LLVMAddIncoming(idx, (LLVMValueRef[]){zero, next_idx},
						(LLVMBasicBlockRef[]){prev_bb, body_end_bb}, 2);
		LLVMPositionBuilderAtEnd(c->builder, exit_bb);
		return;
	}

	case NODE_RETURN: {
		LLVMValueRef ret_val = codegen_expr(c, n->data.ret_stmt.expr);
		if (c->in_coroutine) {
			if (c->current_promise_ptr) {
				ret_val =
					coerce_value(c, ret_val, n->data.ret_stmt.expr->data_type,
								 LLVMInt32TypeInContext(c->context), NULL);
				LLVMValueRef store =
					LLVMBuildStore(c->builder, ret_val, c->current_promise_ptr);
				LLVMSetVolatile(store, 1);
			}
			LLVMBuildBr(c->builder, c->coro_cleanup_block);
		} else {
			if (LLVMGetTypeKind(c->current_ret_type) == LLVMVoidTypeKind)
				LLVMBuildRetVoid(c->builder);
			else {
				ret_val =
					coerce_value(c, ret_val, n->data.ret_stmt.expr->data_type,
								 c->current_ret_type, NULL);
				LLVMBuildRet(c->builder, ret_val);
			}
		}
		return;
	}

	case NODE_DEFER:
		codegen_stmt(c, n->data.defer.stmt);
		return;

	case NODE_DROP: {
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
		return;
	}

	default:
		break;
	}

	timbr_err("Internal error: unknown AST node type in codegen_stmt (%d)\n",
			  n->type);
	exit(1);
}
