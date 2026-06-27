#include "codegen_internal.h"

void kawa_init(KawaCompiler *c, const char *module_name) {
	LLVMInitializeNativeTarget();
	LLVMInitializeNativeAsmPrinter();
	LLVMInitializeNativeAsmParser();

	c->context = LLVMContextCreate();
	c->module = LLVMModuleCreateWithNameInContext(module_name, c->context);
	c->builder = LLVMCreateBuilderInContext(c->context);
	c->scope_stack = NULL;
	c->coro_resume = NULL;
	c->lambda_counter = 0;
	c->in_coroutine = 0;

	init_metadata(c);

	LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
	LLVMTypeRef i64 = LLVMInt64TypeInContext(c->context);
	LLVMTypeRef void_t = LLVMVoidTypeInContext(c->context);

	// Malloc
	c->malloc_type = LLVMFunctionType(i8ptr, &i64, 1, 0);
	c->malloc_fn = LLVMAddFunction(c->module, "malloc", c->malloc_type);
	// [FIX] Attributes removed for stability

	// Realloc
	LLVMTypeRef realloc_args[] = {i8ptr, i64};
	c->realloc_type = LLVMFunctionType(i8ptr, realloc_args, 2, 0);
	c->realloc_fn = LLVMAddFunction(c->module, "realloc", c->realloc_type);

	// Free
	c->free_type = LLVMFunctionType(void_t, &i8ptr, 1, 0);
	c->free_fn = LLVMAddFunction(c->module, "free", c->free_type);
	// [FIX] Attributes removed for stability

	// Coroutines (Intrinsics)
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

	// llvm.coro.end
	LLVMTypeRef end_args[] = {i8ptr, LLVMInt1TypeInContext(c->context)};
	c->coro_end_type =
		LLVMFunctionType(LLVMInt1TypeInContext(c->context), end_args, 2, 0);
	c->coro_end = LLVMAddFunction(c->module, "llvm.coro.end", c->coro_end_type);

	// llvm.coro.free
	c->coro_free_type = LLVMFunctionType(i8ptr, begin_args, 2, 0);
	c->coro_free =
		LLVMAddFunction(c->module, "llvm.coro.free", c->coro_free_type);

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

	c->drip_promise_index = 8;
	c->brew_promise_index = 8;
}
