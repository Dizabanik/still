#include "codegen_internal.h"

/* Every suspension has its own lexical cleanup edge. Only defers and owners
 * that have actually been initialized before this suspension are destroyed.
 * No dynamic cleanup stack, allocation, or branch flags are needed. */
LLVMBasicBlockRef wky_coro_cancel_block(StillCompiler *c) {
    LLVMBasicBlockRef saved=LLVMGetInsertBlock(c->builder);
    LLVMBasicBlockRef cancel=wky_append_block(c->current_func,"cancel_cleanup");
    LLVMPositionBuilderAtEnd(c->builder,cancel);
    for (DeferFrame *d=c->defer_stack; d; d=d->next) run_defer_frame(c,d);
    LLVMBuildBr(c->builder,c->coro_cleanup_block);
    LLVMPositionBuilderAtEnd(c->builder,saved);
    return cancel;
}

static LLVMValueRef resource_call(StillCompiler *c, const char *name, LLVMTypeRef result, LLVMValueRef *args, unsigned count) {
    c->uses_memory=1;
    LLVMValueRef fn=LLVMGetNamedFunction(c->module,name);
    if (!fn) {
        LLVMTypeRef types[3]; for (unsigned i=0; i<count; ++i) types[i]=LLVMTypeOf(args[i]);
        fn=LLVMAddFunction(c->module,name,LLVMFunctionType(result,types,count,0));
    }
    return LLVMBuildCall2(c->builder,LLVMGlobalGetValueType(fn),fn,args,count,LLVMGetTypeKind(result)==LLVMVoidTypeKind ? "" : "resource");
}
void wky_coro_drop(StillCompiler *c, LLVMValueRef slot) {
    Type handle={.kind=TYPE_HANDLE};
    wky_mark_cleanup_effect(c,resource_call(c,"__wky_mem_drop",LLVMVoidTypeInContext(c->context),&slot,1),&handle);
}
LLVMValueRef wky_coro_handle(StillCompiler *c, LLVMValueRef raw) {
    LLVMValueRef destroy=LLVMGetNamedFunction(c->module,"__wky_coro_destroy");
    if (!destroy) {
        LLVMBasicBlockRef saved=LLVMGetInsertBlock(c->builder);
        LLVMMetadataRef location=LLVMGetCurrentDebugLocation2(c->builder);
        LLVMTypeRef pointer=LLVMPointerTypeInContext(c->context,0);
        destroy=LLVMAddFunction(c->module,"__wky_coro_destroy",LLVMFunctionType(LLVMVoidTypeInContext(c->context),&pointer,1,0));
        LLVMSetLinkage(destroy,LLVMInternalLinkage);
        LLVMPositionBuilderAtEnd(c->builder,wky_append_block(destroy,"entry"));
        LLVMSetCurrentDebugLocation2(c->builder,NULL);
        LLVMValueRef handle=LLVMGetParam(destroy,0);
        LLVMBuildCall2(c->builder,c->coro_destroy_type,c->coro_destroy,&handle,1,"");
        LLVMBuildRetVoid(c->builder);
        LLVMPositionBuilderAtEnd(c->builder,saved); LLVMSetCurrentDebugLocation2(c->builder,location);
    }
    Type handle={.kind=TYPE_HANDLE}; LLVMTypeRef type=get_llvm_type(c,&handle);
    LLVMValueRef out=create_entry_block_alloca(c,type,"task_identity");
    LLVMValueRef args[]={out,raw,destroy};
    LLVMValueRef ok=resource_call(c,"__wky_mem_adopt",LLVMInt32TypeInContext(c->context),args,3);
    emit_check_or_trap(c,NULL,cond_to_bool(c,ok),"coroutine identity allocation failed");
    return LLVMBuildLoad2(c->builder,type,out,"task_handle");
}
LLVMValueRef wky_coro_sip(StillCompiler *c, ASTNode *node) {
    ASTNode *handle=node->data.sip.handle;
    Type *type=wky_expr_type(c,handle);
    if (!type || type->kind!=TYPE_HANDLE) { still_error(STILL_E_TYPE,node,"sip requires a live coroutine handle"); exit(1); }
    int lvalue=handle->type==NODE_VAR_REF || handle->type==NODE_MEMBER_ACCESS || handle->type==NODE_INDEX || handle->type==NODE_DEREF;
    LLVMValueRef value=lvalue ? wky_memory_value(c,handle) : codegen_expr(c,handle);
    LLVMValueRef slot=create_entry_block_alloca(c,LLVMTypeOf(value),"sip_identity");
    LLVMBuildStore(c->builder,value,slot);
    /* Pin the entire resume call. Source cleanup cannot cancel an executing
     * frame through another owning slot, including one in a channel. */
    LLVMValueRef raw=resource_call(c,"__wky_mem_pin",LLVMPointerTypeInContext(c->context,0),&slot,1);
    LLVMValueRef done=LLVMBuildCall2(c->builder,c->coro_done_type,c->coro_done,&raw,1,"task_done");
    LLVMBasicBlockRef resume=wky_append_block(c->current_func,"sip_resume"), end=wky_append_block(c->current_func,"sip_done");
    LLVMBuildCondBr(c->builder,done,end,resume); LLVMPositionBuilderAtEnd(c->builder,resume);
    LLVMBuildCall2(c->builder,c->coro_resume_type,c->coro_resume,&raw,1,"");
    LLVMBuildBr(c->builder,end); LLVMPositionBuilderAtEnd(c->builder,end);
    LLVMValueRef args[]={raw,LLVMConstInt(LLVMInt32TypeInContext(c->context),c->drip_promise_index,0),LLVMConstNull(LLVMInt1TypeInContext(c->context))};
    LLVMValueRef promise=LLVMBuildCall2(c->builder,c->coro_promise_type,c->coro_promise,args,3,"task_promise");
    LLVMValueRef result=LLVMBuildLoad2(c->builder,LLVMInt32TypeInContext(c->context),promise,"sip_value");
    resource_call(c,"__wky_mem_unpin",LLVMVoidTypeInContext(c->context),&slot,1);
    if (!lvalue) wky_coro_drop(c,slot);
    node->data_type=arena_alloc(c->arena,sizeof(Type)); node->data_type->kind=TYPE_I32;
    return result;
}

// Shared coroutine-frame prologue: emits llvm.coro.id / coro.alloc /
// coro.size / coro.begin and the suspend dispatch switch. Returns the
// coroutine handle and fills in the cleanup/suspend block out-params.
LLVMValueRef build_coro_frame(StillCompiler *c, LLVMValueRef fn,
                               int promise_index, LLVMBasicBlockRef *cleanup_bb,
                               LLVMBasicBlockRef *suspend_bb) {
	LLVMContextRef ctx = c->context;
	LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);

	LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(fn);
	LLVMValueRef promise_alloca = create_entry_block_alloca(
		c, LLVMInt32TypeInContext(ctx), "promise_storage");
	LLVMBuildStore(c->builder,LLVMConstNull(LLVMInt32TypeInContext(ctx)),promise_alloca);
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
	c->current_coro_id=id;
	LLVMValueRef need_alloc = LLVMBuildCall2(
		c->builder, c->coro_alloc_type, c->coro_alloc, &id, 1, "need_alloc");
	LLVMValueRef size = LLVMBuildCall2(c->builder, c->coro_size_type,
									   c->coro_size, NULL, 0, "size");

	LLVMBasicBlockRef alloc_bb = wky_append_block(fn, "alloc");
	LLVMBasicBlockRef cont_bb = wky_append_block(fn, "alloc_cont");
	LLVMBuildCondBr(c->builder, need_alloc, alloc_bb, cont_bb);
	LLVMPositionBuilderAtEnd(c->builder, alloc_bb);
	LLVMValueRef malloc_ptr = LLVMBuildCall2(
		c->builder, c->malloc_type, c->malloc_fn, &size, 1, "coro_mem");
	emit_check_or_trap(c,NULL,LLVMBuildIsNotNull(c->builder,malloc_ptr,"coro_allocated"),"coroutine allocation failed");
	LLVMBasicBlockRef allocated_bb=LLVMGetInsertBlock(c->builder);
	LLVMBuildBr(c->builder, cont_bb);
	LLVMPositionBuilderAtEnd(c->builder, cont_bb);

	LLVMValueRef phi = LLVMBuildPhi(c->builder, i8ptr, "mem_phi");
	LLVMAddIncoming(phi, (LLVMValueRef[]){malloc_ptr, null_ptr},
					(LLVMBasicBlockRef[]){allocated_bb, entry}, 2);

	LLVMValueRef hdl =
		LLVMBuildCall2(c->builder, c->coro_begin_type, c->coro_begin,
					   (LLVMValueRef[]){id, phi}, 2, "hdl");
	c->current_coro_hdl = hdl;

	LLVMValueRef suspend = LLVMBuildCall2(
		c->builder, c->coro_suspend_type, c->coro_suspend,
		(LLVMValueRef[]){LLVMConstNull(LLVMTokenTypeInContext(ctx)),
						 LLVMConstInt(LLVMInt1TypeInContext(ctx), 0, 0)},
		2, "suspend");

	// Same clang contract as the elided path: the shared exit block holds
	// coro.end(false); case 1 (destroy) funnels through it too.
	*suspend_bb = wky_append_block(fn, "suspend");
	*cleanup_bb = wky_append_block(fn, "cleanup");
	c->coro_cleanup_block=*cleanup_bb;
	LLVMValueRef sw = LLVMBuildSwitch(c->builder, suspend, *suspend_bb, 2);
	LLVMAddCase(sw, LLVMConstInt(LLVMInt8TypeInContext(ctx), 0, 0),
				wky_append_block(fn, "resume"));
	LLVMAddCase(sw, LLVMConstInt(LLVMInt8TypeInContext(ctx), 1, 0),
				wky_coro_cancel_block(c));
	return hdl;
}

// Shared epilogue: final suspend, then the coro.end-bearing exit block.
// Clang contract (verified against -Xclang -disable-llvm-passes output):
// the final suspend's case-0 arm and every mid-body switch's default arm
// converge on ONE block that calls llvm.coro.end(false) -- coro-split
// rewrites exactly those blocks per-funclet. `cleanup_bb` (case 1 /
// destroy path) funnels through the same exit.
void finish_coro_body(StillCompiler *c, LLVMBasicBlockRef cleanup_bb,
					  LLVMBasicBlockRef suspend_bb) {
	if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder))) {
		// Final suspend: case 1 -> shared exit; case 0 is impossible
		// (nobody resumes a finished frame) but clang still routes it to
		// the exit, so we do too.
		LLVMValueRef fin = LLVMBuildCall2(
			c->builder, c->coro_suspend_type, c->coro_suspend,
			(LLVMValueRef[]){
				LLVMConstNull(LLVMTokenTypeInContext(c->context)),
				LLVMConstInt(LLVMInt1TypeInContext(c->context), 1, 0)},
			2, "final");
		LLVMValueRef fsw =
			LLVMBuildSwitch(c->builder, fin, suspend_bb, 2);
		LLVMAddCase(fsw,
					LLVMConstInt(LLVMInt8TypeInContext(c->context), 0, 0),
					suspend_bb);
		LLVMAddCase(fsw,
					LLVMConstInt(LLVMInt8TypeInContext(c->context), 1, 0),
					cleanup_bb);
	} else if (LLVMGetBasicBlockParent(LLVMGetInsertBlock(c->builder)) ==
			   NULL) {
		return;
	}
	LLVMPositionBuilderAtEnd(c->builder, cleanup_bb);
	LLVMValueRef memory=LLVMBuildCall2(c->builder,c->coro_free_type,c->coro_free,
	    (LLVMValueRef[]){c->current_coro_id,c->current_coro_hdl},2,"coro_allocation");
	// A stack-elided frame returns null here; free(null) is valid.
	LLVMBuildCall2(c->builder,c->free_type,c->free_fn,&memory,1,"");
	LLVMBuildBr(c->builder, suspend_bb);

	LLVMPositionBuilderAtEnd(c->builder, suspend_bb);
	LLVMBuildCall2(
		c->builder, c->coro_end_type, c->coro_end,
		(LLVMValueRef[]){LLVMConstNull(LLVMPointerType(
							 LLVMInt8TypeInContext(c->context), 0)),
						 LLVMConstInt(LLVMInt1TypeInContext(c->context), 0, 0),
						 LLVMConstNull(LLVMTokenTypeInContext(c->context))},
		3, "");
	// The original function returns the handle from here; in .resume/
	// .destroy funclets coro-split rewrites this very instruction.
	if (LLVMGetTypeKind(LLVMGetReturnType(LLVMGlobalGetValueType(
			c->current_func))) == LLVMVoidTypeKind)
		LLVMBuildRetVoid(c->builder);
	else
		LLVMBuildRet(c->builder, c->current_coro_hdl);
}

// brew { ... } -- anonymous coroutine task. Compiles the body into a fresh
// `wky_task_N` function and returns its handle to the caller.
LLVMValueRef codegen_brew(StillCompiler *c, ASTNode *n) {
	char task_name[64];
	snprintf(task_name, sizeof(task_name), "wky_task_%d", c->lambda_counter++);

	// LLVM decides whether the exact frame can live in its caller's stack.
	// Never guess a fixed frame capacity before coroutine splitting.

	// Save outer coroutine state so nested brew compiles as a fresh
	// coroutine and restores cleanly.
	LLVMBasicBlockRef old_block = LLVMGetInsertBlock(c->builder);
	LLVMValueRef old_func = c->current_func;
	LLVMTypeRef old_ret_type = c->current_ret_type;
	Scope *old_scope = c->scope_stack;
	int was_in_coroutine = c->in_coroutine;
	LLVMBasicBlockRef old_cleanup = c->coro_cleanup_block;
	LLVMBasicBlockRef old_suspend = c->coro_suspend_block;
	LLVMBasicBlockRef old_finish=c->coro_finish_block;
	LLVMValueRef old_hdl = c->current_coro_hdl;
	LLVMValueRef old_id=c->current_coro_id;
	LLVMValueRef old_prom = c->current_promise_ptr;
	Type *old_ret_ast=c->current_ret_node_type;
	DeferFrame *old_defers=c->defer_stack;
	FilterFrame *old_filters=c->filter_stack;
	StableFrame *old_stable=c->stable_stack;
	Scope *old_locals=c->function_locals;
	struct LoopTargets *old_loop=c->loop_stack;

	LLVMTypeRef ret_type =
		LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
	LLVMTypeRef task_type=LLVMFunctionType(ret_type,NULL,0,0);
	LLVMValueRef task_func = LLVMAddFunction(c->module, task_name, task_type);
	LLVMSetLinkage(task_func,LLVMInternalLinkage);

	unsigned kind_id = LLVMGetEnumAttributeKindForName("presplitcoroutine", 17);
	LLVMAddAttributeAtIndex(task_func, LLVMAttributeFunctionIndex,
							LLVMCreateEnumAttribute(c->context, kind_id, 0));

	c->current_func = task_func;
	c->current_ret_type = ret_type;
	c->current_ret_node_type=NULL;
	c->in_coroutine = 1;
	c->defer_stack=NULL; c->filter_stack=NULL; c->stable_stack=NULL;
	c->function_locals=NULL; c->loop_stack=NULL;
	LLVMPositionBuilderAtEnd(c->builder,
							 wky_append_block(task_func, "entry"));

	LLVMBasicBlockRef cleanup_bb, suspend_bb;
	build_coro_frame(c,task_func,c->brew_promise_index,&cleanup_bb,&suspend_bb);
	c->coro_cleanup_block = cleanup_bb;
	c->coro_suspend_block = suspend_bb;
	c->coro_finish_block=wky_append_block(task_func,"finish");

	// Position at the resume block (the switch's case-0 successor).
	{
		LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(task_func);
		while (bb && strcmp(LLVMGetBasicBlockName(bb), "resume") != 0)
			bb = LLVMGetNextBasicBlock(bb);
		LLVMPositionBuilderAtEnd(c->builder, bb);
	}
	// Globals stay visible inside the task; locals of the spawner do not
	// (their allocas belong to another function).
	c->scope_stack = c->global_scope;
	ASTNode task_decl={.type=NODE_FUNC_DECL}; task_decl.data.func.body=n->data.brew.body;
	wky_verify_semantics(c,&task_decl);
	wky_verify_ownership(c,&task_decl);
	codegen_stmt(c, n->data.brew.body);
	if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder))) LLVMBuildBr(c->builder,c->coro_finish_block);
	LLVMPositionBuilderAtEnd(c->builder,c->coro_finish_block);
	finish_coro_body(c, cleanup_bb, suspend_bb);

	LLVMPositionBuilderAtEnd(c->builder, old_block);
	c->current_func = old_func;
	c->current_ret_type = old_ret_type;
	c->scope_stack = old_scope;
	c->in_coroutine = was_in_coroutine;
	c->coro_cleanup_block = old_cleanup;
	c->coro_suspend_block = old_suspend;
	c->coro_finish_block=old_finish;
	c->current_coro_hdl = old_hdl;
	c->current_coro_id=old_id;
	c->current_promise_ptr = old_prom;
	c->current_ret_node_type=old_ret_ast;
	c->defer_stack=old_defers; c->filter_stack=old_filters; c->stable_stack=old_stable;
	c->function_locals=old_locals; c->loop_stack=old_loop;

	LLVMValueRef task_handle = LLVMBuildCall2(
		c->builder, task_type, task_func,
		NULL,0,"task_hdl");

	LLVMMetadataRef md_str = LLVMMDStringInContext2(c->context, "brew", 4);
	LLVMMetadataRef md_args[] = {md_str};
	LLVMMetadataRef md = LLVMMDNodeInContext2(c->context, md_args, 1);
	unsigned KIND = LLVMGetMDKindID("wky.coro.kind", strlen("wky.coro.kind"));
	LLVMSetMetadata(task_handle, KIND, LLVMMetadataAsValue(c->context, md));
	return wky_coro_handle(c,task_handle);
}
