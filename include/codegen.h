#ifndef KAWA_CODEGEN_H
#define KAWA_CODEGEN_H

#include "arena.h"
#include "ast.h"
#include "lexer.h"
#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>
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

// Active `filter`/`dregs` handler. A linked stack: `press` stores into the
// top frame's err slot and branches to its catch block. No unwinding --
// press is an explicit control transfer, so nounwind survives everywhere.
typedef struct FilterFrame {
	LLVMBasicBlockRef catch_bb;
	LLVMValueRef err_slot; // alloca'd i32 in the enclosing function
	struct DeferFrame *defers_at_entry; // press runs only defers newer than this
	struct FilterFrame *next;
} FilterFrame;

// Deferred statements (`defer stmt;`): pushed at the defer site, emitted in
// reverse order when the enclosing function returns.
typedef struct DeferFrame {
	struct ASTNode *stmt;
	struct DeferFrame *next;
} DeferFrame;

typedef struct {
	LLVMModuleRef module;
	LLVMBuilderRef builder;
	LLVMContextRef context;

	// Debug build: emit runtime checks (bounds traps). Release builds pay
	// literally nothing -- no check instructions are generated at all.
	int debug_build;

	// Lazily-declared noreturn trap: kawa_trap(msg, file, line).
	LLVMValueRef trap_fn;

	// Debug info (-g). DIBuilder + CU/file/subprogram metadata; NULL when
	// debug info is off so every emission site can just check di_builder.
	LLVMDIBuilderRef di_builder;
	LLVMMetadataRef di_cu;
	LLVMMetadataRef di_file;
	// Current source line for instruction locations (updated per statement).
	int di_line;

	// Source filename for diagnostics (owned by the driver).
	const char *source_filename;

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

	// filter/dregs handler stack (top = innermost active catch).
	FilterFrame *filter_stack;

	// defer stack (top = most recently deferred; run in reverse on return).
	DeferFrame *defer_stack;

	// Innermost loop targets for break/continue. continue_bb points at the
	// step/condition block; break_bb at the exit block.
	struct LoopTargets {
		LLVMBasicBlockRef break_bb;
		LLVMBasicBlockRef continue_bb;
		struct LoopTargets *next;
	} *loop_stack;

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
// Enable debug-build instrumentation (bounds traps). Call after kawa_init,
// before kawa_compile.
void kawa_set_debug(KawaCompiler *c, int debug);
// Source filename used in trap diagnostics.
void kawa_set_source_file(KawaCompiler *c, const char *filename);
void kawa_compile(KawaCompiler *c, ASTNode *root);
void kawa_optimize_and_write(KawaCompiler *c, const char *filename);

#endif
