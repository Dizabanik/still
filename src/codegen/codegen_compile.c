#include "codegen_internal.h"

void kawa_compile(KawaCompiler *c, ASTNode *root) {
	ASTNode *cur = root->next;

	// Pass 1: Register Structs AND Aliases
	ASTNode *scanner = cur;
	while (scanner) {
		if (scanner->type == NODE_STRUCT_DECL) {
			LLVMTypeRef exists =
				LLVMGetTypeByName(c->module, scanner->data.struct_decl.name);
			if (!exists) {
				LLVMStructCreateNamed(c->context,
									  scanner->data.struct_decl.name);
			}
		}
		// [FIX] Register Alias
		if (scanner->type == NODE_ALIAS) {
			register_alias(scanner->data.alias.name, scanner->data_type);
		}

		scanner = scanner->next;
	}

	// Pass 2: Define Struct Bodies
	scanner = cur;
	while (scanner) {
		if (scanner->type == NODE_STRUCT_DECL) {
			LLVMTypeRef struct_t =
				LLVMGetTypeByName(c->module, scanner->data.struct_decl.name);

			// Count fields
			int field_count = 0;
			ASTNode *field = scanner->data.struct_decl.fields;
			while (field) {
				field_count++;
				field = field->next;
			}

			// Collect types
			LLVMTypeRef *elem_types = malloc(sizeof(LLVMTypeRef) * field_count);
			field = scanner->data.struct_decl.fields;
			int idx = 0;
			while (field) {
				elem_types[idx++] = get_llvm_type(c, field->data_type);
				field = field->next;
			}

			LLVMStructSetBody(struct_t, elem_types, field_count, 0);
			free(elem_types);

			// [FIX] Register Struct Def so get_field_index works!
			register_struct(scanner->data.struct_decl.name, struct_t);

			// Fill the fields info in the newly created StructDef (it's at
			// head)
			StructDef *sd = struct_defs;
			sd->field_count = field_count;

			field = scanner->data.struct_decl.fields;
			int f_idx = 0;
			while (field) {
				sd->fields[f_idx].name = strdup(field->data.var_decl.name);
				sd->fields[f_idx].type = get_llvm_type(c, field->data_type);
				f_idx++;
				field = field->next;
			}
		}
		scanner = scanner->next;
	}

	// Pass 3: Generate Functions
	while (cur) {
		if (cur->type == NODE_FUNC_DECL) {
			codegen_func_decl(c, cur, NULL);
		} else if (cur->type == NODE_IMPL_BLOCK) {
			ASTNode *method = cur->data.impl.methods;
			while (method) {
				codegen_func_decl(c, method, cur->data.impl.struct_name);
				method = method->next;
			}
		}
		// Structs already handled above
		cur = cur->next;
	}
}

void kawa_optimize_and_write(KawaCompiler *c, const char *filename) {
	// char *error = NULL;
	// if (LLVMVerifyModule(c->module, LLVMReturnStatusAction, &error)) {
	// 	timbr_err("LLVM Module Verification Failed:\n%s\n", error);
	// 	LLVMDisposeMessage(error);
	// 	// Dump the broken module so you can see where the error is
	// 	LLVMDumpModule(c->module);
	// 	exit(1);
	// }
	// LLVMDisposeMessage(error);

	// [OPTIMIZATION] Set up Target Machine for Host CPU
	char *error_msg = NULL;
	LLVMTargetRef target;
	LLVMGetTargetFromTriple(LLVMGetDefaultTargetTriple(), &target, &error_msg);
	if (!target) {
		timbr_err("Target selection failed: %s\n", error_msg);
		return;
	}

	LLVMTargetMachineRef machine = LLVMCreateTargetMachine(
		target, LLVMGetDefaultTargetTriple(), LLVMGetHostCPUName(),
		LLVMGetHostCPUFeatures(), LLVMCodeGenLevelAggressive, LLVMRelocDefault,
		LLVMCodeModelDefault);

	// Set Data Layout (Critical for Vectorization)
	LLVMSetModuleDataLayout(c->module, LLVMCreateTargetDataLayout(machine));
	LLVMSetTarget(c->module, LLVMGetDefaultTargetTriple());

	LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();

	// [OPTIMIZATION] Cleaned up pass pipeline
	// "default<O3>" covers most cases. We prepend coro specific passes.
	LLVMRunPasses(c->module,
				  "coro-early,coro-split,coro-elide,coro-cleanup,default<O3>",
				  machine, opts);

	LLVMDisposePassBuilderOptions(opts);

	if (LLVMWriteBitcodeToFile(c->module, filename) != 0) {
		timbr_err("Error writing bitcode\n");
	}
	// char *text = LLVMPrintModuleToString(c->module);
	// LLVMDumpModule(c->module);
	if (LLVMPrintModuleToFile(c->module, "output.ll", &error_msg)) {
		timbr_err("Writing file failed: %s\n", error_msg);
	};

	LLVMDisposeTargetMachine(machine);
}
