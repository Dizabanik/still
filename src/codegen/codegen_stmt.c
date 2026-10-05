#include "codegen_internal.h"

static void scoped_statement(StillCompiler *c, ASTNode *n) {
	if (!n || n->type==NODE_BLOCK) { codegen_stmt(c,n); return; }
	ASTNode block={.type=NODE_BLOCK,.line=n->line};
	block.data.block.stmts=n;
	codegen_stmt(c,&block);
}

void run_defer_frame(StillCompiler *c, DeferFrame *d) {
	if (d->memory_slot) {
		if (d->memory_type) wky_memory_cleanup_value(c,d->memory_slot,d->memory_type);
		else wky_memory_cleanup(c, d->memory_slot, d->memory_unpin);
		return;
	}
	Scope *saved_scope = c->scope_stack;
	for (int i = 0; i < d->capture_count; i++) {
		scope_push(c, d->captures[i].name, d->captures[i].slot, d->captures[i].type, d->captures[i].node);
	}
	codegen_stmt(c, d->stmt);
	c->scope_stack = saved_scope;
}

void codegen_stmt(StillCompiler *c, ASTNode *n) {
	if (!n)
		return;
	if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder))) {
		// The current block already transferred control (return/break/
		// continue/press). Anything else in this statement list is dead --
		// warn once per statement so `return x; foo();` gets flagged.
		if (n->type != NODE_CASE && n->line > 0 && !c->warned_unreachable) {
			c->warned_unreachable = 1;
			still_diag_note("this statement never runs; control left the block "
					   "above it");
			still_warn(STILL_W_UNREACHABLE, n, "unreachable statement");
		}
		return;
	}

	// Attribute instructions emitted for this statement to its source line
	// (no-op unless -g). Line info is what makes profiles and stack traces
	// point at Whisky code instead of raw addresses.
	still_di_set_location(c, n->line);

	switch (n->type) {
	case NODE_STABLE:
		wky_memory_stable(c, n);
		return;

	case NODE_CALL:
	case NODE_SEND:
		if (wky_contains_managed(c,wky_expr_type(c,n),1)) {
			LLVMValueRef value = codegen_expr(c, n);
			LLVMValueRef slot = create_entry_block_alloca(c, LLVMTypeOf(value), "discarded_owner");
			LLVMBuildStore(c->builder, value, slot);
			wky_memory_cleanup_value(c,slot,wky_expr_type(c,n));
			return;
		}
		// `ch <- v;` as a statement: the send's value is discarded.
		(void)codegen_expr(c, n);
		return;
	case NODE_RECV:
		// `<-ch;` drains one element.
		(void)codegen_expr(c, n);
		return;
	case NODE_SIP: // `sip(h);` as a statement: run it for the resume side effect
	case NODE_SET_POUR:
		codegen_expr(c, n);
		return;

	case NODE_BLOCK: {
		Scope *saved_scope = c->scope_stack;
		ASTNode *saved_list = c->cur_stmt_list;
		c->cur_stmt_list = n->data.block.stmts;
		DeferFrame *saved_defers = c->defer_stack;
		for (ASTNode *s = n->data.block.stmts; s; s = s->next)
			codegen_stmt(c, s);
		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder))) {
			for (DeferFrame *d = c->defer_stack; d && d != saved_defers; d = d->next)
				run_defer_frame(c, d);
		}
		c->defer_stack = saved_defers;
		c->cur_stmt_list = saved_list;
		c->scope_stack = saved_scope;
		return;
	}

    case NODE_VAR_DECL: {
        wky_check_value_type(c,n,n->data_type);
		if (wky_contains_managed(c,n->data_type,1) && c->in_coroutine) {
			still_error(STILL_E_TYPE, n, "managed owners in coroutines require cancellation cleanup support");
			exit(1);
		}
		LLVMTypeRef var_type = get_llvm_type(c, n->data_type);
		LLVMValueRef val_ptr =
			create_entry_block_alloca(c, var_type, n->data.var_decl.name);

		LLVMValueRef init_val = NULL;
		if (n->data.var_decl.init) {
			// Propagate the declared type to a struct literal initializer
			// so it picks up the right field layout.
			if (n->data.var_decl.init->type == NODE_STRUCT_LITERAL &&
				!n->data.var_decl.init->data_type)
				n->data.var_decl.init->data_type = n->data_type;

			// Array -> slice binding: build the view from the ARRAY'S
			// ADDRESS, not from a loaded copy -- slices are views, so
			// writes through them must land in the original storage.
			int handled_slice_view = 0;
			if (LLVMGetTypeKind(var_type) == LLVMStructTypeKind &&
				n->data_type && n->data_type->kind == TYPE_SLICE &&
				n->data.var_decl.init->type == NODE_VAR_REF) {
				Scope *sv0 = scope_find(
					c, n->data.var_decl.init->data.var_ref.name);
				if (sv0 && sv0->node && sv0->node->data_type &&
					sv0->node->data_type->kind == TYPE_ARRAY) {
					Type *at = sv0->node->data_type;
					Type elem_ref = {0};
					elem_ref.kind = at->inner ? at->inner->kind : TYPE_I32;
					elem_ref.inner = at->inner ? at->inner->inner : NULL;
					elem_ref.is_signed =
						at->inner ? at->inner->is_signed : 0;
					LLVMTypeRef elem = get_llvm_type(c, at->inner);
					LLVMValueRef arr_addr = sv0->val;
					LLVMValueRef data = LLVMBuildGEP2(
						c->builder, elem, arr_addr,
						(LLVMValueRef[]){LLVMConstInt(
							LLVMInt64TypeInContext(c->context), 0, 0)},
						1, "view_data");
					init_val = LLVMGetUndef(var_type);
					init_val = LLVMBuildInsertValue(
						c->builder, init_val, data, 0, "view_ins_data");
					init_val = LLVMBuildInsertValue(
						c->builder, init_val,
						LLVMConstInt(LLVMInt64TypeInContext(c->context),
									 (unsigned long long)at->array_len, 0),
						1, "view_ins_len");
					handled_slice_view = 1;
				}
			}
			if (!handled_slice_view) {
			// Brew binding: let the coro emitter see this decl + the
			// statement list so it can prove the handle never escapes
			// and put the frame on the stack.
			ASTNode *saved_brew_decl = c->cur_brew_decl;
			c->cur_brew_decl =
				(n->data.var_decl.init->type == NODE_BREW ||
				 n->data.var_decl.init->type == NODE_CAST)
					? n
					: NULL;
			init_val = codegen_expr(c, n->data.var_decl.init);
			c->cur_brew_decl = saved_brew_decl;
			// Bare var refs carry no parser-side type; stamp from their
			// declaration so shape-aware coercions (array -> slice) fire.
			if (!n->data.var_decl.init->data_type &&
				n->data.var_decl.init->type == NODE_VAR_REF) {
				Scope *sv = scope_find(
					c, n->data.var_decl.init->data.var_ref.name);
				if (sv && sv->node && sv->node->data_type)
					n->data.var_decl.init->data_type =
						sv->node->data_type;
			}
			init_val =
				coerce_value(c, init_val, n->data.var_decl.init->data_type,
							 var_type, n->data_type);
			}
		} else {
			init_val = LLVMConstNull(var_type);
		}
		LLVMValueRef store = LLVMBuildStore(c->builder, init_val, val_ptr);
		attach_tbaa(c, store, var_type);
		// A declaration becomes visible after its initializer. Shadowing
		// reads the outer binding; a self-initializer cannot load this alloca.
		scope_push(c, n->data.var_decl.name, val_ptr, var_type, n);
		if (wky_contains_managed(c,n->data_type,1)) wky_memory_defer_value(c,val_ptr,n->data_type);
		return;
	}

	case NODE_ASSIGN: {
		ASTNode *target = n->data.assign.target;
		// Operator-provided indexed store (IDEAS 1.2): `base[i] = v` on a
		// struct with impl `self_index_set` becomes that call. Checked
		// before the ordinary lvalue path so library types never need
		// real storage for their elements.
		if (target->type == NODE_INDEX) {
			Type *bt = index_base_struct_type(
				c, target->data.index.object);
			if (bt && impl_has_method(c, bt->name, "self_index_set")) {
				char mangled[256];
				snprintf(mangled, sizeof(mangled), "%s__self_index_set",
						 bt->name);
				LLVMValueRef setter =
					LLVMGetNamedFunction(c->module, mangled);
				if (!setter) {
					still_error(STILL_E_UNDEF, n,
						 "`%s__self_index_set` is registered but was "
						 "not emitted",
						 bt->name);
					exit(1);
				}
				{
					// A setter taking `self` BY VALUE mutates a temporary
					// copy -- the store would vanish. Only pointer
					// receivers (`self*`) can implement indexed
					// assignment, so the base address passes directly.
					LLVMTypeRef recv_t =
						LLVMTypeOf(LLVMGetParam(setter, 0));
					if (!recv_t ||
						LLVMGetTypeKind(recv_t) !=
							LLVMPointerTypeKind) {
						still_error(STILL_E_ARGS, n,
							 "indexed assignment needs a pointer "
						 "receiver: declare `%s__self_index_set(self*, ...)`",
						 bt->name);
						exit(1);
					}
						LLVMTypeRef base_elem_unused = NULL;
						LLVMValueRef base_addr = get_address(
							c, target->data.index.object,
							&base_elem_unused);
						LLVMValueRef idx = codegen_expr(
							c, target->data.index.index);
						// Stamp bare VAR_REF indices with their
						// declared type so signedness survives widening.
						if (!target->data.index.index->data_type &&
							target->data.index.index->type ==
								NODE_VAR_REF) {
							Scope *sv = scope_find(
								c, target->data.index.index->data.var_ref
									   .name);
							if (sv && sv->node && sv->node->data_type)
								target->data.index.index->data_type =
									sv->node->data_type;
						}
						LLVMTypeRef idx_want =
							LLVMTypeOf(LLVMGetParam(setter, 1));
						if (idx_want &&
							LLVMGetTypeKind(idx_want) ==
								LLVMIntegerTypeKind &&
							LLVMTypeOf(idx) != idx_want) {
							Type dsti = {0};
							dsti.kind = TYPE_I64;
							dsti.is_signed = 1;
							idx = coerce_value(
								c, idx,
								target->data.index.index->data_type,
								idx_want, &dsti);
						}
						LLVMValueRef val = codegen_expr(
							c, n->data.assign.value);
						// LLVM forbids naming instructions that
						// produce no value -- void calls must pass "".
						LLVMBuildCall2(
							c->builder,
							LLVMGlobalGetValueType(setter), setter,
							(LLVMValueRef[]){base_addr, idx, val}, 3,
							"");
						return;
				}
			}
		}
		LLVMTypeRef target_type = NULL;
        LLVMValueRef container=NULL;
        LLVMValueRef target_ptr=wky_memory_lvalue(c,target,&target_type,&container);
        if (!target_ptr) target_ptr=get_address(c,target,&target_type);
		for (StableFrame *f = c->stable_stack; f; f = f->next) {
			if (f->slot == target_ptr) {
				still_error(STILL_E_TYPE, n, "cannot reassign a stable binding");
				exit(1);
			}
		}
        Type *target_ast=wky_expr_type(c,target);
        int owning_field=wky_is_owner(target_ast);
        if (!wky_is_owner(target_ast)) wky_check_value_type(c,n,target_ast);
        if (owning_field && container && target_ast->kind==TYPE_ARENA) {
            still_error(STILL_E_TYPE,n,"arenas cannot be embedded in owning slots");
            exit(1);
        }
        if (owning_field && n->data.assign.value->type==NODE_CALL &&
            !n->data.assign.value->data_type)
            n->data.assign.value->data_type=target_ast;
		if (!target_ptr || !target_type) {
			still_error(STILL_E_SEMANTIC, n,
				 "cannot resolve assignment target"); // internal
			exit(1);
		}
        wky_literal_context(c,n->data.assign.value,target_ast);

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

		// Slice target + array source: bind a view over the ORIGINAL
		// array storage (same reasoning as the decl path).
		LLVMValueRef val = NULL;
		int handled_slice_view = 0;
		Type *tgt_ast = target->data_type;
		if (!tgt_ast && target->type == NODE_VAR_REF) {
			Scope *tv = scope_find(c, target->data.var_ref.name);
			if (tv && tv->node)
				tgt_ast = tv->node->data_type;
		}
		if (LLVMGetTypeKind(target_type) == LLVMStructTypeKind &&
			tgt_ast && tgt_ast->kind == TYPE_SLICE &&
			n->data.assign.value->type == NODE_VAR_REF) {
			Scope *sv0 = scope_find(
				c, n->data.assign.value->data.var_ref.name);
			if (sv0 && sv0->node && sv0->node->data_type &&
				sv0->node->data_type->kind == TYPE_ARRAY) {
				Type *at = sv0->node->data_type;
				LLVMTypeRef elem = get_llvm_type(c, at->inner);
				LLVMValueRef data = LLVMBuildGEP2(
					c->builder, elem, sv0->val,
					(LLVMValueRef[]){LLVMConstInt(
						LLVMInt64TypeInContext(c->context), 0, 0)},
					1, "view_data");
				val = LLVMGetUndef(target_type);
				val = LLVMBuildInsertValue(c->builder, val, data, 0,
											"view_ins_data");
				val = LLVMBuildInsertValue(
					c->builder, val,
					LLVMConstInt(LLVMInt64TypeInContext(c->context),
								 (unsigned long long)at->array_len, 0),
					1, "view_ins_len");
				handled_slice_view = 1;
			}
		}
		if (!handled_slice_view)
			val = codegen_expr(c, n->data.assign.value);
		if (!handled_slice_view) {
		if (!n->data.assign.value->data_type &&
			n->data.assign.value->type == NODE_VAR_REF) {
			Scope *sv = scope_find(
				c, n->data.assign.value->data.var_ref.name);
			if (sv && sv->node && sv->node->data_type)
				n->data.assign.value->data_type = sv->node->data_type;
		}
		val = coerce_value(c, val, n->data.assign.value->data_type, target_type,
						   target->data_type);
		}
        if (owning_field) {
            wky_memory_store_owner(c,target_ptr,val,container);
            return;
        }
        if (wky_contains_managed(c,target_ast,1)) {
            wky_memory_store_value(c,target_ptr,val,target_ast,container);
            return;
        }
        if (wky_expr_may_invalidate(c,n->data.assign.value))
            target_ptr=wky_memory_write_address(c,target_ptr,target_type,container);
        else if (container)
            still_report_site(c,n,"lifetime_and_extent","__wky_mem_write_address",
                             "built-in right-hand side expressions cannot invalidate managed storage");
        LLVMValueRef store = LLVMBuildStore(c->builder, val, target_ptr);
		attach_tbaa(c, store, target_type);
		return;
	}

	case NODE_IF: {
		LLVMValueRef cond_val =
			cond_to_bool(c, codegen_expr(c, n->data.if_stmt.cond));
		LLVMValueRef func = c->current_func;
		LLVMBasicBlockRef then_bb = wky_append_block(func, "then");
		LLVMBasicBlockRef else_bb = n->data.if_stmt.else_block
										? wky_append_block(func, "else")
										: NULL;
		LLVMBasicBlockRef merge_bb = wky_append_block(func, "if_cont");
		LLVMValueRef br_instr = LLVMBuildCondBr(c->builder, cond_val, then_bb,
												else_bb ? else_bb : merge_bb);
		set_branch_weights(c, br_instr, 1, 1);
		LLVMPositionBuilderAtEnd(c->builder, then_bb);
		scoped_statement(c, n->data.if_stmt.then_block);
		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder)))
			LLVMBuildBr(c->builder, merge_bb);
		if (else_bb) {
			LLVMPositionBuilderAtEnd(c->builder, else_bb);
			scoped_statement(c, n->data.if_stmt.else_block);
			if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder)))
				LLVMBuildBr(c->builder, merge_bb);
		}
		LLVMPositionBuilderAtEnd(c->builder, merge_bb);
		return;
	}

	case NODE_WHILE: {
		LLVMBasicBlockRef cond_bb =
			wky_append_block(c->current_func, "while_cond");
		LLVMBasicBlockRef body_bb =
			wky_append_block(c->current_func, "while_body");
		LLVMBasicBlockRef exit_bb =
			wky_append_block(c->current_func, "while_exit");
		LLVMBuildBr(c->builder, cond_bb);
		LLVMPositionBuilderAtEnd(c->builder, cond_bb);
		LLVMValueRef cond_val =
			cond_to_bool(c, codegen_expr(c, n->data.while_stmt.cond));
		LLVMValueRef br =
			LLVMBuildCondBr(c->builder, cond_val, body_bb, exit_bb);
		set_branch_weights(c, br, 64, 1); // loops iterate more often than not
		LLVMPositionBuilderAtEnd(c->builder, body_bb);

		struct LoopTargets targets = {exit_bb, cond_bb, c->defer_stack, c->loop_stack};
		c->loop_stack = &targets;
		scoped_statement(c, n->data.while_stmt.body);
		c->loop_stack = targets.next;

		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder)))
			LLVMBuildBr(c->builder, cond_bb);
		LLVMPositionBuilderAtEnd(c->builder, exit_bb);
		return;
	}

	case NODE_FOR: {
		Scope *for_scope = c->scope_stack;
		DeferFrame *for_defers = c->defer_stack;
		// for init; cond; step { body }
		// Lowered as its own block structure so `continue` lands on the
		// step (not the condition) and per-iteration allocas stay scoped.
		if (n->data.for_stmt.init)
			codegen_stmt(c, n->data.for_stmt.init);

		LLVMBasicBlockRef cond_bb =
			wky_append_block(c->current_func, "for_cond");
		LLVMBasicBlockRef body_bb =
			wky_append_block(c->current_func, "for_body");
		LLVMBasicBlockRef step_bb =
			wky_append_block(c->current_func, "for_step");
		LLVMBasicBlockRef exit_bb =
			wky_append_block(c->current_func, "for_exit");

		LLVMBuildBr(c->builder, cond_bb);
		LLVMPositionBuilderAtEnd(c->builder, cond_bb);
		if (n->data.for_stmt.cond) {
			LLVMValueRef cond_val =
				cond_to_bool(c, codegen_expr(c, n->data.for_stmt.cond));
			LLVMValueRef br =
				LLVMBuildCondBr(c->builder, cond_val, body_bb, exit_bb);
			set_branch_weights(c, br, 64, 1);
		} else {
			LLVMBuildBr(c->builder, body_bb); // `for (;;)` = infinite
		}

		LLVMPositionBuilderAtEnd(c->builder, body_bb);
		struct LoopTargets targets = {exit_bb, step_bb, c->defer_stack, c->loop_stack};
		c->loop_stack = &targets;

		struct LoopRange lr = {0};
		int has_lr = 0;
		if (n->data.for_stmt.cond && n->data.for_stmt.cond->type == NODE_BINARY_OP) {
			ASTNode *left = n->data.for_stmt.cond->data.bin_op.left;
			ASTNode *right = n->data.for_stmt.cond->data.bin_op.right;
			int op = n->data.for_stmt.cond->data.bin_op.op;
			if (left && left->type == NODE_VAR_REF && right && right->type == NODE_LITERAL) {
				if (op == TOK_LANGLE || op == TOK_LEQ) {
					lr.var_name = left->data.var_ref.name;
					lr.upper_bound = right->data.literal.i64_val;
					lr.is_inclusive = (op == TOK_LEQ);
					lr.parent = c->loop_ranges;
					c->loop_ranges = &lr;
					has_lr = 1;
				}
			}
		}

		scoped_statement(c, n->data.for_stmt.body);

		if (has_lr)
			c->loop_ranges = lr.parent;

		c->loop_stack = targets.next;

		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder)))
			LLVMBuildBr(c->builder, step_bb);
		LLVMPositionBuilderAtEnd(c->builder, step_bb);
		if (n->data.for_stmt.step)
			codegen_stmt(c, n->data.for_stmt.step);
		LLVMBuildBr(c->builder, cond_bb);
		LLVMPositionBuilderAtEnd(c->builder, exit_bb);
		for (DeferFrame *d = c->defer_stack; d && d != for_defers; d = d->next)
			run_defer_frame(c, d);
		c->defer_stack = for_defers;
		c->scope_stack = for_scope;
		return;
	}

	case NODE_SWITCH: {
		// C semantics: the switched value selects a case body; bodies
		// fall through into each other unless interrupted (break/return).
		// `break` binds to this switch (loop_stack push shadows any outer
		// loop's break); `continue` still reaches the enclosing loop.
		LLVMValueRef cond = codegen_expr(c, n->data.switch_stmt.value);

		if (LLVMGetTypeKind(LLVMTypeOf(cond)) != LLVMIntegerTypeKind) {
			still_error(STILL_E_TYPE, n, "switch value must be an integer");
			exit(1);
		}
		unsigned bits = LLVMGetIntTypeWidth(LLVMTypeOf(cond));

		LLVMContextRef ctx = c->context;
		LLVMBasicBlockRef exit_bb =
			wky_append_block(c->current_func, "switch_exit");

		// One body block per case, in source order -- fallthrough is then
		// just "no terminator at the end of the previous body".
		int case_count = 0;
		for (ASTNode *cs = n->data.switch_stmt.cases; cs; cs = cs->next)
			case_count++;

		LLVMBasicBlockRef *body_bbs = arena_alloc(
			c->arena,
			sizeof(LLVMBasicBlockRef) * (case_count > 0 ? case_count : 1));
		LLVMBasicBlockRef default_bb = NULL;
		int i = 0;
		for (ASTNode *cs = n->data.switch_stmt.cases; cs; cs = cs->next, i++) {
			body_bbs[i] = wky_append_block(c->current_func, "case_body");
			if (!cs->data.case_stmt.expr)
				default_bb = body_bbs[i];
		}

		struct LoopTargets targets = {exit_bb, NULL, c->defer_stack, c->loop_stack};
		// `continue` inside a switch belongs to the enclosing loop; pass it
		// through so NODE_CONTINUE resolves against the right target (or
		// errors with its own message when there is no loop).
		targets.continue_bb =
			c->loop_stack ? c->loop_stack->continue_bb : NULL;

		// Dispatch: LLVMBuildSwitch needs the default destination up front.
		// Case labels are compile-time integers -- literals, consts, enum
		// members, const arithmetic -- folded via const_eval_i64 and
		// truncated to the selector's width (bit pattern compare).
		c->loop_stack = &targets;
		LLVMValueRef switch_instr = NULL;
		long long *seen_vals =
			arena_alloc(c->arena, sizeof(long long) *
									  (case_count > 0 ? case_count : 1));
		int seen_count = 0;
		i = 0;
		for (ASTNode *cs = n->data.switch_stmt.cases; cs; cs = cs->next, i++) {
			if (!cs->data.case_stmt.expr)
				continue; // default: handled as the dispatch fallback
			long long cv = 0;
			if (!const_eval_i64(c, cs->data.case_stmt.expr, &cv)) {
				still_error(STILL_E_ARGS, n,
					 "case value must be a compile-time integer "
					 "(literal or const)");
				exit(1);
			}
			unsigned long long raw =
				(unsigned long long)cv &
				(bits >= 64 ? ~0ULL : ((1ULL << bits) - 1ULL));
			// Duplicate labels are a user error -- the verifier would reject
			// them anyway, but this reports the actual case values.
			for (int k = 0; k < seen_count; k++) {
				if (((unsigned long long)seen_vals[k] &
					 (bits >= 64 ? ~0ULL : ((1ULL << bits) - 1ULL))) == raw) {
					char case_txt[32];
					snprintf(case_txt, sizeof(case_txt), "%lld", cv);
					still_diag_note("value first used by the earlier arm with this label");
					still_error_named(STILL_E_SEMANTIC, cs, case_txt,
						  "duplicate case value %lld in switch", cv);
					exit(1);
				}
			}
			seen_vals[seen_count++] = cv;
			LLVMValueRef case_const =
				LLVMConstInt(LLVMIntTypeInContext(ctx, bits), raw, 0);
			if (!switch_instr) {
				switch_instr = LLVMBuildSwitch(
					c->builder, cond, default_bb ? default_bb : exit_bb,
					case_count);
			}
			LLVMAddCase(switch_instr, case_const, body_bbs[i]);
		}
		if (!switch_instr) {
			// No constant cases at all: control goes to default/exit.
			LLVMBuildBr(c->builder, default_bb ? default_bb : exit_bb);
		}

		// Bodies, in order. Each falls through to the next by omitting a
		// terminator when the source did (C fallthrough); break/return
		// terminate their own block.
		i = 0;
		for (ASTNode *cs = n->data.switch_stmt.cases; cs; cs = cs->next, i++) {
			LLVMPositionBuilderAtEnd(c->builder, body_bbs[i]);
			codegen_stmt(c, cs->data.case_stmt.body);
			if (!LLVMGetBasicBlockTerminator(
					LLVMGetInsertBlock(c->builder))) {
				LLVMBuildBr(c->builder, cs->next ? body_bbs[i + 1] : exit_bb);
			}
		}

		c->loop_stack = targets.next;
		LLVMPositionBuilderAtEnd(c->builder, exit_bb);
		return;
	}

	case NODE_BREAK:
	case NODE_CONTINUE: {
		if (!c->loop_stack) {
			still_diag_help("`while` and `batch ... in` introduce loops");
			still_error(STILL_E_SCOPE, n, "%s outside of a loop",
				 n->type == NODE_BREAK ? "break" : "continue");
			exit(1);
		}
		for (DeferFrame *d = c->defer_stack; d && d != c->loop_stack->defers_at_entry; d = d->next)
			run_defer_frame(c, d);
		LLVMBuildBr(c->builder, n->type == NODE_BREAK
									? c->loop_stack->break_bb
									: c->loop_stack->continue_bb);
		return;
	}

	case NODE_BATCH: {
		ASTNode *coll = n->data.batch.collection;
		LLVMContextRef ctx = c->context;
		LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx);

		LLVMValueRef data_ptr = NULL, len = NULL;
		LLVMTypeRef elem_t = NULL;
		Type *elem_ast = NULL;

		if (coll->type == NODE_VAR_REF) {
			Scope *s_coll = scope_find(c, coll->data.var_ref.name);
			if (!s_coll) {
				const char *cands[33];
				int nc = 0;
				for (Scope *cur = c->scope_stack; cur && nc < 32; cur = cur->next)
					cands[nc++] = cur->name;
				cands[nc] = NULL;
				const char *alt = still_diag_closest(coll->data.var_ref.name, cands);
				if (alt)
					still_diag_help("a variable with a similar name exists: `%s`",
							   get_var_path(c, alt));
				still_error_named(STILL_E_UNDEF, n, coll->data.var_ref.name,
					  "cannot find variable `%s` in this scope",
					  get_var_path(c, coll->data.var_ref.name));
				exit(1);
			}

			Type *coll_ast =
				(s_coll->node) ? s_coll->node->data_type : NULL;
			int is_array = (coll_ast && coll_ast->kind == TYPE_ARRAY);
			int is_slice = (coll_ast && coll_ast->kind == TYPE_SLICE);
			if (is_slice) {
				LLVMTypeRef slice_t = get_llvm_type(c, coll_ast);
				elem_t = get_llvm_type(c, coll_ast->inner);
				elem_ast = coll_ast->inner;
				LLVMTypeRef ptr_t = LLVMPointerType(elem_t, 0);
				LLVMValueRef len_ptr2 = LLVMBuildStructGEP2(
					c->builder, slice_t, s_coll->val, 1, "len_ptr");
				len = LLVMBuildLoad2(c->builder, i64_t, len_ptr2, "len");
				attach_tbaa(c, len, i64_t);
				LLVMValueRef data_pp = LLVMBuildStructGEP2(
					c->builder, slice_t, s_coll->val, 0, "buf_ptr");
				data_ptr =
					LLVMBuildLoad2(c->builder, ptr_t, data_pp, "buf");
				attach_tbaa(c, data_ptr, ptr_t);
			} else if (is_array) {
				LLVMTypeRef arr_t = s_coll->type;
				unsigned alen = LLVMGetArrayLength(arr_t);
				elem_t = LLVMGetElementType(arr_t);
				elem_ast = coll_ast ? coll_ast->inner : NULL;
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
		} else {
			LLVMValueRef coll_val = codegen_expr(c, coll);
			LLVMTypeRef cty = LLVMTypeOf(coll_val);
			if (LLVMGetTypeKind(cty) == LLVMStructTypeKind) {
				data_ptr = LLVMBuildExtractValue(c->builder, coll_val, 0, "batch_coll_buf");
				len = LLVMBuildExtractValue(c->builder, coll_val, 1, "batch_coll_len");
				if (coll->data_type && coll->data_type->inner) {
					elem_ast = coll->data_type->inner;
					elem_t = get_llvm_type(c, elem_ast);
				} else {
					elem_t = LLVMGetElementType(LLVMTypeOf(data_ptr));
				}
			} else {
				still_error(STILL_E_ARGS, n, "batch requires a slice, array, or collection");
				exit(1);
			}
		}

		if (!elem_ast) {
			elem_ast = arena_alloc(c->arena, sizeof(Type));
			elem_ast->kind = TYPE_I32;
		}
		if (!elem_t)
			elem_t = LLVMInt32TypeInContext(ctx);

		LLVMBasicBlockRef prev_bb = LLVMGetInsertBlock(c->builder);
		LLVMBasicBlockRef loop_bb =
			wky_append_block(c->current_func, "batch_loop");
		LLVMBasicBlockRef body_bb =
			wky_append_block(c->current_func, "batch_body");
		LLVMBasicBlockRef exit_bb =
			wky_append_block(c->current_func, "batch_exit");

		LLVMValueRef zero = LLVMConstInt(i64_t, 0, 0);
		LLVMBuildBr(c->builder, loop_bb);
		LLVMPositionBuilderAtEnd(c->builder, loop_bb);
		LLVMValueRef idx = LLVMBuildPhi(c->builder, i64_t, "idx");
		LLVMValueRef cmp =
			LLVMBuildICmp(c->builder, LLVMIntULT, idx, len, "loop_cond");
		LLVMBuildCondBr(c->builder, cmp, body_bb, exit_bb);
		LLVMPositionBuilderAtEnd(c->builder, body_bb);

		LLVMValueRef item_ptr =
			LLVMBuildGEP2(c->builder, elem_t, data_ptr, &idx, 1, "item_ptr");
		LLVMValueRef item_val =
			LLVMBuildLoad2(c->builder, elem_t, item_ptr, "item");
		attach_tbaa(c, item_val, elem_t);
		LLVMValueRef n_ptr =
			create_entry_block_alloca(c, elem_t, n->data.batch.iterator_var);
		LLVMBuildStore(c->builder, item_val, n_ptr);

		ASTNode *iter_decl = arena_alloc(c->arena, sizeof(ASTNode));
		iter_decl->type = NODE_VAR_DECL;
		iter_decl->data.var_decl.name = n->data.batch.iterator_var;
		iter_decl->data_type = elem_ast;

		Scope *old_scope = c->scope_stack;
		scope_push(c, n->data.batch.iterator_var, n_ptr, elem_t, iter_decl);

		struct LoopTargets targets = {exit_bb, loop_bb, c->defer_stack, c->loop_stack};
		c->loop_stack = &targets;
		codegen_stmt(c, n->data.batch.body);
		c->loop_stack = targets.next;

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
		if (n->data.ret_stmt.expr &&
			n->data.ret_stmt.expr->type == NODE_STRUCT_LITERAL &&
			!n->data.ret_stmt.expr->data_type) {
			n->data.ret_stmt.expr->data_type = c->current_ret_node_type;
		}
		LLVMValueRef ret_val = NULL;
		if (n->data.ret_stmt.expr != NULL) {
			ret_val = codegen_expr(c, n->data.ret_stmt.expr);
		}

		// Deferred statements run before control leaves the function, in
		// reverse registration order (LIFO).
		for (DeferFrame *d = c->defer_stack; d; d = d->next)
			run_defer_frame(c, d);

		if (n->data.ret_stmt.expr == NULL) {
			// bare `return;` -- runs defers, then leaves. Valid in void
			// functions; in drips it finishes without a final value.
			if (c->in_coroutine) {
				LLVMBuildBr(c->builder, c->coro_cleanup_block);
			} else if (LLVMGetTypeKind(c->current_ret_type) ==
					   LLVMVoidTypeKind) {
				LLVMBuildRetVoid(c->builder);
			} else {
				still_diag_note(
					"the enclosing function's declared return type is "
					"not void");
				still_error(STILL_E_TYPE, n, "`return;` in a non-void function");
				return;
			}
			return;
		}
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
			if (c->uses_print && c->current_func && strcmp(LLVMGetValueName(c->current_func), "main") == 0) {
				LLVMValueRef flush_fn = declare_wky_runtime_fn(c, "__wky_flush");
				if (flush_fn)
					LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(flush_fn), flush_fn, NULL, 0, "");
			}
			if (LLVMGetTypeKind(c->current_ret_type) == LLVMVoidTypeKind)
				LLVMBuildRetVoid(c->builder);
			else {
				ret_val =
					coerce_value(c, ret_val, n->data.ret_stmt.expr->data_type,
								 c->current_ret_type, c->current_ret_node_type);
				LLVMBuildRet(c->builder, ret_val);
			}
		}
		return;
	}

	case NODE_SELECT: {
		// select { case v = <- ch: ... default: ... }: poll each channel
		// in declaration order and run the FIRST ready one. Nothing ready
		// with a default runs the default; nothing ready without one
		// yields (cooperative block) and re-polls.
		LLVMContextRef ctx = c->context;
		LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx);

		int ncases = 0;
		for (struct SelectCase *cs2 = n->data.select_stmt.cases; cs2;
			 cs2 = cs2->next)
			ncases++;
		if (ncases > 8) {
			still_error(STILL_E_ARGS, n, "select supports at most 8 channels");
			exit(1);
		}

		struct SelectCase *cases[8] = {0};
		LLVMValueRef chan_addrs[8] = {0};
		LLVMTypeRef chan_ts[8] = {0};
		int ci = 0;
		for (struct SelectCase *cs2 = n->data.select_stmt.cases; cs2;
			 cs2 = cs2->next, ci++) {
			cases[ci] = cs2;
			Type *ct2 = cs2->chan->data_type;
			if (!ct2 || ct2->kind != TYPE_CHAN) {
				still_error(STILL_E_TYPE, n, "select case requires a chan<T>");
				exit(1);
			}
			chan_ts[ci] = get_llvm_type(c, ct2);
			chan_addrs[ci] = get_address(c, cs2->chan, NULL);
		}

		LLVMBasicBlockRef retry_bb =
			wky_append_block(c->current_func, "sel_retry");
		LLVMBasicBlockRef none_bb =
			wky_append_block(c->current_func, "sel_none");
		LLVMBasicBlockRef done_bb =
			wky_append_block(c->current_func, "sel_done");
		LLVMBasicBlockRef case_bbs[8];
		for (int k = 0; k < ncases; k++)
			case_bbs[k] =
				wky_append_block(c->current_func, "sel_case");

		LLVMBuildBr(c->builder, retry_bb);

		// Poll chain: each channel is checked in declaration order; the
		// first with cnt > 0 wins, a fully-empty chain falls to none_bb.
		LLVMPositionBuilderAtEnd(c->builder, retry_bb);
		for (int k = 0; k < ncases; k++) {
			LLVMValueRef cnt_p = LLVMBuildStructGEP2(
				c->builder, chan_ts[k], chan_addrs[k], 3, "");
			LLVMValueRef cnt =
				LLVMBuildLoad2(c->builder, i64_t, cnt_p, "sel_cnt");
			LLVMValueRef ready = LLVMBuildICmp(
				c->builder, LLVMIntUGT, cnt,
				LLVMConstInt(i64_t, 0, 0), "sel_ready");
			LLVMBasicBlockRef next_poll =
				(k + 1 < ncases) ? wky_append_block(
									   c->current_func, "sel_poll")
								 : none_bb;
			LLVMValueRef br = LLVMBuildCondBr(c->builder, ready,
											  case_bbs[k], next_poll);
			set_branch_weights(c, br, 1, 99);
			if (k + 1 < ncases)
				LLVMPositionBuilderAtEnd(c->builder, next_poll);
		}
		if (ncases == 0)
			LLVMBuildBr(c->builder, none_bb);

		for (int k = 0; k < ncases; k++) {
			LLVMPositionBuilderAtEnd(c->builder, case_bbs[k]);
			// Receive from channel k (guaranteed non-empty here).
			struct SelectCase *cs = cases[k];
			Type *elem_ast =
				cs->chan->data_type ? cs->chan->data_type->inner : NULL;
			ASTNode recv_node = {0};
			recv_node.type = NODE_RECV;
			recv_node.data_type = elem_ast;
			recv_node.line = n->line;
			recv_node.data.recv.chan = cs->chan;
			LLVMValueRef val = codegen_expr(c, &recv_node);

			Scope *saved_scope = c->scope_stack;
			if (cs->var_decl) {
				LLVMValueRef vptr = create_entry_block_alloca(
					c, LLVMTypeOf(val), cs->var_decl->data.var_decl.name);
				LLVMBuildStore(c->builder, val, vptr);
				scope_push(c, cs->var_decl->data.var_decl.name, vptr,
						   LLVMTypeOf(val), cs->var_decl);
			}
			codegen_stmt(c, cs->body);
			c->scope_stack = saved_scope;
			if (!LLVMGetBasicBlockTerminator(
					LLVMGetInsertBlock(c->builder)))
				LLVMBuildBr(c->builder, done_bb);
		}

		LLVMPositionBuilderAtEnd(c->builder, none_bb);
		if (n->data.select_stmt.has_default) {
			codegen_stmt(c, n->data.select_stmt.default_body);
			if (!LLVMGetBasicBlockTerminator(
					LLVMGetInsertBlock(c->builder)))
				LLVMBuildBr(c->builder, done_bb);
		} else if (c->in_coroutine && c->current_coro_hdl) {
			LLVMValueRef save_token =
				LLVMBuildCall2(c->builder, c->coro_save_type,
							   c->coro_save, &c->current_coro_hdl, 1,
							   "save");
			LLVMValueRef susp = LLVMBuildCall2(
				c->builder, c->coro_suspend_type, c->coro_suspend,
				(LLVMValueRef[]){save_token,
								 LLVMConstInt(
									 LLVMInt1TypeInContext(ctx), 0, 0)},
				2, "yield");
			LLVMBasicBlockRef resume_bb2 =
				wky_append_block(c->current_func, "sel_resume");
			LLVMValueRef sw = LLVMBuildSwitch(c->builder, susp,
											  c->coro_suspend_block, 2);
			LLVMAddCase(sw,
						LLVMConstInt(LLVMInt8TypeInContext(ctx), 0, 0),
						resume_bb2);
			LLVMAddCase(sw,
						LLVMConstInt(LLVMInt8TypeInContext(ctx), 1, 0),
						c->coro_cleanup_block);
			LLVMPositionBuilderAtEnd(c->builder, resume_bb2);
			LLVMBuildBr(c->builder, retry_bb);
		} else {
			still_diag_note("add a `default:` arm or run inside a drip so select "
					   "can yield");
			still_error(STILL_E_SEMANTIC, n,
				 "select with no ready case blocks forever (no default, "
				 "not in a coroutine)");
			exit(1);
		}

		LLVMPositionBuilderAtEnd(c->builder, done_bb);
		return;
	}

	case NODE_FILTER: {
		// filter { ... } dregs (err) { ... }: try body with an explicit
		// error slot. `press` stores into the slot and jumps to catch_bb.
		// No unwinding -- press is a plain branch, so nounwind survives.
		// The slot carries the declared payload type (`dregs (e: ParseErr)`);
		// a typeless dregs keeps the legacy i32 slot.
		LLVMContextRef ctx = c->context;
		Type *payload = n->data.filter.err_type;
        wky_check_value_type(c,n,payload);
        if (c->in_coroutine && wky_contains_managed(c,payload,1)) {
            still_error(STILL_E_OWNERSHIP,n,"owned error payloads in coroutines require cancellation cleanup support");
            exit(1);
        }
		LLVMTypeRef slot_t =
			payload ? get_llvm_type(c, payload)
					: LLVMInt32TypeInContext(ctx);
		FilterFrame frame;
		frame.err_slot = create_entry_block_alloca(c, slot_t, "filter.err");
		frame.err_type = payload;
		frame.catch_bb = wky_append_block(c->current_func, "dregs");

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
			run_defer_frame(c, d);
		c->defer_stack = saved_defers;

		c->filter_stack = saved_filters;
		LLVMBasicBlockRef merge_bb =
			wky_append_block(c->current_func, "filter_merge");
		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder)))
			LLVMBuildBr(c->builder, merge_bb);
		LLVMPositionBuilderAtEnd(c->builder, frame.catch_bb);
        Scope *catch_scope=c->scope_stack;
		// Bind err_var to the SLOT (scope entries hold addresses; loads
		// happen at use sites).
		{
			ASTNode *bind = arena_alloc(c->arena, sizeof(ASTNode));
			bind->type = NODE_VAR_DECL;
			bind->data.var_decl.name = n->data.filter.err_var;
			bind->data_type = payload;
			if (!bind->data_type) {
				bind->data_type =
					arena_alloc(c->arena, sizeof(Type));
				bind->data_type->kind = TYPE_I32;
			}
			scope_push(c, n->data.filter.err_var, frame.err_slot, slot_t,
					   bind);
		}
        if (wky_contains_managed(c,payload,1)) wky_memory_defer_value(c,frame.err_slot,payload);
		codegen_stmt(c, n->data.filter.catch_block);
		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder))) {
            if (wky_contains_managed(c,payload,1)) wky_memory_cleanup_value(c,frame.err_slot,payload);
			LLVMBuildBr(c->builder, merge_bb);
        }
        c->defer_stack=saved_defers;
        c->scope_stack=catch_scope;
		LLVMPositionBuilderAtEnd(c->builder, merge_bb);
		return;
	}

	case NODE_PRESS: {
		if (!c->filter_stack) {
			still_error(STILL_E_SCOPE, n,
				 "press outside of filter/dregs: nothing to catch");
			exit(1);
		}
		FilterFrame *target = c->filter_stack;
		LLVMContextRef ctx = c->context;
		Type *dst_ast = target->err_type;
		LLVMTypeRef slot_t =
			dst_ast ? get_llvm_type(c, dst_ast)
					: LLVMInt32TypeInContext(ctx);
		// A struct literal pressed straight at a typed handler inherits the
		// payload type (same propagation the var-decl path does) so its
		// field layout resolves.
		if (n->data.press.target->type == NODE_STRUCT_LITERAL &&
			!n->data.press.target->data_type)
			n->data.press.target->data_type = dst_ast;
		LLVMValueRef val = codegen_expr(c, n->data.press.target);
		val = coerce_value(c, val, n->data.press.target->data_type, slot_t,
						   dst_ast);
		LLVMBuildStore(c->builder, val, target->err_slot);
		// Defers registered between the active filter and this press run
		// before control transfers to the handler.
		for (DeferFrame *d = c->defer_stack; d != target->defers_at_entry;
			 d = d->next)
			run_defer_frame(c, d);
		LLVMBuildBr(c->builder, target->catch_bb);
		return;
	}

	case NODE_DEFER:
		// Real defer: register, don't execute. Emitted in reverse order
		// before every return (and before press transfers control).
		{
			DeferFrame *d = arena_alloc(c->arena, sizeof(DeferFrame));
			d->stmt = n->data.defer.stmt;
			d->capture_count = 0;
			for (ASTNode *cap = n->data.defer.captures; cap && d->capture_count < 16; cap = cap->next) {
				Scope *s = scope_find(c, cap->data.var_decl.name);
				if (s) {
					if (wky_contains_managed(c, s->node ? s->node->data_type : NULL, 1)) {
						still_error(STILL_E_OWNERSHIP, n, "defer captures cannot copy an owner; borrow it without a capture list");
						exit(1);
					}
					LLVMValueRef shadow = create_entry_block_alloca(c, s->type, "defer_cap");
					LLVMValueRef cur_val = LLVMBuildLoad2(c->builder, s->type, s->val, "cap_val");
					LLVMBuildStore(c->builder, cur_val, shadow);
					d->captures[d->capture_count].name = s->name;
					d->captures[d->capture_count].slot = shadow;
					d->captures[d->capture_count].type = s->type;
					d->captures[d->capture_count].node = s->node;
					d->capture_count++;
				}
			}
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
			wky_append_block(c->current_func, "resume");
		LLVMValueRef sw =
			LLVMBuildSwitch(c->builder, suspend, c->coro_suspend_block, 2);
		LLVMAddCase(sw, LLVMConstInt(LLVMInt8TypeInContext(c->context), 0, 0),
					resume_bb);
		LLVMAddCase(sw, LLVMConstInt(LLVMInt8TypeInContext(c->context), 1, 0),
					c->coro_cleanup_block);
		LLVMPositionBuilderAtEnd(c->builder, resume_bb);
		return;
	}

	case NODE_UNCHECKED_BLOCK:
		// No bounds checks inside, even at --debug. The depth counter
		// makes nesting work for free; every check site asks
		// `c->unchecked_depth || !c->debug_build`.
		c->unchecked_depth++;
		if (n->data.block.stmts)
			codegen_stmt(c, n->data.block.stmts);
		c->unchecked_depth--;
		return;

	case NODE_MATCH:
		codegen_match(c, n, NULL, NULL);
		return;

	case NODE_ASM: {
		const char *cons =
			n->data.asm_block.constraints ? n->data.asm_block.constraints : "";
		LLVMTypeRef asm_t = LLVMFunctionType(
			LLVMVoidTypeInContext(c->context), NULL, 0, 0);
		LLVMValueRef asm_val = LLVMGetInlineAsm(
			asm_t, n->data.asm_block.asm_template,
			strlen(n->data.asm_block.asm_template), cons, strlen(cons),
			/*hasSideEffects*/ 1, /*isAlignStack*/ 0,
			LLVMInlineAsmDialectATT, /*CanThrow*/ 0);
		LLVMBuildCall2(c->builder, asm_t, asm_val, NULL, 0, "");
		return;
	}
	default:
		break;
	}

	still_diag_error_at(STILL_E_SEMANTIC,
				   c->source_filename ? c->source_filename : "<wky>", NULL,
				   n && n->line > 0 ? n->line : 0,
				   "unknown AST node type %d in codegen_stmt", // internal
				   n->type);
	exit(1);
}

void codegen_match(StillCompiler *c, ASTNode *n, LLVMValueRef res_slot, LLVMTypeRef res_type) {
	ASTNode *target = n->data.match_stmt.target;
	LLVMValueRef target_val = codegen_expr(c, target);
	LLVMTypeRef enum_t = LLVMTypeOf(target_val);
	Type *target_type = wky_expr_type(c,target);
	if (!target_type || target_type->kind != TYPE_ENUM || LLVMGetTypeKind(enum_t) != LLVMStructTypeKind) {
		still_error(STILL_E_TYPE, n, "match requires a tagged enum value");
		exit(1);
	}

	LLVMValueRef match_slot = create_entry_block_alloca(c, enum_t, "match_target");
	LLVMBuildStore(c->builder, target_val, match_slot);

	LLVMValueRef tag_ptr = LLVMBuildStructGEP2(c->builder, enum_t, match_slot, 0, "match_tag_ptr");
	LLVMValueRef tag = LLVMBuildLoad2(c->builder, LLVMInt64TypeInContext(c->context), tag_ptr, "match_tag");

	// Determine enum name
	const char *enum_name = NULL;
	if (target->data_type && target->data_type->kind == TYPE_ENUM && target->data_type->name) {
		enum_name = target->data_type->name;
	} else {
		for (ASTNode *a = n->data.match_stmt.arms; a; a = a->next) {
			if (!a->data.match_arm.is_else) {
				if (a->data.match_arm.enum_name) {
					enum_name = a->data.match_arm.enum_name;
					break;
				}
				for (ASTNode *s = c->program_root; s; s = s->next) {
					if (s->type == NODE_ENUM_DECL && s->data.enum_decl.name) {
						for (EnumVariant *ev = s->data.enum_decl.variants; ev; ev = ev->next) {
							if (strcmp(ev->name, a->data.match_arm.variant_name) == 0) {
								enum_name = s->data.enum_decl.name;
								break;
							}
						}
						if (enum_name) break;
					}
				}
				if (enum_name) break;
			}
		}
	}

	ASTNode *enum_decl = enum_name ? find_enum_decl(c, enum_name) : NULL;
	if (!enum_decl || LLVMGetTypeKind(enum_t) != LLVMStructTypeKind) {
		still_error(STILL_E_TYPE, n, "match requires a tagged enum value");
		exit(1);
	}
	int has_else = 0;
	for (ASTNode *a = n->data.match_stmt.arms; a; a = a->next) {
		if (a->data.match_arm.is_else) {
			if (has_else) { still_error(STILL_E_ARGS, a, "duplicate else arm"); exit(1); }
			has_else = 1;
			continue;
		}
		EnumVariant *variant = find_enum_variant(enum_decl, a->data.match_arm.variant_name);
		if (!variant || (a->data.match_arm.enum_name && strcmp(a->data.match_arm.enum_name, enum_name))) {
			still_error(STILL_E_TYPE, a, "variant `%s` is not in enum `%s`", a->data.match_arm.variant_name, enum_name);
			exit(1);
		}
		int bindings = 0;
		for (ASTNode *b = a->data.match_arm.bindings; b; b = b->next) ++bindings;
		if (bindings != variant->payload_count) {
			still_error(STILL_E_ARITY, a, "pattern `%s` expects %d bindings, got %d", variant->name, variant->payload_count, bindings);
			exit(1);
		}
		for (ASTNode *prev = n->data.match_stmt.arms; prev != a; prev = prev->next)
			if (!prev->data.match_arm.is_else && !strcmp(prev->data.match_arm.variant_name, variant->name)) {
				still_error(STILL_E_ARGS, a, "duplicate match arm `%s`", variant->name); exit(1);
			}
	}
	if (!has_else) {
		for (EnumVariant *variant = enum_decl->data.enum_decl.variants; variant; variant = variant->next) {
			int covered = 0;
			for (ASTNode *a = n->data.match_stmt.arms; a; a = a->next)
				if (!strcmp(a->data.match_arm.variant_name, variant->name)) covered = 1;
			if (!covered) {
				still_error(STILL_E_TYPE, n, "non-exhaustive match: missing `%s.%s`", enum_name, variant->name);
				exit(1);
			}
		}
	}

	int arm_count = 0;
	ASTNode *else_arm = NULL;
	for (ASTNode *a = n->data.match_stmt.arms; a; a = a->next) {
		if (a->data.match_arm.is_else)
			else_arm = a;
		else
			arm_count++;
	}

	LLVMBasicBlockRef exit_bb = wky_append_block(c->current_func, "match_exit");
	LLVMBasicBlockRef default_bb = NULL;
	LLVMBasicBlockRef else_bb = else_arm ? wky_append_block(c->current_func, "match_else") : NULL;

	LLVMBasicBlockRef trap_bb = NULL;
	if (!else_bb) {
		trap_bb = wky_append_block(c->current_func, "match_trap");
		default_bb = trap_bb;
	} else {
		default_bb = else_bb;
	}

	LLVMValueRef switch_inst = LLVMBuildSwitch(c->builder, tag, default_bb, arm_count);

	for (ASTNode *a = n->data.match_stmt.arms; a; a = a->next) {
		if (a->data.match_arm.is_else)
			continue;

		LLVMBasicBlockRef arm_bb = wky_append_block(c->current_func, "match_arm");
		EnumVariant *ev = enum_decl ? find_enum_variant(enum_decl, a->data.match_arm.variant_name) : NULL;
		if (!ev && a->data.match_arm.variant_name) {
			for (ASTNode *s = c->program_root; s; s = s->next) {
				if (s->type == NODE_ENUM_DECL) {
					for (EnumVariant *v = s->data.enum_decl.variants; v; v = v->next) {
						if (strcmp(v->name, a->data.match_arm.variant_name) == 0) {
							ev = v;
							if (!enum_decl) enum_decl = s;
							break;
						}
					}
					if (ev) break;
				}
			}
		}

		long long arm_tag = ev ? ev->tag : 0;
		LLVMAddCase(switch_inst, LLVMConstInt(LLVMInt64TypeInContext(c->context), arm_tag, 0), arm_bb);

		LLVMPositionBuilderAtEnd(c->builder, arm_bb);
		Scope *saved_scope = c->scope_stack;

		if (ev && ev->payload_count > 0 && a->data.match_arm.bindings) {
			LLVMTypeRef param_ts[16];
			for (int pi = 0; pi < ev->payload_count; pi++) {
				param_ts[pi] = get_llvm_type(c, ev->payload_types[pi]);
			}
			LLVMTypeRef payload_struct_t = LLVMStructTypeInContext(c->context, param_ts, ev->payload_count, 0);

			LLVMValueRef raw_payload = LLVMBuildStructGEP2(c->builder, enum_t, match_slot, 1, "payload_raw");
			LLVMValueRef typed_payload = LLVMBuildPointerCast(c->builder, raw_payload,
				LLVMPointerType(payload_struct_t, 0), "typed_payload");

			ASTNode *b = a->data.match_arm.bindings;
			for (int pi = 0; pi < ev->payload_count && b; pi++, b = b->next) {
				LLVMValueRef fld_ptr = LLVMBuildStructGEP2(c->builder, payload_struct_t, typed_payload, pi, b->data.var_decl.name);
				LLVMValueRef val = LLVMBuildLoad2(c->builder, param_ts[pi], fld_ptr, b->data.var_decl.name);
				LLVMValueRef b_slot = create_entry_block_alloca(c, param_ts[pi], b->data.var_decl.name);
				LLVMBuildStore(c->builder, val, b_slot);
				scope_push(c, b->data.var_decl.name, b_slot, param_ts[pi], b);
			}
		}

		if (res_slot != NULL) {
			LLVMValueRef arm_val = codegen_expr(c, a->data.match_arm.body);
			arm_val = coerce_value(c, arm_val, a->data.match_arm.body ? a->data.match_arm.body->data_type : NULL,
				res_type, a->data_type);
			LLVMBuildStore(c->builder, arm_val, res_slot);
		} else {
			codegen_stmt(c, a->data.match_arm.body);
		}

		c->scope_stack = saved_scope;
		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder))) {
			LLVMBuildBr(c->builder, exit_bb);
		}
	}

	if (else_arm) {
		LLVMPositionBuilderAtEnd(c->builder, else_bb);
		if (res_slot != NULL) {
			LLVMValueRef arm_val = codegen_expr(c, else_arm->data.match_arm.body);
			arm_val = coerce_value(c, arm_val, else_arm->data.match_arm.body ? else_arm->data.match_arm.body->data_type : NULL,
				res_type, else_arm->data_type);
			LLVMBuildStore(c->builder, arm_val, res_slot);
		} else {
			codegen_stmt(c, else_arm->data.match_arm.body);
		}
		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder))) {
			LLVMBuildBr(c->builder, exit_bb);
		}
	} else if (trap_bb) {
		LLVMPositionBuilderAtEnd(c->builder, trap_bb);
		LLVMValueRef trap_fn = LLVMGetNamedFunction(c->module, "llvm.trap");
		LLVMTypeRef trap_t = LLVMFunctionType(LLVMVoidTypeInContext(c->context), NULL, 0, 0);
		if (!trap_fn) {
			trap_fn = LLVMAddFunction(c->module, "llvm.trap", trap_t);
		}
		LLVMBuildCall2(c->builder, trap_t, trap_fn, NULL, 0, "");
		LLVMBuildUnreachable(c->builder);
	}

	LLVMPositionBuilderAtEnd(c->builder, exit_bb);
}
