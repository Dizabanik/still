#include "codegen_internal.h"

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
			LLVMTypeKind init_k = LLVMGetTypeKind(init_type);
			LLVMTypeKind var_k = LLVMGetTypeKind(var_type);

			if (init_k == LLVMIntegerTypeKind && var_k == LLVMIntegerTypeKind) {
				unsigned iw = LLVMGetIntTypeWidth(init_type);
				unsigned vw = LLVMGetIntTypeWidth(var_type);
				if (iw > vw)
					init_val =
						LLVMBuildTrunc(c->builder, init_val, var_type, "trunc");
				if (iw < vw)
					init_val =
						LLVMBuildZExt(c->builder, init_val, var_type, "zext");
			} else if (init_k == LLVMIntegerTypeKind &&
					   (var_k == LLVMFloatTypeKind ||
						var_k == LLVMDoubleTypeKind)) {
				init_val =
					LLVMBuildSIToFP(c->builder, init_val, var_type, "itofp");
			} else if ((init_k == LLVMFloatTypeKind ||
						init_k == LLVMDoubleTypeKind) &&
					   var_k == LLVMIntegerTypeKind) {
				init_val =
					LLVMBuildFPToSI(c->builder, init_val, var_type, "fptosi");
			} else if ((init_k == LLVMFloatTypeKind ||
						init_k == LLVMDoubleTypeKind) &&
					   (var_k == LLVMFloatTypeKind ||
						var_k == LLVMDoubleTypeKind)) {
				if (init_k != var_k) {
					init_val =
						LLVMBuildFPExt(c->builder, init_val, var_type, "fpext");
				}
			}
		} else {
			init_val = LLVMConstNull(var_type);
		}
		LLVMValueRef store = LLVMBuildStore(c->builder, init_val, val_ptr);
		attach_tbaa(c, store, var_type);
	} else if (n->type == NODE_ASSIGN) {
		LLVMTypeRef target_type = NULL;

		// [FIX] Use get_address for ALL assignments to handle nested access
		LLVMValueRef target_ptr =
			get_address(c, n->data.assign.target, &target_type);
		ASTNode *target = n->data.assign.target;

		if (target_ptr && target_type) {
			if (n->data.assign.value->type == NODE_STRUCT_LITERAL) {
				n->data.assign.value->data_type =
					(target->type == NODE_VAR_REF)
						? scope_find(c, target->data.var_ref.name)
							  ->node->data_type
						: NULL;
			}
			LLVMValueRef val = codegen_expr(c, n->data.assign.value);

			// [FIX] Auto-Cast for Assignment
			LLVMTypeRef val_type = LLVMTypeOf(val);
			LLVMTypeKind val_k = LLVMGetTypeKind(val_type);
			LLVMTypeKind tgt_k = LLVMGetTypeKind(target_type);

			if (val_k == LLVMIntegerTypeKind && tgt_k == LLVMIntegerTypeKind) {
				unsigned vw = LLVMGetIntTypeWidth(val_type);
				unsigned tw = LLVMGetIntTypeWidth(target_type);
				if (vw > tw)
					val = LLVMBuildTrunc(c->builder, val, target_type, "trunc");
				if (vw < tw)
					val = LLVMBuildZExt(c->builder, val, target_type, "zext");
			} else if (val_k == LLVMIntegerTypeKind &&
					   (tgt_k == LLVMFloatTypeKind ||
						tgt_k == LLVMDoubleTypeKind)) {
				val = LLVMBuildSIToFP(c->builder, val, target_type, "itofp");
			} else if ((val_k == LLVMFloatTypeKind ||
						val_k == LLVMDoubleTypeKind) &&
					   tgt_k == LLVMIntegerTypeKind) {
				val = LLVMBuildFPToSI(c->builder, val, target_type, "fptosi");
			} else if ((val_k == LLVMFloatTypeKind ||
						val_k == LLVMDoubleTypeKind) &&
					   (tgt_k == LLVMFloatTypeKind ||
						tgt_k == LLVMDoubleTypeKind)) {
				if (val_k != tgt_k) {
					val = LLVMBuildFPExt(c->builder, val, target_type, "fpext");
				}
			}

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
