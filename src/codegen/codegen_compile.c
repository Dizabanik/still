#include "codegen_internal.h"

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

	// Pass 3: Generate functions. Struct bodies and aliases are already
	// resolved at this point, so any type reference inside a function
	// body resolves correctly on first pass.
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
