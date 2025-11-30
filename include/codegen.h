#ifndef KAWA_CODEGEN_H
#define KAWA_CODEGEN_H

#include "ast.h"
#include "lexer.h"
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>

typedef struct Scope {
	char *name;
	LLVMValueRef val;
	LLVMTypeRef type;
	struct ASTNode *node;
	struct Scope *next;
} Scope;

typedef struct {
	LLVMModuleRef module;
	LLVMBuilderRef builder;
	LLVMContextRef context;

	LLVMValueRef current_func;
	LLVMTypeRef current_ret_type;

	// Track the current coroutine handle & promise for use in 'drop'
	LLVMValueRef current_coro_hdl;
	LLVMValueRef current_promise_ptr;

	Scope *scope_stack;
	int lambda_counter;

	int in_coroutine;
	LLVMBasicBlockRef coro_cleanup_block;
	LLVMBasicBlockRef coro_suspend_block;
	int brew_promise_index;
	int drip_promise_index;

	LLVMValueRef malloc_fn;
	LLVMTypeRef malloc_type;
	LLVMValueRef realloc_fn;
	LLVMTypeRef realloc_type;
	LLVMValueRef free_fn;
	LLVMTypeRef free_type;

	LLVMValueRef coro_id;
	LLVMTypeRef coro_id_type;
	LLVMValueRef coro_begin;
	LLVMTypeRef coro_begin_type;
	LLVMValueRef coro_size;
	LLVMTypeRef coro_size_type;
	LLVMValueRef coro_save;
	LLVMTypeRef coro_save_type;
	LLVMValueRef coro_suspend;
	LLVMTypeRef coro_suspend_type;
	LLVMValueRef coro_end;
	LLVMTypeRef coro_end_type;
	LLVMValueRef coro_free;
	LLVMTypeRef coro_free_type;
	LLVMValueRef coro_resume;
	LLVMTypeRef coro_resume_type;
	LLVMValueRef coro_promise;
	LLVMTypeRef coro_promise_type;
	LLVMValueRef coro_alloc;
	LLVMTypeRef coro_alloc_type;
	LLVMValueRef coro_done;
	LLVMTypeRef coro_done_type;

} KawaCompiler;

void kawa_init(KawaCompiler *c, const char *module_name);
void kawa_compile(KawaCompiler *c, ASTNode *root);
void kawa_optimize_and_write(KawaCompiler *c, const char *filename);

#endif
