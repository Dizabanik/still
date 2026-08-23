#include "codegen_internal.h"

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

		struct LoopTargets targets = {exit_bb, cond_bb, c->loop_stack};
		c->loop_stack = &targets;
		codegen_stmt(c, n->data.while_stmt.body);
		c->loop_stack = targets.next;

		LLVMBuildBr(c->builder, cond_bb);
		LLVMPositionBuilderAtEnd(c->builder, exit_bb);
		return;
	}

	case NODE_FOR: {
		// for init; cond; step { body }
		// Lowered as its own block structure so `continue` lands on the
		// step (not the condition) and per-iteration allocas stay scoped.
		if (n->data.for_stmt.init)
			codegen_stmt(c, n->data.for_stmt.init);

		LLVMBasicBlockRef cond_bb =
			LLVMAppendBasicBlock(c->current_func, "for_cond");
		LLVMBasicBlockRef body_bb =
			LLVMAppendBasicBlock(c->current_func, "for_body");
		LLVMBasicBlockRef step_bb =
			LLVMAppendBasicBlock(c->current_func, "for_step");
		LLVMBasicBlockRef exit_bb =
			LLVMAppendBasicBlock(c->current_func, "for_exit");

		LLVMBuildBr(c->builder, cond_bb);
		LLVMPositionBuilderAtEnd(c->builder, cond_bb);
		if (n->data.for_stmt.cond) {
			LLVMValueRef cond_val =
				cond_to_bool(c, codegen_expr(c, n->data.for_stmt.cond));
			LLVMValueRef br =
				LLVMBuildCondBr(c->builder, cond_val, body_bb, exit_bb);
			set_branch_weights(c, br, 64, 1);
			add_loop_metadata(c, br);
		} else {
			LLVMBuildBr(c->builder, body_bb); // `for (;;)` = infinite
		}

		LLVMPositionBuilderAtEnd(c->builder, body_bb);
		struct LoopTargets targets = {exit_bb, step_bb, c->loop_stack};
		c->loop_stack = &targets;
		codegen_stmt(c, n->data.for_stmt.body);
		c->loop_stack = targets.next;

		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder)))
			LLVMBuildBr(c->builder, step_bb);
		LLVMPositionBuilderAtEnd(c->builder, step_bb);
		if (n->data.for_stmt.step)
			codegen_stmt(c, n->data.for_stmt.step);
		LLVMBuildBr(c->builder, cond_bb);
		LLVMPositionBuilderAtEnd(c->builder, exit_bb);
		return;
	}

	case NODE_BREAK:
	case NODE_CONTINUE: {
		if (!c->loop_stack) {
			timbr_err("%s outside of a loop\n",
					  n->type == NODE_BREAK ? "break" : "continue");
			exit(1);
		}
		LLVMBuildBr(c->builder, n->type == NODE_BREAK
									? c->loop_stack->break_bb
									: c->loop_stack->continue_bb);
		return;
	}

	case NODE_BATCH: {
		ASTNode *coll = n->data.batch.collection;
		LLVMContextRef ctx = c->context;
		LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx);

		// Collection shape: set ({ ptr, len, cap }) or fixed-size array.
		LLVMValueRef data_ptr, len;
		LLVMTypeRef elem_t;

		if (coll->type != NODE_VAR_REF) {
			timbr_err("Batch requires a variable collection\n");
			exit(1);
		}
		Scope *s_coll = scope_find(c, coll->data.var_ref.name);
		if (!s_coll) {
			timbr_err("Batch on unknown var\n");
			exit(1);
		}

		int is_array = (s_coll->node && s_coll->node->data_type &&
						s_coll->node->data_type->kind == TYPE_ARRAY);
		if (is_array) {
			// Zero-copy: GEP straight into the array alloca; the trip
			// count is a constant so the backend can fully unroll small
			// arrays and vectorize large ones.
			LLVMTypeRef arr_t = s_coll->type;
			unsigned alen = LLVMGetArrayLength(arr_t);
			elem_t = LLVMGetElementType(arr_t);
			data_ptr = s_coll->val;
			len = LLVMConstInt(i64_t, alen, 0);
		} else {
			LLVMTypeRef i32_ptr_t =
				LLVMPointerType(LLVMInt32TypeInContext(ctx), 0);
			LLVMValueRef set_ptr = s_coll->val;
			LLVMValueRef len_ptr = LLVMBuildStructGEP2(c->builder, s_coll->type,
													   set_ptr, 1, "len_ptr");
			len = LLVMBuildLoad2(c->builder, i64_t, len_ptr, "len");
			attach_tbaa(c, len, i64_t);
			LLVMValueRef data_ptr_ptr = LLVMBuildStructGEP2(
				c->builder, s_coll->type, set_ptr, 0, "buf_ptr");
			data_ptr =
				LLVMBuildLoad2(c->builder, i32_ptr_t, data_ptr_ptr, "buf");
			attach_tbaa(c, data_ptr, i32_ptr_t);
			elem_t = LLVMInt32TypeInContext(ctx);
		}

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
			LLVMBuildGEP2(c->builder, elem_t, data_ptr, &idx, 1, "item_ptr");
		LLVMValueRef item_val =
			LLVMBuildLoad2(c->builder, elem_t, item_ptr, "item");
		attach_tbaa(c, item_val, elem_t);
		LLVMValueRef n_ptr =
			create_entry_block_alloca(c, elem_t, n->data.batch.iterator_var);
		LLVMBuildStore(c->builder, item_val, n_ptr);
		Scope *old_scope = c->scope_stack;
		scope_push(c, n->data.batch.iterator_var, n_ptr, elem_t, NULL);
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
		// Deferred statements run before control leaves the function, in
		// reverse registration order (LIFO).
		for (DeferFrame *d = c->defer_stack; d; d = d->next)
			codegen_stmt(c, d->stmt);

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

	case NODE_FILTER: {
		// filter { ... } dregs (err) { ... }: try body with an explicit
		// error slot. `press` stores into the slot and jumps to catch_bb.
		// No unwinding -- press is a plain branch, so nounwind survives.
		LLVMContextRef ctx = c->context;
		FilterFrame frame;
		frame.err_slot = create_entry_block_alloca(
			c, LLVMInt32TypeInContext(ctx), "filter.err");
		frame.catch_bb = LLVMAppendBasicBlock(c->current_func, "dregs");

		FilterFrame *saved_filters = c->filter_stack;
		frame.next = saved_filters;
		frame.defers_at_entry = c->defer_stack;
		c->filter_stack = &frame;

		// Defers registered inside the try block belong to this filter:
		// press runs them, and they come off the stack when the filter ends.
		DeferFrame *saved_defers = c->defer_stack;

		codegen_stmt(c, n->data.filter.try_block);

		// Normal exit through the try body: run defers registered inside,
		// then pop them so the enclosing scope won't repeat them.
		for (DeferFrame *d = c->defer_stack; d && d != saved_defers; d = d->next)
			codegen_stmt(c, d->stmt);
		c->defer_stack = saved_defers;

		c->filter_stack = saved_filters;
		LLVMBasicBlockRef merge_bb =
			LLVMAppendBasicBlock(c->current_func, "filter_merge");
		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder)))
			LLVMBuildBr(c->builder, merge_bb);
		LLVMPositionBuilderAtEnd(c->builder, frame.catch_bb);
		// Bind err_var to the SLOT (scope entries hold addresses; loads
		// happen at use sites).
		scope_push(c, n->data.filter.err_var, frame.err_slot,
				   LLVMInt32TypeInContext(ctx), NULL);
		codegen_stmt(c, n->data.filter.catch_block);
		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder)))
			LLVMBuildBr(c->builder, merge_bb);
		LLVMPositionBuilderAtEnd(c->builder, merge_bb);
		return;
	}

	case NODE_PRESS: {
		if (!c->filter_stack) {
			timbr_err("press outside of filter/dregs: nothing to catch\n");
			exit(1);
		}
		FilterFrame *target = c->filter_stack;
		LLVMValueRef val = codegen_expr(c, n->data.press.target);
		LLVMTypeRef i32_t = LLVMInt32TypeInContext(c->context);
		val = coerce_value(c, val, n->data.press.target->data_type, i32_t,
						   NULL);
		LLVMValueRef store = LLVMBuildStore(c->builder, val, target->err_slot);
		LLVMSetVolatile(store, 1);
		// Defers registered between the active filter and this press run
		// before control transfers to the handler.
		for (DeferFrame *d = c->defer_stack; d != target->defers_at_entry;
			 d = d->next)
			codegen_stmt(c, d->stmt);
		LLVMBuildBr(c->builder, target->catch_bb);
		return;
	}

	case NODE_DEFER:
		// Real defer: register, don't execute. Emitted in reverse order
		// before every return (and before press transfers control).
		{
			DeferFrame *d = arena_alloc(c->arena, sizeof(DeferFrame));
			d->stmt = n->data.defer.stmt;
			d->next = c->defer_stack;
			c->defer_stack = d;
		}
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
