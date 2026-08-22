#include "codegen_internal.h"

// Shared coroutine-frame prologue: emits llvm.coro.id / coro.alloc /
// coro.size / coro.begin and the suspend dispatch switch. Returns the
// coroutine handle and fills in the cleanup/suspend block out-params.
LLVMValueRef build_coro_frame(KawaCompiler *c, LLVMValueRef fn,
							  int promise_index, LLVMBasicBlockRef *cleanup_bb,
							  LLVMBasicBlockRef *suspend_bb) {
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
LLVMValueRef codegen_brew(KawaCompiler *c, ASTNode *n) {
	char task_name[64];
	snprintf(task_name, sizeof(task_name), "kawa_task_%d", c->lambda_counter++);

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
	LLVMTypeRef task_type = LLVMFunctionType(ret_type, NULL, 0, 0);
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
	LLVMValueRef hdl = build_coro_frame(c, task_func, c->brew_promise_index,
										&cleanup_bb, &suspend_bb);
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

	LLVMValueRef task_handle =
		LLVMBuildCall2(c->builder, task_type, task_func, NULL, 0, "task_hdl");

	LLVMMetadataRef md_str = LLVMMDStringInContext2(c->context, "brew", 4);
	LLVMMetadataRef md_args[] = {md_str};
	LLVMMetadataRef md = LLVMMDNodeInContext2(c->context, md_args, 1);
	unsigned KIND = LLVMGetMDKindID("kawa.coro.kind", strlen("kawa.coro.kind"));
	LLVMSetMetadata(task_handle, KIND, LLVMMetadataAsValue(c->context, md));
	return task_handle;
}
