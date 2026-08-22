#ifndef KAWA_CODEGEN_H
#define KAWA_CODEGEN_H

#include "arena.h"
#include "ast.h"
#include "lexer.h"
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>

// Forward decls to avoid pulling codegen_internal.h into public API.
struct StructDef;
struct AliasDef;

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

	// Arena shared with the parser. Owns all codegen-side heap-ish memory:
	// Scope frames, StructDef/AliasDef nodes, duplicated names. Lifetime
	// is the same as the compiler (typically program lifetime).
	Arena *arena;

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

	// Type registries (heads of singly-linked lists). Nodes are arena-owned.
	struct StructDef *struct_defs;
	struct AliasDef *alias_defs;

	// TBAA metadata cache, owned by this compiler's context.
	// Initialized lazily by init_metadata().
	LLVMMetadataRef tbaa_root;
	LLVMMetadataRef tbaa_scalar;
	LLVMMetadataRef tbaa_ptr;
	LLVMMetadataRef tbaa_int;

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
	LLVMValueRef coro_resume;
	LLVMTypeRef coro_resume_type;
	LLVMValueRef coro_promise;
	LLVMTypeRef coro_promise_type;
	LLVMValueRef coro_alloc;
	LLVMTypeRef coro_alloc_type;
	LLVMValueRef coro_done;
	LLVMTypeRef coro_done_type;

} KawaCompiler;

// Initialize a compiler. The caller owns `arena` and must keep it alive for
// the lifetime of `c`. The arena is used for all codegen-side heap
// allocations (scope frames, struct/alias registries, duplicated names).
void kawa_init(KawaCompiler *c, const char *module_name, Arena *arena);
void kawa_compile(KawaCompiler *c, ASTNode *root);
void kawa_optimize_and_write(KawaCompiler *c, const char *filename);

#endif
