#include "codegen_internal.h"

// Debug-info emission (-g). v1 scope: compile unit + file + subprogram
// metadata and per-statement line locations. That's what makes lldb,
// Instruments and `sample` show Kawa function names and call lines instead
// of raw addresses -- the minimum for profiling real programs.

void kawa_di_init(KawaCompiler *c, const char *source_filename) {
	if (!c->debug_build)
		return;
	c->di_builder = LLVMCreateDIBuilder(c->module);

	// Split the source path into directory + basename for DIFile.
	const char *slash = strrchr(source_filename, '/');
	char dir[1024] = ".", name[512];
	if (slash) {
		size_t dl = (size_t)(slash - source_filename);
		if (dl >= sizeof(dir))
			dl = sizeof(dir) - 1;
		memcpy(dir, source_filename, dl);
		dir[dl] = '\0';
		snprintf(name, sizeof(name), "%s", slash + 1);
	} else {
		snprintf(name, sizeof(name), "%s", source_filename);
	}

	c->di_file = LLVMDIBuilderCreateFile(
		c->di_builder, name, strlen(name), dir, strlen(dir));

	const char *producer = "kawac 0.1.0";
	LLVMDIBuilderCreateCompileUnit(
		c->di_builder, LLVMDWARFSourceLanguageC11, c->di_file, producer,
		strlen(producer), 0 /* isOptimized */, "", 0, 0 /* RuntimeVer */,
		"" /* SplitName */, 0, LLVMDWARFEmissionFull, 0,
		0 /* SplitDebugInlining */, 0 /* DebugInfoForProfiling */,
		"" /* SysRoot */, 0, "" /* SDK */, 0);

	c->di_line = 0;

	// The C API doesn't add these itself; without them llvm-dis/tools drop
	// the metadata as "invalid version (0)".
	LLVMAddModuleFlag(c->module, LLVMModuleFlagBehaviorWarning,
					  "Debug Info Version", strlen("Debug Info Version"),
					  LLVMValueAsMetadata(LLVMConstInt(
						  LLVMInt32TypeInContext(c->context), 3, 0)));
	LLVMAddModuleFlag(c->module, LLVMModuleFlagBehaviorWarning,
					  "Dwarf Version", strlen("Dwarf Version"),
					  LLVMValueAsMetadata(LLVMConstInt(
						  LLVMInt32TypeInContext(c->context), 4, 0)));
}

void kawa_di_finalize(KawaCompiler *c) {
	if (!c->di_builder)
		return;
	LLVMDIBuilderFinalize(c->di_builder);
}

// Map a Kawa type to a DWARF type descriptor. Basic ints/floats/pointers
// cover the common cases; anything else falls back to void so emission
// never fails.
static LLVMMetadataRef di_type(KawaCompiler *c, Type *t) {
	if (!c->di_builder)
		return NULL;
	if (!t)
		return NULL;

	uint64_t size = 0;
	const char *dn = NULL;
	switch (t->kind) {
	case TYPE_VOID:
		return NULL; // callers substitute the subroutine-type fallback
	case TYPE_BOOL:
		size = 1;
		dn = "bool";
		break;
	case TYPE_CHAR:
	case TYPE_U8:
		size = 8;
		dn = t->kind == TYPE_CHAR ? "char" : "u8";
		break;
	case TYPE_I8:
		size = 8;
		dn = "i8";
		break;
	case TYPE_I16:
		size = 16;
		dn = "i16";
		break;
	case TYPE_U16:
		size = 16;
		dn = "u16";
		break;
	case TYPE_I32:
		size = 32;
		dn = "i32";
		break;
	case TYPE_U32:
		size = 32;
		dn = "u32";
		break;
	case TYPE_I64:
		size = 64;
		dn = "i64";
		break;
	case TYPE_U64:
		size = 64;
		dn = "u64";
		break;
	case TYPE_F32:
		size = 32;
		dn = "f32";
		break;
	case TYPE_F64:
		size = 64;
		dn = "f64";
		break;
	default:
		break;
	}

	if (dn) {
		unsigned enc = (t->kind == TYPE_F32 || t->kind == TYPE_F64)
						   ? 0x04 /* DW_ATE_float */
						   : (t->kind == TYPE_BOOL ? 0x02 /* DW_ATE_boolean */
												   : 0x05 /* DW_ATE_signed */);
		if (dn[0] == 'u' && dn[1] >= '0' && dn[1] <= '9')
			enc = 0x07; // DW_ATE_unsigned
		if (t->kind == TYPE_CHAR)
			enc = 0x08; // DW_ATE_unsigned_char
		return LLVMDIBuilderCreateBasicType(c->di_builder, dn, strlen(dn),
											size, enc, 0);
	}

	if ((t->kind == TYPE_PTR || t->kind == TYPE_AMP) && c->di_file) {
		LLVMMetadataRef pointee =
			t->inner ? di_type(c, t->inner) : NULL;
		if (!pointee) {
			// Pointer to unknown -> i8*.
			pointee = LLVMDIBuilderCreateBasicType(
				c->di_builder, "i8", 2, 8, 0x07 /* DW_ATE_unsigned */, 0);
		}
		return LLVMDIBuilderCreatePointerType(
			c->di_builder, pointee, 64, 64, 0, "", 0);
	}

	return NULL;
}

LLVMMetadataRef kawa_di_subprogram(KawaCompiler *c, const char *name,
								   unsigned line, ASTNode *fn_node) {
	if (!c->di_builder)
		return NULL;

	// Subroutine type: return type followed by parameter types (the C API
	// wants them all in one array, void return included as the first slot).
	Type *ret_ast =
		fn_node->data.func.ret_type
			? fn_node->data.func.ret_type
			: NULL;

	LLVMMetadataRef params[17]; // ret + up to 16 params
	unsigned pcount = 0;

	LLVMMetadataRef ret_di = NULL;
	if (!fn_node->data.func.is_drip)
		ret_di = di_type(c, ret_ast);
	if (!ret_di) {
		ret_di = LLVMDIBuilderCreateBasicType(
			c->di_builder, "void", 4, 0, 0x07 /* DW_ATE_unsigned */, 0);
	}
	params[pcount++] = ret_di;

	for (ASTNode *a = fn_node->data.func.args; a && pcount < 17;
		 a = a->next) {
		LLVMMetadataRef pdi = di_type(c, a->data_type);
		if (!pdi)
			pdi = LLVMDIBuilderCreateBasicType(c->di_builder, "void", 4, 0,
											   0x07 /* DW_ATE_unsigned */, 0);
		params[pcount++] = pdi;
	}

	LLVMMetadataRef ty = LLVMDIBuilderCreateSubroutineType(
		c->di_builder, c->di_file, params, pcount, 0);

	LLVMMetadataRef sp = LLVMDIBuilderCreateFunction(
		c->di_builder, c->di_file, name, strlen(name), name, strlen(name),
		c->di_file, line, ty, 1 /* IsLocalToUnit */, 1 /* IsDefinition */,
		line /* ScopeLine */, LLVMDIFlagZero, 0 /* IsOptimized */);
	return sp;
}

// Create the DISubprogram for a function and attach it to the LLVM func
// that is currently being emitted (c->current_func).
void kawa_di_attach_subprogram(KawaCompiler *c, const char *name,
							   ASTNode *fn_node) {
	if (!c->di_builder)
		return;
	LLVMMetadataRef sp =
		kawa_di_subprogram(c, name, fn_node->line > 0 ? fn_node->line : 1,
						   fn_node);
	if (sp)
		LLVMSetSubprogram(c->current_func, sp);
}

// Attach a source location to whatever instruction the builder emits next.
void kawa_di_set_location(KawaCompiler *c, int line) {
	if (!c->di_builder || !c->current_func || line <= 0)
		return;
	c->di_line = line;
	LLVMMetadataRef loc = LLVMDIBuilderCreateDebugLocation(
		c->context, line, 0, LLVMGetSubprogram(c->current_func), NULL);
	LLVMSetCurrentDebugLocation2(c->builder, loc);
}
