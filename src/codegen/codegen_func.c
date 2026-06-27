#include "codegen_internal.h"

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
