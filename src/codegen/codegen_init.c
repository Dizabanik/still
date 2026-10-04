#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "codegen_internal.h"
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

static char *native_triple(KawaCompiler *c,const char *default_triple) {
#ifdef __APPLE__
	/* A Darwin kernel version is not a macOS deployment version. LLVM's
	 * kernel-to-product mapping can lag a new OS; use the product version
	 * directly and honor the same deployment override as the C linker. */
	char product[64]={0};
	size_t size=sizeof(product);
	const char *version=getenv("MACOSX_DEPLOYMENT_TARGET");
	if (!version) {
		if (sysctlbyname("kern.osproductversion",product,&size,NULL,0))
			return arena_strdup(c->arena,default_triple);
		version=product;
	}
	unsigned components[3]={0};
	const char *part=version;
	int valid=1;
	for (unsigned i=0;i<3;++i) {
		if (*part<'0' || *part>'9') { valid=0; break; }
		char *end;
		unsigned long value=strtoul(part,&end,10);
		if (value>999) { valid=0; break; }
		components[i]=(unsigned)value;
		if (!*end) break;
		if (*end!='.' || i==2) { valid=0; break; }
		part=end+1;
	}
	if (!valid || !components[0]) {
		kdiag_error_at(KAWA_E_SEMANTIC,"<kawa>",NULL,0,"invalid macOS deployment version");
		exit(1);
	}
	if (!getenv("MACOSX_DEPLOYMENT_TARGET")) components[2]=0;
	const char *apple=strstr(default_triple,"-apple-");
	if (apple) {
		size_t architecture=(size_t)(apple-default_triple);
		char *out=arena_alloc(c->arena,architecture+64);
		snprintf(out,architecture+64,"%.*s-apple-macosx%u.%u.%u",(int)architecture,
			default_triple,components[0],components[1],components[2]);
		return out;
	}
#endif
	return arena_strdup(c->arena,default_triple);
}

void kawa_set_debug(KawaCompiler *c, int debug) { c->debug_build = debug; }

void kawa_set_source_file(KawaCompiler *c, const char *filename) {
	c->source_filename = filename;
	kawa_di_init(c, filename); // no-op unless debug_build is set
}

void kawa_init(KawaCompiler *c, const char *module_name, Arena *arena) {
	// Zero everything first so any field we forget to initialize below
	// is at least NULL/0 and not garbage.
	memset(c, 0, sizeof(*c));

	if (!arena) {
		kdiag_error_at(KAWA_E_SEMANTIC, "<kawa>", NULL, 0,
					   "kawa_init: arena must not be NULL"); // internal
		exit(1);
	}
	c->arena = arena;
	c->opt_level = 2; // default matches the CLI's -O2

	LLVMInitializeNativeTarget();
	LLVMInitializeNativeAsmPrinter();
	LLVMInitializeNativeAsmParser();

	c->context = LLVMContextCreate();
	c->module = LLVMModuleCreateWithNameInContext(module_name, c->context);
	c->builder = LLVMCreateBuilderInContext(c->context);

	// Layout is needed while declaring enum payloads, before optimization.
	// Use the same native target for layout, verification and object emission.
	char *default_triple = LLVMGetDefaultTargetTriple();
	char *triple = native_triple(c,default_triple);
	char *cpu = LLVMGetHostCPUName(), *features = LLVMGetHostCPUFeatures();
	LLVMTargetRef target;
	char *target_error = NULL;
	if (LLVMGetTargetFromTriple(triple, &target, &target_error)) {
		kdiag_error_at(KAWA_E_SEMANTIC, "<kawa>", NULL, 0, "target selection failed: %s", target_error);
		LLVMDisposeMessage(target_error);
		exit(1);
	}
	c->target_machine = LLVMCreateTargetMachine(target, triple, cpu, features,
		LLVMCodeGenLevelAggressive, LLVMRelocDefault, LLVMCodeModelDefault);
	c->target_data = LLVMCreateTargetDataLayout(c->target_machine);
	LLVMSetModuleDataLayout(c->module, c->target_data);
	LLVMSetTarget(c->module, triple);
	LLVMDisposeMessage(default_triple);
	LLVMDisposeMessage(cpu);
	LLVMDisposeMessage(features);

	init_metadata(c);

	LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
	LLVMTypeRef i64 = LLVMInt64TypeInContext(c->context);
	LLVMTypeRef void_t = LLVMVoidTypeInContext(c->context);

	// Malloc / Realloc / Free. Declared as plain `nounwind` (set per-call by
	// the runtime), vararg = 0 because we always pass i64 for size.
	c->malloc_type = LLVMFunctionType(i8ptr, &i64, 1, 0);
	c->malloc_fn = LLVMAddFunction(c->module, "malloc", c->malloc_type);

	LLVMTypeRef realloc_args[] = {i8ptr, i64};
	c->realloc_type = LLVMFunctionType(i8ptr, realloc_args, 2, 0);
	c->realloc_fn = LLVMAddFunction(c->module, "realloc", c->realloc_type);

	c->free_type = LLVMFunctionType(void_t, &i8ptr, 1, 0);
	c->free_fn = LLVMAddFunction(c->module, "free", c->free_type);

	// Coroutine intrinsics.
	LLVMTypeRef token = LLVMTokenTypeInContext(c->context);
	LLVMTypeRef i32 = LLVMInt32TypeInContext(c->context);

	// llvm.coro.id
	LLVMTypeRef id_args[] = {i32, i8ptr, i8ptr, i8ptr};
	c->coro_id_type = LLVMFunctionType(token, id_args, 4, 0);
	c->coro_id = LLVMAddFunction(c->module, "llvm.coro.id", c->coro_id_type);

	// llvm.coro.begin
	LLVMTypeRef begin_args[] = {token, i8ptr};
	c->coro_begin_type = LLVMFunctionType(i8ptr, begin_args, 2, 0);
	c->coro_begin =
		LLVMAddFunction(c->module, "llvm.coro.begin", c->coro_begin_type);

	// llvm.coro.size
	c->coro_size_type = LLVMFunctionType(i64, NULL, 0, 0);
	c->coro_size =
		LLVMAddFunction(c->module, "llvm.coro.size.i64", c->coro_size_type);

	// llvm.coro.suspend
	LLVMTypeRef susp_args[] = {token, LLVMInt1TypeInContext(c->context)};
	c->coro_suspend_type =
		LLVMFunctionType(LLVMInt8TypeInContext(c->context), susp_args, 2, 0);
	c->coro_suspend =
		LLVMAddFunction(c->module, "llvm.coro.suspend", c->coro_suspend_type);

	// llvm.coro.save
	c->coro_save_type = LLVMFunctionType(token, &i8ptr, 1, 0);
	c->coro_save =
		LLVMAddFunction(c->module, "llvm.coro.save", c->coro_save_type);

	// llvm.coro.end -- LLVM 21 expects (ptr, i1, token). The token argument
	// is unused by the intrinsic itself but is part of the signature; pass
	// `null` from the call sites.
	LLVMTypeRef end_args[] = {i8ptr, LLVMInt1TypeInContext(c->context),
							  token};
	c->coro_end_type =
		LLVMFunctionType(LLVMInt1TypeInContext(c->context), end_args, 3, 0);
	c->coro_end = LLVMAddFunction(c->module, "llvm.coro.end", c->coro_end_type);

	// llvm.coro.promise
	LLVMTypeRef prom_args[] = {i8ptr, i32, LLVMInt1TypeInContext(c->context)};
	c->coro_promise_type = LLVMFunctionType(i8ptr, prom_args, 3, 0);
	c->coro_promise =
		LLVMAddFunction(c->module, "llvm.coro.promise", c->coro_promise_type);

	// llvm.coro.alloc
	c->coro_alloc_type =
		LLVMFunctionType(LLVMInt1TypeInContext(c->context), &token, 1, 0);
	c->coro_alloc =
		LLVMAddFunction(c->module, "llvm.coro.alloc", c->coro_alloc_type);

	// llvm.coro.done
	LLVMTypeRef done_args[] = {i8ptr};
	c->coro_done_type =
		LLVMFunctionType(LLVMInt1TypeInContext(c->context), done_args, 1, 0);
	c->coro_done =
		LLVMAddFunction(c->module, "llvm.coro.done", c->coro_done_type);

	// llvm.coro.resume -- eagerly declared so sip() doesn't rebuild the type
	// on every call (each rebuild would defeat caching via LLVMTypeRef identity).
	{
		LLVMTypeRef resume_args[] = {i8ptr};
		c->coro_resume_type = LLVMFunctionType(void_t, resume_args, 1, 0);
		c->coro_resume =
			LLVMAddFunction(c->module, "llvm.coro.resume", c->coro_resume_type);
	}

	// Promise layout index for the int "yield" slot. 8 = byte offset of
	// the second word-sized slot in the promise; matches the layout used by
	// drip/brew promise storage.
	c->drip_promise_index = 8;
	c->brew_promise_index = 8;
}
