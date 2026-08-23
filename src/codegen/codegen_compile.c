#include "codegen_internal.h"

// Runtime-initialized globals can't emit their kawa_globals_init body during
// pass 3 -- user functions don't exist yet, so a call like `let g = make();`
// would fail to resolve its callee. Pass 3 records them here; the bodies are
// emitted after all functions are generated.
typedef struct PendingGlobalInit {
	ASTNode *decl;
	LLVMValueRef global;
	LLVMTypeRef g_type;
	struct PendingGlobalInit *next;
} PendingGlobalInit;

static void emit_runtime_global_inits(KawaCompiler *c,
									  PendingGlobalInit *pending,
									  LLVMValueRef *init_fn,
									  LLVMTypeRef *init_fn_type);

// Lower a top-level `let x = ...;` / `[N]i32 arr = {...};` declaration into a
// module-level global. Constant initializers become LLVM constants (zero
// runtime cost, folded into every use); anything else gets queued for the
// synthesized `kawa_globals_init` that main() calls first.
static void codegen_global_decl(KawaCompiler *c, ASTNode *n,
								PendingGlobalInit **pending) {
	LLVMTypeRef g_type = get_llvm_type(c, n->data_type);

	// Propagate the declared type to a struct/array literal initializer so
	// BOTH the const-eval path and the runtime path see the right layout.
	if (n->data.var_decl.init && n->data.var_decl.init->type == NODE_STRUCT_LITERAL &&
		!n->data.var_decl.init->data_type)
		n->data.var_decl.init->data_type = n->data_type;

	LLVMValueRef init_const = NULL;
	int needs_runtime_init = 0;
	if (n->data.var_decl.init) {
		if (!global_init_is_constant(c, n->data.var_decl.init))
			needs_runtime_init = 1;
		else
			init_const = const_eval_global_init(c, n->data.var_decl.init,
												g_type, n->data_type);
	}

	LLVMValueRef global =
		LLVMAddGlobal(c->module, g_type, n->data.var_decl.name);
	LLVMSetInitializer(global,
					   init_const ? init_const : LLVMConstNull(g_type));
	// Whole-program model: every global lives and dies inside this module.
	// Internal linkage lets LLVM see that -- and on Mach-O it decides where
	// zero storage goes: external globals land in __DATA,__common, which
	// costs real time at first touch for huge arrays; internal ones get
	// proper .zerofill __DATA,__bss, same as static C.
	LLVMSetLinkage(global, LLVMInternalLinkage);
	LLVMSetAlignment(global, 16);

	// `const x = ...` at file scope: the value never changes, so say so.
	// This lets the optimizer fold loads and keep the global in registers.
	if (n->data.var_decl.is_const && !n->data.var_decl.is_orbit)
		LLVMSetGlobalConstant(global, 1);

	// Register so function bodies resolve the name to this storage.
	scope_push(c, n->data.var_decl.name, global, g_type, n);

	if (needs_runtime_init) {
		PendingGlobalInit *p = arena_alloc(c->arena, sizeof(PendingGlobalInit));
		p->decl = n;
		p->global = global;
		p->g_type = g_type;
		p->next = *pending;
		*pending = p;
	}
}

// Emit kawa_globals_init after all user functions exist, so initializers may
// call any of them. Declaration order is preserved by walking the pending
// list back to front.
static void emit_runtime_global_inits(KawaCompiler *c,
									  PendingGlobalInit *pending,
									  LLVMValueRef *init_fn,
									  LLVMTypeRef *init_fn_type) {
	if (!pending)
		return;

	// Reverse into source order.
	PendingGlobalInit *rev = NULL;
	for (PendingGlobalInit *p = pending; p; p = p->next) {
		PendingGlobalInit *cell =
			arena_alloc(c->arena, sizeof(PendingGlobalInit));
		*cell = *p;
		cell->next = rev;
		rev = cell;
	}

	*init_fn_type = LLVMFunctionType(LLVMVoidTypeInContext(c->context), NULL,
									 0, 0);
	*init_fn = LLVMAddFunction(c->module, "kawa_globals_init", *init_fn_type);
	unsigned nw_id = LLVMGetEnumAttributeKindForName("nounwind", 8);
	LLVMAddAttributeAtIndex(*init_fn, LLVMAttributeFunctionIndex,
							LLVMCreateEnumAttribute(c->context, nw_id, 0));
	LLVMPositionBuilderAtEnd(c->builder,
							 LLVMAppendBasicBlock(*init_fn, "entry"));

	LLVMValueRef saved_func = c->current_func;
	LLVMTypeRef saved_ret = c->current_ret_type;
	c->current_func = *init_fn;
	c->current_ret_type = *init_fn_type;

	for (PendingGlobalInit *p = rev; p; p = p->next) {
		ASTNode *n = p->decl;
		LLVMValueRef val = codegen_expr(c, n->data.var_decl.init);
		val = coerce_value(c, val, n->data.var_decl.init->data_type,
						   p->g_type, n->data_type);
		LLVMValueRef store = LLVMBuildStore(c->builder, val, p->global);
		attach_tbaa(c, store, p->g_type);
	}

	if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder)))
		LLVMBuildRetVoid(c->builder);

	c->current_func = saved_func;
	c->current_ret_type = saved_ret;
}

void kawa_compile(KawaCompiler *c, ASTNode *root) {
	ASTNode *cur = root->next;

	// Pass 1: Forward-declare named structs + register aliases. Aliases are
	// resolved lazily through resolve_alias_type, so we only need to track
	// the target Type* here.
	for (ASTNode *scanner = cur; scanner; scanner = scanner->next) {
		if (scanner->type == NODE_STRUCT_DECL) {
			if (!LLVMGetTypeByName(c->module, scanner->data.struct_decl.name))
				LLVMStructCreateNamed(c->context,
									  scanner->data.struct_decl.name);
		}
		if (scanner->type == NODE_ALIAS)
			register_alias(c, scanner->data.alias.name, scanner->data_type);
	}

	// Pass 2: Define struct bodies + populate the field table in one go.
	for (ASTNode *scanner = cur; scanner; scanner = scanner->next) {
		if (scanner->type != NODE_STRUCT_DECL)
			continue;
		LLVMTypeRef struct_t =
			LLVMGetTypeByName(c->module, scanner->data.struct_decl.name);

		int field_count = 0;
		for (ASTNode *f = scanner->data.struct_decl.fields; f; f = f->next)
			field_count++;

		LLVMTypeRef *elem_types =
			arena_alloc(c->arena, sizeof(LLVMTypeRef) *
									  (field_count > 0 ? field_count : 1));
		int idx = 0;
		for (ASTNode *f = scanner->data.struct_decl.fields; f; f = f->next)
			elem_types[idx++] = get_llvm_type(c, f->data_type);
		LLVMStructSetBody(struct_t, elem_types, field_count, 0);

		register_struct(c, scanner->data.struct_decl.name, struct_t,
						scanner->data.struct_decl.fields);
	}

	// Pass 3: Globals first (function bodies may reference them), then
	// functions. Constant-initialized globals are done here; runtime-
	// initialized ones are queued and emitted after all functions exist so
	// their initializers can call any user function.
	LLVMValueRef globals_init_fn = NULL;
	LLVMTypeRef globals_init_type = NULL;
	PendingGlobalInit *pending_inits = NULL;

	for (ASTNode *scanner = cur; scanner; scanner = scanner->next) {
		if (scanner->type == NODE_VAR_DECL)
			codegen_global_decl(c, scanner, &pending_inits);
	}

	while (cur) {
		if (cur->type == NODE_FUNC_DECL) {
			codegen_func_decl(c, cur, NULL);
		} else if (cur->type == NODE_IMPL_BLOCK) {
			for (ASTNode *method = cur->data.impl.methods; method;
				 method = method->next)
				codegen_func_decl(c, method, cur->data.impl.struct_name);
		}
		cur = cur->next;
	}

	emit_runtime_global_inits(c, pending_inits, &globals_init_fn,
							  &globals_init_type);

	// Inject a call to kawa_globals_init at the top of main() so runtime-
	// initialized globals are ready before any user code runs. Constant-
	// initialized globals need no call at all.
	if (globals_init_fn) {
		LLVMValueRef main_fn = LLVMGetNamedFunction(c->module, "main");
		if (main_fn) {
			LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(main_fn);
			LLVMBuilderRef tmp = LLVMCreateBuilderInContext(c->context);
			LLVMValueRef first = LLVMGetFirstInstruction(entry);
			if (first)
				LLVMPositionBuilderBefore(tmp, first);
			else
				LLVMPositionBuilderAtEnd(tmp, entry);
			LLVMBuildCall2(tmp, globals_init_type, globals_init_fn, NULL, 0,
						   "");
			LLVMDisposeBuilder(tmp);
		}
	}
}

void kawa_optimize_and_write(KawaCompiler *c, const char *filename) {
	// Verify the module *before* any optimization runs. This catches
	// malformed metadata, type mismatches, and structural IR errors with
	// the most precise diagnostics (post-optimization errors are harder to
	// attribute).
	{
		char *error = NULL;
		if (LLVMVerifyModule(c->module, LLVMPrintMessageAction, &error)) {
			timbr_err("LLVM Module Verification Failed:\n%s\n", error);
			LLVMDumpModule(c->module);
			LLVMDisposeMessage(error);
			exit(1);
		}
		if (error)
			LLVMDisposeMessage(error);
	}

	char *error_msg = NULL;
	LLVMTargetRef target;
	const char *triple = LLVMGetDefaultTargetTriple();
	if (LLVMGetTargetFromTriple(triple, &target, &error_msg)) {
		timbr_err("Target selection failed: %s\n", error_msg);
		LLVMDisposeMessage(error_msg);
		return;
	}

	// Aggressive codegen level + host CPU/features so the backend can use
	// every instruction the machine has (AVX-512, etc.). Combined with
	// default<O3> below this is what gets Kawa output on par with -O3 C.
	LLVMTargetMachineRef machine = LLVMCreateTargetMachine(
		target, triple, LLVMGetHostCPUName(), LLVMGetHostCPUFeatures(),
		LLVMCodeGenLevelAggressive, LLVMRelocDefault, LLVMCodeModelDefault);

	// Set the data layout first -- vectorization needs a concrete layout.
	LLVMSetModuleDataLayout(c->module, LLVMCreateTargetDataLayout(machine));
	LLVMSetTarget(c->module, triple);

	// Coroutine transforms must run before the main pipeline so coro-split
	// lowers the frame before inlining decisions are made.
	LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
	if (!getenv("KAWA_NO_OPT"))
		LLVMRunPasses(c->module,
					  "coro-early,coro-split,coro-elide,coro-cleanup,default<O3>",
					  machine, opts);
	LLVMDisposePassBuilderOptions(opts);

	if (LLVMWriteBitcodeToFile(c->module, filename) != 0)
		timbr_err("Error writing bitcode\n");
	if (LLVMPrintModuleToFile(c->module, "output.ll", &error_msg)) {
		timbr_err("Writing file failed: %s\n", error_msg);
		LLVMDisposeMessage(error_msg);
	}

	LLVMDisposeTargetMachine(machine);
}
