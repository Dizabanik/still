#include "codegen_internal.h"

void codegen_func_decl(KawaCompiler *c, ASTNode *cur,
					   const char *implicit_self_struct) {
	LLVMContextRef ctx = c->context;
	LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);

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

	LLVMTypeRef *param_types =
		arena_alloc(c->arena, sizeof(LLVMTypeRef) *
								  (total_arg_cnt > 0 ? total_arg_cnt : 1));

	int type_idx = 0;
	for (ASTNode *a = cur->data.func.args; a; a = a->next)
		param_types[type_idx++] = get_llvm_type(c, a->data_type);

	LLVMTypeRef func_t = LLVMFunctionType(ret_t, param_types, total_arg_cnt, 0);

	// `main` adaptation: the OS entry point is `i32 @main(i32 argc,
	// ptr argv)`. A user main declared with those two params maps straight
	// onto it; any other shape gets renamed to kawa_main with a thin
	// synthesized @main wrapper calling it.
	const char *llvm_name = cur->data.func.name;
	int is_user_main = strcmp(llvm_name, "main") == 0 && !implicit_self_struct;
	if (is_user_main) {
		int args_ok = explicit_arg_cnt == 2 &&
					  cur->data.func.args->data_type &&
					  cur->data.func.args->data_type->kind == TYPE_I32 &&
					  cur->data.func.args->next &&
					  cur->data.func.args->next->data_type &&
					  cur->data.func.args->next->data_type->kind == TYPE_PTR;
		if (!args_ok)
			llvm_name = "kawa_main";
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
	c->current_ret_type = ret_t;
	c->current_ret_node_type = cur->data.func.ret_type;

	// Debug info: attach a DISubprogram so stacks/profiles show real names.
	// fn_node line isn't tracked at decl granularity; use 1 (file scope).
	kawa_di_attach_subprogram(c, llvm_name, cur);

	// Optimization attributes: nounwind enables exception-free codegen and
	// better scheduling; willreturn + memory(none) on pure functions lets
	// the optimizer hoist/delete calls. noinline keeps drip coroutines from
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

	// `pure fn`: declared side-effect-free. memory(none) lets the optimizer
	// hoist calls out of loops, CSE repeated calls and delete dead ones --
	// the single biggest lever for making Kawa beat naive C output.
	if (cur->data.func.is_pure) {
		const char *mem = "memory";
		unsigned mem_id = LLVMGetEnumAttributeKindForName(mem, strlen(mem));
		// memory(none) == no reads, no writes. The raw value encodes the
		// MemoryEffects bitfield; 0 is `none`.
		LLVMAddAttributeAtIndex(c->current_func, LLVMAttributeFunctionIndex,
								LLVMCreateEnumAttribute(c->context, mem_id, 0));
	}

	LLVMBasicBlockRef entry = LLVMAppendBasicBlock(c->current_func, "entry");
	LLVMPositionBuilderAtEnd(c->builder, entry);

	// Clear any debug location left over from the previous function --
	// instructions emitted here (param spills) would otherwise reference
	// the previous function's subprogram and fail verification.
	LLVMSetCurrentDebugLocation2(c->builder, NULL);

	// Defer/filter stacks are per-function; save and clear before the body.
	DeferFrame *saved_defers = c->defer_stack;
	FilterFrame *saved_filters = c->filter_stack;
	Scope *fn_scope_base = c->scope_stack;
	c->defer_stack = NULL;
	c->filter_stack = NULL;
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

		// Pointer parameters: `noalias` (Kawa has no address-taken
		// parameters escaping through globals) and `readonly` when the fn
		// is pure -- both feed alias analysis and vectorization. Parameter
		// attribute indices are 1-based; 0 means the return value.
		if (LLVMGetTypeKind(arg_type) == LLVMPointerTypeKind) {
			unsigned na_id = LLVMGetEnumAttributeKindForName("noalias", 7);
			LLVMAddAttributeAtIndex(c->current_func, arg_idx,
									LLVMCreateEnumAttribute(c->context, na_id, 0));
			if (cur->data.func.is_pure) {
				unsigned ro_id =
					LLVMGetEnumAttributeKindForName("readonly", 8);
				LLVMAddAttributeAtIndex(
					c->current_func, arg_idx,
					LLVMCreateEnumAttribute(c->context, ro_id, 0));
			}
		}

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
		for (Scope *sc = c->scope_stack; sc && sc != fn_scope_base; sc = sc->next) {
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
	c->overloads_active = saved_overload_idx;
}
