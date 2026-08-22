#include "codegen_internal.h"

void codegen_func_decl(KawaCompiler *c, ASTNode *cur,
					   const char *implicit_self_struct) {
	LLVMContextRef ctx = c->context;
	LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);

	LLVMTypeRef ret_t = LLVMInt32TypeInContext(ctx);
	if (cur->data.func.is_drip)
		ret_t = i8ptr;

	int explicit_arg_cnt = 0;
	for (ASTNode *a = cur->data.func.args; a; a = a->next)
		explicit_arg_cnt++;

	int total_arg_cnt = explicit_arg_cnt + (implicit_self_struct ? 1 : 0);

	LLVMTypeRef *param_types =
		arena_alloc(c->arena, sizeof(LLVMTypeRef) *
								  (total_arg_cnt > 0 ? total_arg_cnt : 1));

	int type_idx = 0;
	if (implicit_self_struct)
		param_types[type_idx++] = i8ptr;

	for (ASTNode *a = cur->data.func.args; a; a = a->next)
		param_types[type_idx++] = get_llvm_type(c, a->data_type);

	LLVMTypeRef func_t = LLVMFunctionType(ret_t, param_types, total_arg_cnt, 0);

	c->current_func = LLVMAddFunction(c->module, cur->data.func.name, func_t);
	c->current_ret_type = ret_t;

	// Optimization attributes: nounwind enables exception-free codegen and
	// better scheduling; willreturn + memory(none) on pure functions lets
	// the optimizer hoist/delete calls. noinline keeps drip coroutines from
	// being inlined into their spawner before coro-split runs.
	unsigned nounwind_id = LLVMGetEnumAttributeKindForName("nounwind", 8);
	LLVMAddAttributeAtIndex(
		c->current_func, LLVMAttributeFunctionIndex,
		LLVMCreateEnumAttribute(c->context, nounwind_id, 0));
	if (cur->data.func.is_drip) {
		unsigned noinline_id = LLVMGetEnumAttributeKindForName("noinline", 8);
		LLVMAddAttributeAtIndex(
			c->current_func, LLVMAttributeFunctionIndex,
			LLVMCreateEnumAttribute(c->context, noinline_id, 0));
		unsigned presplit_id =
			LLVMGetEnumAttributeKindForName("presplitcoroutine", 17);
		LLVMAddAttributeAtIndex(
			c->current_func, LLVMAttributeFunctionIndex,
			LLVMCreateEnumAttribute(c->context, presplit_id, 0));
	}

	LLVMBasicBlockRef entry = LLVMAppendBasicBlock(c->current_func, "entry");
	LLVMPositionBuilderAtEnd(c->builder, entry);

	// Spill each parameter to an entry-block alloca so mem2reg can promote
	// it; parameters used exactly once never touch memory after O3.
	int arg_idx = 0;

	if (implicit_self_struct)
		arg_idx++; // self pointer stays in its LLVM parameter slot

	for (ASTNode *a = cur->data.func.args; a; a = a->next) {
		LLVMValueRef p_val = LLVMGetParam(c->current_func, arg_idx++);
		LLVMTypeRef arg_type = get_llvm_type(c, a->data_type);
		LLVMValueRef p_alloc =
			create_entry_block_alloca(c, arg_type, a->data.var_decl.name);
		LLVMBuildStore(c->builder, p_val, p_alloc);
		scope_push(c, a->data.var_decl.name, p_alloc, arg_type, a);
	}

	if (cur->data.func.is_drip) {
		int was_in_coroutine = c->in_coroutine;
		c->in_coroutine = 1;

		LLVMBasicBlockRef cleanup_bb, suspend_bb;
		LLVMValueRef hdl =
			build_coro_frame(c, c->current_func, c->drip_promise_index,
							 &cleanup_bb, &suspend_bb);
		LLVMBasicBlockRef old_cleanup = c->coro_cleanup_block;
		LLVMBasicBlockRef old_suspend = c->coro_suspend_block;
		c->coro_cleanup_block = cleanup_bb;
		c->coro_suspend_block = suspend_bb;

		// Position at the resume block (the switch's case-0 successor).
		LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(c->current_func);
		while (bb && strcmp(LLVMGetBasicBlockName(bb), "resume") != 0)
			bb = LLVMGetNextBasicBlock(bb);
		LLVMPositionBuilderAtEnd(c->builder, bb);

		codegen_stmt(c, cur->data.func.body);
		finish_coro_body(c, cleanup_bb, suspend_bb);
		LLVMPositionBuilderAtEnd(c->builder, suspend_bb);

		LLVMMetadataRef md_str = LLVMMDStringInContext2(ctx, "drip", 4);
		LLVMMetadataRef md_args[] = {md_str};
		LLVMMetadataRef md = LLVMMDNodeInContext2(ctx, md_args, 1);
		unsigned KIND =
			LLVMGetMDKindID("kawa.coro.kind", strlen("kawa.coro.kind"));
		LLVMSetMetadata(hdl, KIND, LLVMMetadataAsValue(ctx, md));
		LLVMBuildRet(c->builder, hdl);

		c->in_coroutine = was_in_coroutine;
		c->coro_cleanup_block = old_cleanup;
		c->coro_suspend_block = old_suspend;
		c->current_promise_ptr = NULL;
	} else {
		codegen_stmt(c, cur->data.func.body);
		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder)))
			LLVMBuildRet(c->builder, LLVMConstNull(ret_t));
	}
}
