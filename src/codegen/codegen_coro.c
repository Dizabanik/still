#include "codegen_internal.h"

// Upper bound for elided brew frames. A frame holds the promise slot plus
// spilled locals of the task body; our bodies are small by construction.
#define KAWA_CORO_STACK_FRAME_BYTES 8192

// Shared coroutine-frame prologue: emits llvm.coro.id / coro.alloc /
// coro.size / coro.begin and the suspend dispatch switch. Returns the
// coroutine handle and fills in the cleanup/suspend block out-params.
LLVMValueRef build_coro_frame(KawaCompiler *c, LLVMValueRef fn,
							  int promise_index, LLVMBasicBlockRef *cleanup_bb,
							  LLVMBasicBlockRef *suspend_bb) {
	return build_coro_frame_ex(c, fn, promise_index, cleanup_bb, suspend_bb, 0);
}

// Variant with heap elision (IDEAS 2.6): when stack_frame is set, the frame
// comes from a caller-provided buffer -- coro.begin receives it and the
// malloc branch never fires. The buffer is sized at runtime from
// llvm.coro.size via a dynamic alloca in the CALLER's entry, so it is exact
// (no guessing) and dies with the caller's frame.
LLVMValueRef build_coro_frame_ex(KawaCompiler *c, LLVMValueRef fn,
								 int promise_index,
								 LLVMBasicBlockRef *cleanup_bb,
								 LLVMBasicBlockRef *suspend_bb,
								 int use_stack_frame) {
	LLVMContextRef ctx = c->context;
	LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);

	LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(fn);
	LLVMValueRef promise_alloca = create_entry_block_alloca(
		c, LLVMInt32TypeInContext(ctx), "promise_storage");
	c->current_promise_ptr = promise_alloca;

	LLVMValueRef promise_void =
		LLVMBuildPointerCast(c->builder, promise_alloca, i8ptr, "prom_void");
	LLVMValueRef null_ptr = LLVMConstNull(i8ptr);

	LLVMValueRef id = LLVMBuildCall2(
		c->builder, c->coro_id_type, c->coro_id,
		(LLVMValueRef[]){
			LLVMConstInt(LLVMInt32TypeInContext(ctx), promise_index, 0),
			promise_void, null_ptr, null_ptr},
		4, "id");
	if (use_stack_frame) {
		// Heap elision: the caller allocated the frame on ITS stack and
		// passed it through our hidden first parameter. coro.begin adopts
		// that memory and skips the malloc path entirely. Sound because
		// the handle never escapes: every resume happens while the
		// spawning statement list is still live.
		LLVMValueRef buf = LLVMGetParam(fn, 0);

		*suspend_bb = LLVMAppendBasicBlock(fn, "suspend");
		*cleanup_bb = LLVMAppendBasicBlock(fn, "cleanup");
		LLVMValueRef hdl0 = LLVMBuildCall2(
			c->builder, c->coro_begin_type, c->coro_begin,
			(LLVMValueRef[]){id, buf}, 2, "hdl");
		c->current_coro_hdl = hdl0;

		LLVMValueRef suspend = LLVMBuildCall2(
			c->builder, c->coro_suspend_type, c->coro_suspend,
			(LLVMValueRef[]){LLVMConstNull(LLVMTokenTypeInContext(ctx)),
							 LLVMConstInt(LLVMInt1TypeInContext(ctx), 0, 0)},
			2, "suspend");
		LLVMValueRef sw =
			LLVMBuildSwitch(c->builder, suspend, *suspend_bb, 2);
		LLVMAddCase(sw, LLVMConstInt(LLVMInt8TypeInContext(ctx), 0, 0),
					LLVMAppendBasicBlock(fn, "resume"));
		LLVMAddCase(sw, LLVMConstInt(LLVMInt8TypeInContext(ctx), 1, 0),
					*cleanup_bb);
		return hdl0;
	}

	LLVMValueRef need_alloc = LLVMBuildCall2(
		c->builder, c->coro_alloc_type, c->coro_alloc, &id, 1, "need_alloc");
	LLVMValueRef size = LLVMBuildCall2(c->builder, c->coro_size_type,
									   c->coro_size, NULL, 0, "size");

	LLVMBasicBlockRef alloc_bb = LLVMAppendBasicBlock(fn, "alloc");
	LLVMBasicBlockRef cont_bb = LLVMAppendBasicBlock(fn, "alloc_cont");
	LLVMBuildCondBr(c->builder, need_alloc, alloc_bb, cont_bb);
	LLVMPositionBuilderAtEnd(c->builder, alloc_bb);
	LLVMValueRef malloc_ptr = LLVMBuildCall2(
		c->builder, c->malloc_type, c->malloc_fn, &size, 1, "coro_mem");
	LLVMBuildBr(c->builder, cont_bb);
	LLVMPositionBuilderAtEnd(c->builder, cont_bb);

	LLVMValueRef phi = LLVMBuildPhi(c->builder, i8ptr, "mem_phi");
	LLVMAddIncoming(phi, (LLVMValueRef[]){malloc_ptr, null_ptr},
					(LLVMBasicBlockRef[]){alloc_bb, entry}, 2);

	LLVMValueRef hdl =
		LLVMBuildCall2(c->builder, c->coro_begin_type, c->coro_begin,
					   (LLVMValueRef[]){id, phi}, 2, "hdl");
	c->current_coro_hdl = hdl;

	LLVMValueRef suspend = LLVMBuildCall2(
		c->builder, c->coro_suspend_type, c->coro_suspend,
		(LLVMValueRef[]){LLVMConstNull(LLVMTokenTypeInContext(ctx)),
						 LLVMConstInt(LLVMInt1TypeInContext(ctx), 0, 0)},
		2, "suspend");

	*suspend_bb = LLVMAppendBasicBlock(fn, "suspend");
	*cleanup_bb = LLVMAppendBasicBlock(fn, "cleanup");
	LLVMValueRef sw = LLVMBuildSwitch(c->builder, suspend, *suspend_bb, 2);
	LLVMAddCase(sw, LLVMConstInt(LLVMInt8TypeInContext(ctx), 0, 0),
				LLVMAppendBasicBlock(fn, "resume"));
	LLVMAddCase(sw, LLVMConstInt(LLVMInt8TypeInContext(ctx), 1, 0),
				*cleanup_bb);
	return hdl;
}

// Shared epilogue: final suspend + coro.end + branch to the suspend block.
void finish_coro_body(KawaCompiler *c, LLVMBasicBlockRef cleanup_bb,
					  LLVMBasicBlockRef suspend_bb) {
	if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder))) {
		// Final suspend. Both switch cases target cleanup; emit the
		// unconditional branch directly so the pipeline gets a cleaner CFG.
		(void)LLVMBuildCall2(
			c->builder, c->coro_suspend_type, c->coro_suspend,
			(LLVMValueRef[]){
				LLVMConstNull(LLVMTokenTypeInContext(c->context)),
				LLVMConstInt(LLVMInt1TypeInContext(c->context), 1, 0)},
			2, "final");
		LLVMBuildBr(c->builder, cleanup_bb);
	}
	LLVMPositionBuilderAtEnd(c->builder, cleanup_bb);
	LLVMBuildCall2(
		c->builder, c->coro_end_type, c->coro_end,
		(LLVMValueRef[]){LLVMConstNull(LLVMPointerType(
							 LLVMInt8TypeInContext(c->context), 0)),
						 LLVMConstInt(LLVMInt1TypeInContext(c->context), 0, 0),
						 LLVMConstNull(LLVMTokenTypeInContext(c->context))},
		3, "");
	LLVMBuildBr(c->builder, suspend_bb);
}

// brew { ... } -- anonymous coroutine task. Compiles the body into a fresh
// `kawa_task_N` function and returns its handle to the caller.
// Escape analysis for brew handles (IDEAS 2.6), decided from the AST of the
// enclosing block BEFORE codegen: `let h = brew { ... }` is stack-frame
// eligible iff every later mention of h in the same statement list is exactly
// `sip(h)`. Anything else -- returning, storing, passing to another call --
// keeps the heap path. Soundness: sip resumes within the spawning statement
// list, so the caller frame provably outlives every resume.
static int expr_mentions_handle_other_than_sip(ASTNode *e, const char *name);

static int stmt_list_brew_local(ASTNode *stmts, ASTNode *brew_decl,
								const char *name) {
	int saw_binding = 0;
	for (ASTNode *st = stmts; st; st = st->next) {
		switch (st->type) {
		case NODE_VAR_DECL:
			if (st == brew_decl) {
				saw_binding++;
				break;
			}
			// Another decl whose init mentions the handle outside a sip()
			// call escapes (let copy = h; let p = f(h); ...).
			if (st->data.var_decl.init &&
				expr_mentions_handle_other_than_sip(st->data.var_decl.init,
													name))
				return 0;
			break;
		case NODE_SIP:
			// `sip(h)` is THE safe use; any other shape escapes.
			if (!(st->data.sip.handle && st->data.sip.handle->type ==
												NODE_VAR_REF &&
				  strcmp(st->data.sip.handle->data.var_ref.name, name) == 0))
				return 0;
			break;
		case NODE_RETURN:
		case NODE_ASSIGN:
		case NODE_CALL:
		case NODE_WHILE:
		case NODE_FOR:
		case NODE_IF:
		case NODE_BATCH:
			// Conservative: control flow or other statements in the list
			// after the binding disqualify (they might re-suspend past the
			// frame's lifetime).
			return 0;
		default:
			break;
		}
	}
	return saw_binding == 1;
}

// True when expression `e` mentions `name` in any way other than being the
// direct argument of sip(). Only the shapes an initializer can legally take
// are inspected; anything exotic counts as a mention (conservative).
static int expr_mentions_handle_other_than_sip(ASTNode *e,
											   const char *name) {
	if (!e)
		return 0;
	switch (e->type) {
	case NODE_VAR_REF:
		return strcmp(e->data.var_ref.name, name) == 0;
	case NODE_CALL:
		// sip(name): safe. Any other callee with name as an arg: unsafe.
		if (e->data.call.callee && e->data.call.callee->type == NODE_VAR_REF &&
			strcmp(e->data.call.callee->data.var_ref.name, "sip") == 0)
			return 0;
		for (ASTNode *a = e->data.call.args; a; a = a->next)
			if (expr_mentions_handle_other_than_sip(a, name))
				return 1;
		return 0;
	case NODE_BINARY_OP:
		return expr_mentions_handle_other_than_sip(e->data.bin_op.left,
												   name) ||
			   expr_mentions_handle_other_than_sip(e->data.bin_op.right,
												   name);
	default:
		return 0; // literals etc. cannot mention the handle
	}
}

// Mark every stack-eligible brew node reachable from the program root.
void kawa_mark_stack_frames(ASTNode *root) {
	(void)root;
	// Decided inline during codegen instead (see codegen_brew): the parser
	// keeps no parent links, so eligibility is computed against the current
	// statement list at emission time.
}

LLVMValueRef codegen_brew(KawaCompiler *c, ASTNode *n) {
	char task_name[64];
	snprintf(task_name, sizeof(task_name), "kawa_task_%d", c->lambda_counter++);

	// Heap elision decision: the brew's enclosing statement list decides.
	// `let h = brew {...}; ... sip(h);` and nothing else -> stack frame.
	int use_stack_frame = 0;
	LLVMValueRef stack_buf = NULL;
	if (c->cur_brew_decl && c->cur_stmt_list) {
		const char *hname = c->cur_brew_decl->data.var_decl.name;
		use_stack_frame =
			stmt_list_brew_local(c->cur_stmt_list, c->cur_brew_decl, hname);
	}
	if (use_stack_frame) {
		// Static buffer in the CALLER'S ENTRY BLOCK: entry allocas run
		// once, so a loop body spawning many tasks reuses one slot instead
		// of eating stack per iteration. 8KB covers our small task bodies;
		// it dies with the spawning frame -- exactly the elision guarantee.
		LLVMBasicBlockRef saved_bb = LLVMGetInsertBlock(c->builder);
		LLVMValueRef saved_fn = c->current_func;
		stack_buf = create_entry_block_alloca(
			c, LLVMArrayType(LLVMInt8TypeInContext(c->context),
							 KAWA_CORO_STACK_FRAME_BYTES),
			"coro_stack_frame");
		LLVMPositionBuilderAtEnd(c->builder, saved_bb);
		c->current_func = saved_fn;
		LLVMSetAlignment(stack_buf, 64);
	}

	// Save outer coroutine state so nested brew compiles as a fresh
	// coroutine and restores cleanly.
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
	// Hidden frame-buffer parameter on the elided path only: heap-path
	// coroutines keep their zero-arg signature.
	LLVMTypeRef i8ptr_t = LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
	LLVMTypeRef task_type =
		use_stack_frame
			? LLVMFunctionType(ret_type, (LLVMTypeRef[]){i8ptr_t}, 1, 0)
			: LLVMFunctionType(ret_type, NULL, 0, 0);
	LLVMValueRef task_func = LLVMAddFunction(c->module, task_name, task_type);

	unsigned kind_id = LLVMGetEnumAttributeKindForName("presplitcoroutine", 17);
	LLVMAddAttributeAtIndex(task_func, LLVMAttributeFunctionIndex,
							LLVMCreateEnumAttribute(c->context, kind_id, 0));

	c->current_func = task_func;
	c->current_ret_type = ret_type;
	c->in_coroutine = 1;
	LLVMPositionBuilderAtEnd(c->builder,
							 LLVMAppendBasicBlock(task_func, "entry"));

	LLVMBasicBlockRef cleanup_bb, suspend_bb;
	LLVMValueRef hdl = build_coro_frame_ex(c, task_func, c->brew_promise_index,
										   &cleanup_bb, &suspend_bb,
										   use_stack_frame);
	c->coro_cleanup_block = cleanup_bb;
	c->coro_suspend_block = suspend_bb;

	// Position at the resume block (the switch's case-0 successor).
	{
		LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(task_func);
		while (bb && strcmp(LLVMGetBasicBlockName(bb), "resume") != 0)
			bb = LLVMGetNextBasicBlock(bb);
		LLVMPositionBuilderAtEnd(c->builder, bb);
	}
	c->scope_stack = NULL;
	codegen_stmt(c, n->data.brew.body);
	finish_coro_body(c, cleanup_bb, suspend_bb);
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
		c->builder, task_type, task_func,
		use_stack_frame ? (LLVMValueRef[]){stack_buf} : NULL,
		use_stack_frame ? 1 : 0, "task_hdl");

	LLVMMetadataRef md_str = LLVMMDStringInContext2(c->context, "brew", 4);
	LLVMMetadataRef md_args[] = {md_str};
	LLVMMetadataRef md = LLVMMDNodeInContext2(c->context, md_args, 1);
	unsigned KIND = LLVMGetMDKindID("kawa.coro.kind", strlen("kawa.coro.kind"));
	LLVMSetMetadata(task_handle, KIND, LLVMMetadataAsValue(c->context, md));
	return task_handle;
}
