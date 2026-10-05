#include "codegen_internal.h"

void codegen_func_decl(KawaCompiler *c, ASTNode *cur,
					   const char *implicit_self_struct) {
	Scope *caller_scope = c->scope_stack;
	Scope *caller_locals = c->function_locals;
	c->scope_stack = c->global_scope;
	c->function_locals = NULL;
	LLVMContextRef ctx = c->context;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
    kawa_check_value_type(c,cur,cur->data.func.ret_type);
    for (ASTNode *a=cur->data.func.args; a; a=a->next) {
        kawa_check_value_type(c,a,a->data_type);
    }

	// Explicit return type wins (`fn f64 accel(...)`, `fn void log(...)`).
	// No declared type keeps the legacy default: i32, or i8* for drips.
	LLVMTypeRef ret_t = LLVMInt32TypeInContext(ctx);
	if (cur->data.func.ret_type) {
		ret_t = get_llvm_type(c, cur->data.func.ret_type);
		if (LLVMGetTypeKind(ret_t) == LLVMVoidTypeKind && cur->data.func.is_drip)
			ret_t = LLVMInt32TypeInContext(ctx); // drip promise needs a slot
	}
	if (cur->data.func.is_drip)
		ret_t = i8ptr;

	int explicit_arg_cnt = 0;
	for (ASTNode *a = cur->data.func.args; a; a = a->next)
		explicit_arg_cnt++;

	// Methods declare their receiver explicitly as the first parameter
	// (Kawa style), so there is no hidden self argument: the declared
	// signature IS the ABI. implicit_self_struct only contributes the
	// `Struct__` mangling prefix.
	int total_arg_cnt = explicit_arg_cnt;
	kawa_verify_ownership(c,cur);

	LLVMTypeRef *param_types =
		arena_alloc(c->arena, sizeof(LLVMTypeRef) *
								  (total_arg_cnt > 0 ? total_arg_cnt : 1));

	int type_idx = 0;
	for (ASTNode *a = cur->data.func.args; a; a = a->next)
		param_types[type_idx++] = get_llvm_type(c, a->data_type);

	LLVMTypeRef func_t = LLVMFunctionType(ret_t, param_types, total_arg_cnt, 0);

	// A wrapper owns the C entry ABI, converts argv views when requested,
	// flushes output, and forwards the user main's integer result.
	const char *llvm_name = cur->data.func.name;
	int is_user_main = strcmp(llvm_name, "main") == 0 && !implicit_self_struct;
	if (is_user_main) {
		Type *a = cur->data.func.args ? cur->data.func.args->data_type : NULL;
		Type *b = explicit_arg_cnt == 2 ? cur->data.func.args->next->data_type : NULL;
		int argv_ok = b && b->kind == TYPE_PTR && b->inner &&
			((b->inner->kind == TYPE_PTR && b->inner->inner &&
			  (b->inner->inner->kind == TYPE_CHAR || b->inner->inner->kind == TYPE_U8)) ||
			 (b->inner->kind == TYPE_SLICE && b->inner->inner && b->inner->inner->kind == TYPE_U8));
		if ((explicit_arg_cnt != 0 && !(explicit_arg_cnt == 2 && a && a->kind == TYPE_I32 && argv_ok)) ||
			(LLVMGetTypeKind(ret_t) != LLVMIntegerTypeKind && LLVMGetTypeKind(ret_t) != LLVMVoidTypeKind) || cur->data.func.is_drip) {
			kerr(KAWA_E_TYPE, cur, "main expects no arguments or (i32, str*/char**), and returns an integer or void");
			exit(1);
		}
		llvm_name = "kawa_main";
		c->main_argv_views = argv_ok && b->inner->kind == TYPE_SLICE;
		c->main_ret_ast = cur->data.func.ret_type;
	}

	// Overloaded declarations emit under their mangled symbol so distinct
	// signatures coexist in the module. `main` never participates.
	int saved_overload_idx = c->overloads_active;
	c->overloads_active = -1;
	if (!is_user_main) {
		for (int oi = 0; oi < c->overload_fn_count; oi++) {
			if (c->overload_fns[oi].decl == cur &&
				strcmp(c->overload_fns[oi].mangled, llvm_name) != 0) {
				llvm_name = c->overload_fns[oi].mangled;
				c->overloads_active = oi;
				break;
			}
		}
	}

	c->current_func = LLVMAddFunction(c->module, llvm_name, func_t);
	// Module-private functions permit interprocedural range propagation and
	// specialization. Exported functions keep their independently callable ABI.
	if (!cur->is_pub && !is_user_main)
		LLVMSetLinkage(c->current_func,LLVMInternalLinkage);
	FunctionSignature *signature = arena_alloc(c->arena, sizeof(*signature));
	signature->function = c->current_func;
	signature->declaration = cur;
	signature->parameters=arena_alloc(c->arena,sizeof(Type*)*(total_arg_cnt ? total_arg_cnt : 1));
	int signature_index=0;
	for (ASTNode *a=cur->data.func.args; a; a=a->next)
		signature->parameters[signature_index++]=kawa_concrete_type(c,a->data_type);
	signature->return_type=kawa_concrete_type(c,cur->data.func.ret_type);
	signature->next = c->function_signatures;
	c->function_signatures = signature;
	char line_buf[32];
	snprintf(line_buf, sizeof(line_buf), "%d", cur->line);
	LLVMAddTargetDependentFunctionAttr(c->current_func, "kawa.source", line_buf);
	if (cur->data.func.is_pure)
		LLVMAddTargetDependentFunctionAttr(c->current_func, "kawa.pure", "true");
	if (cur->data.func.is_noalloc)
		LLVMAddTargetDependentFunctionAttr(c->current_func, "kawa.noalloc", "true");
	if (cur->data.func.is_nocapture)
		LLVMAddTargetDependentFunctionAttr(c->current_func,"kawa.nocapture","true");
	unsigned saved_fp_permissions = c->fp_permissions;
	c->fp_permissions = cur->data.func.fp_permissions;
	c->current_ret_type = ret_t;
	c->current_ret_node_type = cur->data.func.ret_type;

	// Debug info: attach a DISubprogram so stacks/profiles show real names.
	// fn_node line isn't tracked at decl granularity; use 1 (file scope).
	kawa_di_attach_subprogram(c, llvm_name, cur);

	// Optimization attributes: nounwind enables exception-free codegen and
	// better scheduling. LLVM infers memory effects; noinline keeps drips from
	// being inlined into their spawner before coro-split runs.
	unsigned nounwind_id = LLVMGetEnumAttributeKindForName("nounwind", 8);
	LLVMAddAttributeAtIndex(
		c->current_func, LLVMAttributeFunctionIndex,
		LLVMCreateEnumAttribute(c->context, nounwind_id, 0));
	// Index overloads are accessor-shaped by contract -- always inline so
	// `a[i]` costs exactly what the raw array access would. Other impl
	// methods get a hint; small bodies inline, big ones don't bloat.
	size_t ilen = cur->data.func.name ? strlen(cur->data.func.name) : 0;
	if (implicit_self_struct &&
		((ilen > 12 &&
		  strcmp(cur->data.func.name + ilen - 12, "__self_index") == 0) ||
		 (ilen > 16 &&
		  strcmp(cur->data.func.name + ilen - 16,
				 "__self_index_set") == 0))) {
		unsigned ai_id =
			LLVMGetEnumAttributeKindForName("alwaysinline", 12);
		LLVMAddAttributeAtIndex(
			c->current_func, LLVMAttributeFunctionIndex,
			LLVMCreateEnumAttribute(c->context, ai_id, 0));
	} else if (implicit_self_struct && !cur->data.func.is_drip) {
		unsigned ih_id =
			LLVMGetEnumAttributeKindForName("inlinehint", 10);
		LLVMAddAttributeAtIndex(
			c->current_func, LLVMAttributeFunctionIndex,
			LLVMCreateEnumAttribute(c->context, ih_id, 0));
	}
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

	// LLVM infers memory effects from verified bodies; a pure function may
	// still read through pointers, so it must not promise memory(none).

	LLVMBasicBlockRef entry = kawa_append_block(c->current_func, "entry");
	LLVMPositionBuilderAtEnd(c->builder, entry);

	// Clear any debug location left over from the previous function --
	// instructions emitted here (param spills) would otherwise reference
	// the previous function's subprogram and fail verification.
	LLVMSetCurrentDebugLocation2(c->builder, NULL);

	// Defer/filter stacks are per-function; save and clear before the body.
	DeferFrame *saved_defers = c->defer_stack;
	FilterFrame *saved_filters = c->filter_stack;
	StableFrame *saved_stable = c->stable_stack;
	c->defer_stack = NULL;
	c->filter_stack = NULL;
	c->stable_stack = NULL;
	c->warned_unreachable = 0;

	// Spill each parameter to an entry-block alloca so mem2reg can promote
	// it; parameters used exactly once never touch memory after O3.
	int arg_idx = 0;

	for (ASTNode *a = cur->data.func.args; a; a = a->next) {
		LLVMValueRef p_val = LLVMGetParam(c->current_func, arg_idx++);
		// Name the param so call sites can resolve named arguments
		// (`f(x: 1)`) by matching against parameter names.
		LLVMSetValueName2(p_val, a->data.var_decl.name,
						  strlen(a->data.var_decl.name));
		LLVMTypeRef arg_type = get_llvm_type(c, a->data_type);

		// Ordinary pointers may alias, including two arguments to one call.
		// Leave noalias/readonly inference to LLVM instead of asserting them.

		LLVMValueRef p_alloc =
			create_entry_block_alloca(c, arg_type, a->data.var_decl.name);
		LLVMBuildStore(c->builder, p_val, p_alloc);
		scope_push(c, a->data.var_decl.name, p_alloc, arg_type, a);
		if (kawa_contains_managed(c,a->data_type,1)) {
			if (cur->data.func.is_drip) {
				kerr(KAWA_E_TYPE, a, "managed owners in coroutines require cancellation cleanup support");
				exit(1);
			}
			kawa_memory_defer_value(c,p_alloc,a->data_type);
		}
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
		// finish_coro_body now terminates the shared exit itself (ret hdl
		// after coro.end); nothing left to emit on that block.

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
		// Falling off the end still runs deferred statements first.
		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder))) {
			for (DeferFrame *d = c->defer_stack; d; d = d->next)
				run_defer_frame(c, d);
			if (LLVMGetTypeKind(ret_t) == LLVMVoidTypeKind)
				LLVMBuildRetVoid(c->builder);
			else
				LLVMBuildRet(c->builder, LLVMConstNull(ret_t));
		}
	}

	// Unused-variable warnings: walk the scope entries this function pushed
	// (everything above the entry snapshot). Parameters count too, matching
	// the "declared but never read" contract. Underscore-prefixed names opt out.
	if (!cur->data.func.is_test) {
		for (Scope *sc = c->function_locals; sc; sc = sc->all_next) {
			if (sc->used || !sc->name || sc->name[0] == '_')
				continue;
			ASTNode *dn = sc->node;
			int pline = dn ? dn->line : 0;
			kdiag_warn_at(KAWA_W_UNUSED,
						  c->source_filename ? c->source_filename : "<kawa>",
						  NULL, pline > 0 ? pline : 0,
						  "variable `%s` is never used", sc->name);
		}
	}

	c->defer_stack = saved_defers;
	c->filter_stack = saved_filters;
	c->stable_stack = saved_stable;
	c->fp_permissions = saved_fp_permissions;
	c->overloads_active = saved_overload_idx;
	c->scope_stack = caller_scope;
	c->function_locals = caller_locals;
}
