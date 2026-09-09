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
	c->program_root = root; // comptime fn lookup
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

	// Function overloading: find bare names declared more than once so
	// those functions emit under mangled symbols and call sites resolve
	// by argument type. Runs before emission so codegen_func_decl sees it.
	collect_overloads(c, cur);
	collect_impl_methods(c, cur);
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
		else if (scanner->type == NODE_IMPL_BLOCK) {
			for (ASTNode *m = scanner->data.impl.methods; m; m = m->next) {
				if (m->type == NODE_VAR_DECL)
					codegen_global_decl(c, m, &pending_inits);
			}
		}
	}

	// Snapshot the file-level scope (globals) so coroutine bodies can
	// reference them without seeing the spawning function's locals.
	c->global_scope = c->scope_stack;

	// Generic detection (IDEAS 2.2): a fn whose return or param type
	// mentions `T` (a single-uppercase-letter "struct" type) is generic.
	// Such fns are registered here and instantiated per call site with a
	// mangled name -- no IR is emitted for the template itself.
	for (ASTNode *g = cur; g; g = g->next) {
		if (g->type != NODE_FUNC_DECL || c->generic_fn_count >= 64)
			continue;
		Type *sig[32];
		int sn = 0;
		if (g->data.func.ret_type && sn < 32)
			sig[sn++] = g->data.func.ret_type;
		for (ASTNode *a = g->data.func.args; a && sn < 32; a = a->next)
			if (a->data_type)
				sig[sn++] = a->data_type;
		int is_generic = 0;
		for (int ti = 0; ti < sn && !is_generic; ti++) {
			Type *ty = sig[ti];
			while (ty &&
				   (ty->kind == TYPE_ARRAY || ty->kind == TYPE_SLICE ||
					ty->kind == TYPE_PTR))
				ty = ty->inner;
			if (ty && ty->kind == TYPE_STRUCT && ty->name &&
				strlen(ty->name) == 1 && ty->name[0] == 'T')
				is_generic = 1;
		}
		if (is_generic)
			c->generic_fns[c->generic_fn_count++] = g;
	}

	while (cur) {
		if (cur->type == NODE_EXTERN_FN) {
			// Declare the C symbol with its exact prototype. External
			// linkage, no body: the linker resolves it from any library
			// on the link line -- no header translation needed.
			LLVMTypeRef ret_t = LLVMInt32TypeInContext(c->context);
			if (cur->data.extern_fn.ret_type)
				ret_t = get_llvm_type(c, cur->data.extern_fn.ret_type);
			int argc = 0;
			for (ASTNode *a = cur->data.extern_fn.args; a; a = a->next)
				argc++;
			LLVMTypeRef *params =
				arena_alloc(c->arena, sizeof(LLVMTypeRef) * (argc > 0 ? argc : 1));
			int pi = 0;
			for (ASTNode *a = cur->data.extern_fn.args; a; a = a->next)
				params[pi++] = get_llvm_type(c, a->data_type);
			LLVMTypeRef fn_t =
				LLVMFunctionType(ret_t, params, (unsigned)argc,
								 (int)cur->data.extern_fn.is_variadic);
			LLVMValueRef fn =
				LLVMAddFunction(c->module, cur->data.extern_fn.name, fn_t);
			// A declared-but-never-called extern costs nothing; marking
			// nounwind lets the optimizer treat calls as leaf ops.
			const char *nw = "nounwind";
			LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex,
									LLVMCreateEnumAttribute(
										c->context,
										LLVMGetEnumAttributeKindForName(
											nw, strlen(nw)),
										0));
			cur = cur->next;
			continue;
		}
		if (cur->type == NODE_FUNC_DECL) {
			// Generic fn: registered earlier, instantiated at call sites.
			int skip_generic = 0;
			for (int gi3 = 0; gi3 < c->generic_fn_count; gi3++)
				if (c->generic_fns[gi3] == cur)
					skip_generic = 1;
			if (skip_generic) {
				cur = cur->next;
				continue;
			}
			if (cur->data.func.is_test &&
				c->test_fn_count < 256)
				c->test_fns[c->test_fn_count++] = cur;
			codegen_func_decl(c, cur, NULL);
		} else if (cur->type == NODE_IMPL_BLOCK) {
			for (ASTNode *method = cur->data.impl.methods; method;
				 method = method->next) {
				if (method->type == NODE_FUNC_DECL)
					codegen_func_decl(c, method, cur->data.impl.struct_name);
			}
		}
		cur = cur->next;
	}

	emit_runtime_global_inits(c, pending_inits, &globals_init_fn,
							  &globals_init_type);


	// Test mode: synthesize kawa__run_all_tests() -- calls each #[test] fn
	// in order, prints PASS/FAIL, returns the failure count. @main then
	// calls the runner instead of user main; exit code is the failures.
	if (c->test_mode && c->test_fn_count > 0) {
		LLVMTypeRef i32_t = LLVMInt32TypeInContext(c->context);
		LLVMTypeRef i8ptr =
			LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
		LLVMTypeRef printf_t = LLVMFunctionType(
			i32_t, (LLVMTypeRef[]){i8ptr}, 1, 1);
		LLVMValueRef printf_fn = LLVMGetNamedFunction(c->module, "printf");
		if (!printf_fn)
			printf_fn = LLVMAddFunction(c->module, "printf", printf_t);

		LLVMTypeRef runner_t = LLVMFunctionType(i32_t, NULL, 0, 0);
		LLVMValueRef runner =
			LLVMAddFunction(c->module, "kawa__run_all_tests", runner_t);
		LLVMBasicBlockRef rb = LLVMAppendBasicBlock(runner, "entry");
		LLVMPositionBuilderAtEnd(c->builder, rb);

		for (int ti = 0; ti < c->test_fn_count; ti++) {
			ASTNode *tf = c->test_fns[ti];
			// The LLVM function name is what codegen_func_decl created;
			// find it by source name (methods are mangled, tests never are).
			LLVMValueRef tfn = LLVMGetNamedFunction(
				c->module, tf->data.func.name);
			if (!tfn || tf->data.func.is_ignored)
				continue;
			const char *nm = tf->data.func.name;
			LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(tfn), tfn,
						   NULL, 0, "");
			char fmtbuf[64];
			snprintf(fmtbuf, sizeof(fmtbuf),
					 "PASS %s\n", nm);
			if (getenv("KAWA_NO_PRINTF"))
				continue;
			// LLVMBuildGlobalStringPtr is the same path user string
			// literals take: it creates a properly-typed private constant
			// and folds to i8* without any manual GEP arithmetic.
			LLVMValueRef fmt_ptr =
				LLVMBuildGlobalStringPtr(c->builder, fmtbuf, "passmsg");
			LLVMBuildCall2(c->builder, printf_t, printf_fn,
						   (LLVMValueRef[]){fmt_ptr}, 1, "");
		}
		LLVMBuildRet(c->builder,
					 LLVMConstInt(LLVMInt32TypeInContext(c->context), 0, 0));
	}

	// If user `main` was renamed kawa_main (any signature that isn't
	// exactly (i32 argc, ptr argv)), synthesize the real entry point:
	//   i32 @main(i32 argc, ptr argv) { return kawa_main(); }
	LLVMValueRef renamed = LLVMGetNamedFunction(c->module, "kawa_main");
	LLVMValueRef test_runner =
		c->test_mode ? LLVMGetNamedFunction(c->module,
											"kawa__run_all_tests")
					 : NULL;
	int wrapper_handled_globals_init = 0;
	if (c->test_mode && test_runner && !renamed) {
		// Tests without a user main: entry calls the runner directly.
		LLVMTypeRef i32_t = LLVMInt32TypeInContext(c->context);
		LLVMTypeRef i8ptr =
			LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
		LLVMValueRef wrapper = LLVMAddFunction(
			c->module, "main",
			LLVMFunctionType(i32_t, (LLVMTypeRef[]){i32_t, i8ptr}, 2, 0));
		LLVMBasicBlockRef bb = LLVMAppendBasicBlock(wrapper, "entry");
		LLVMPositionBuilderAtEnd(c->builder, bb);
		if (globals_init_fn) {
			LLVMBuildCall2(c->builder, globals_init_type, globals_init_fn,
						   NULL, 0, "");
			wrapper_handled_globals_init = 1;
		}
		LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(test_runner),
					   test_runner, NULL, 0, "");
		LLVMBuildRet(c->builder,
					 LLVMConstInt(LLVMInt32TypeInContext(c->context), 0, 0));
	} else if (renamed) {
		LLVMTypeRef i32_t = LLVMInt32TypeInContext(c->context);
		LLVMTypeRef i8ptr =
			LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
		LLVMTypeRef params[] = {i32_t, i8ptr};
		LLVMTypeRef main_t = LLVMFunctionType(i32_t, params, 2, 0);
		LLVMValueRef wrapper = LLVMAddFunction(c->module, "main", main_t);
		LLVMBasicBlockRef bb = LLVMAppendBasicBlock(wrapper, "entry");
		LLVMPositionBuilderAtEnd(c->builder, bb);
		// The globals-init injection below targets @main's entry; build the
		// call AFTER positioning so it lands inside this new block.
		if (globals_init_fn) {
			LLVMBuildCall2(c->builder, globals_init_type, globals_init_fn,
						   NULL, 0, "");
			wrapper_handled_globals_init = 1;
		}
		if (test_runner)
			LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(test_runner),
						   test_runner, NULL, 0, "");
		else
			LLVMBuildCall2(
				c->builder, LLVMGlobalGetValueType(renamed), renamed, NULL,
				0, "");
		LLVMBuildRet(c->builder, LLVMConstNull(i32_t));
	}

	// Capture OS argc/argv for std.process.arg_count/arg_at. If a user
	// main has the (i32, ptr) shape its params ARE the entry's; otherwise
	// the synthesized wrapper receives them. Either way the values live in
	// @main's first two params (or don't exist -> globals stay null and
	// arg_count() returns 0).
	{
		LLVMValueRef main_fn = LLVMGetNamedFunction(c->module, "main");
		if (main_fn && LLVMCountParams(main_fn) >= 2) {
			LLVMTypeRef i32_t = LLVMInt32TypeInContext(c->context);
			LLVMTypeRef i8t =
				LLVMInt8TypeInContext(c->context);
			LLVMValueRef argc_g = LLVMGetNamedGlobal(c->module,
													 "__kawa_argc");
			if (!argc_g) {
				argc_g = LLVMAddGlobal(c->module, i32_t, "__kawa_argc");
				LLVMSetInitializer(argc_g, LLVMConstNull(i32_t));
				LLVMSetLinkage(argc_g, LLVMPrivateLinkage);
			}
			LLVMValueRef argv_g = LLVMGetNamedGlobal(c->module,
													 "__kawa_argv");
			if (!argv_g) {
				argv_g = LLVMAddGlobal(
					c->module, LLVMPointerType(LLVMPointerType(i8t, 0), 0),
					"__kawa_argv");
				LLVMSetInitializer(
					argv_g,
					LLVMConstNull(LLVMPointerType(
						LLVMPointerType(i8t, 0), 0)));
				LLVMSetLinkage(argv_g, LLVMPrivateLinkage);
			}
			LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(main_fn);
			LLVMBuilderRef tmp = LLVMCreateBuilderInContext(c->context);
			LLVMValueRef first = LLVMGetFirstInstruction(entry);
			if (first)
				LLVMPositionBuilderBefore(tmp, first);
			else
				LLVMPositionBuilderAtEnd(tmp, entry);
			LLVMBuildStore(tmp, LLVMGetParam(main_fn, 0), argc_g);
			LLVMBuildStore(tmp, LLVMGetParam(main_fn, 1), argv_g);
			LLVMDisposeBuilder(tmp);
		}
	}

	// Inject a call to kawa_globals_init at the top of main() so runtime-
	// initialized globals are ready before any user code runs. Constant-
	// initialized globals need no call at all. (Skipped when the synthesized
	// kawa_main wrapper already emitted the call -- it would run twice.)
	if (globals_init_fn && !wrapper_handled_globals_init) {
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
			if (getenv("KAWA_DUMP_BAD"))
				LLVMPrintModuleToFile(c->module, "tmp/bad2.ll", NULL);
			kdiag_error_at(KAWA_E_SEMANTIC,
						   c->source_filename ? c->source_filename : "<kawa>",
						   NULL, 0, "LLVM module verification failed:\n%s",
						   error);
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
		kdiag_error_at(KAWA_E_SEMANTIC,
					   c->source_filename ? c->source_filename : "<kawa>", NULL,
					   0, "target selection failed: %s", error_msg);
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
	// lowers the frame before inlining decisions are made. The pass
	// pipeline follows the -O level: O0 skips optimization entirely, O1/O2
	// use LLVM's curated defaults (O2 is kawac's default), and O3 layers
	// aggressive vectorization + unrolling on top.
	LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
	if (!getenv("KAWA_NO_OPT")) {
		const char *pipeline;
		switch (c->opt_level) {
		case 0:
			pipeline = "coro-early,coro-split,coro-elide,coro-cleanup";
			break;
		case 1:
			pipeline = "coro-early,coro-split,coro-elide,coro-cleanup,"
					   "default<O1>";
			break;
		case 3:
			pipeline = "coro-early,coro-split,coro-elide,coro-cleanup,"
					   "default<O3>,lto<O3>";
			break;
		default:
			pipeline = "coro-early,coro-split,coro-elide,coro-cleanup,"
					   "default<O2>";
			break;
		}
		LLVMRunPasses(c->module, pipeline, machine, opts);
	}
	LLVMDisposePassBuilderOptions(opts);

	// Finalize debug info BEFORE optimization runs -- DIBuilder must see
	// its subprograms intact.
	kawa_di_finalize(c);

	if (LLVMWriteBitcodeToFile(c->module, filename) != 0)
		kdiag_error_at(KAWA_E_SEMANTIC,
					   c->source_filename ? c->source_filename : "<kawa>", NULL,
					   0, "error writing bitcode");
	if (LLVMPrintModuleToFile(c->module, "output.ll", &error_msg)) {
		kdiag_error_at(KAWA_E_SEMANTIC,
					   c->source_filename ? c->source_filename : "<kawa>", NULL,
					   0, "writing file failed: %s", error_msg);
		LLVMDisposeMessage(error_msg);
	}

	// Emit the object file directly through the same TargetMachine. This
	// keeps DWARF sections (clang's .bc pipeline was dropping them) and
	// gives the driver something to link without recompiling LLVM IR.
	const char *obj_path = "output.o";
	if (LLVMTargetMachineEmitToFile(machine, c->module, obj_path,
									LLVMObjectFile, &error_msg)) {
		kdiag_error_at(KAWA_E_SEMANTIC,
					   c->source_filename ? c->source_filename : "<kawa>", NULL,
					   0, "emitting object failed: %s", error_msg);
		LLVMDisposeMessage(error_msg);
	}

	LLVMDisposeTargetMachine(machine);
}
