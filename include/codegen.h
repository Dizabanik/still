#ifndef STILL_CODEGEN_H
#define STILL_CODEGEN_H

#include "arena.h"
#include "ast.h"
#include "lexer.h"
#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

// Forward decls to avoid pulling codegen_internal.h into public API.
struct StructDef;
struct AliasDef;

typedef struct Scope {
	char *name;
	LLVMValueRef val;
	LLVMTypeRef type;
	struct ASTNode *node;
	struct Scope *orbit_scope; // lexical environment captured by a derived binding
	int used; // set by scope_find; drives the unused-variable warning
	struct Scope *all_next; // function's locals, including closed lexical scopes
	struct Scope *next;
} Scope;

// Active `filter`/`dregs` handler. A linked stack: `press` stores into the
// top frame's err slot and branches to its catch block. No unwinding --
// press is an explicit control transfer, so nounwind survives everywhere.
typedef struct FilterFrame {
	LLVMBasicBlockRef catch_bb;
	LLVMValueRef err_slot; // alloca in the enclosing function (payload type)
	Type *err_type;		 // declared payload type; NULL = legacy i32
	struct DeferFrame *defers_at_entry; // press runs only defers newer than this
	struct FilterFrame *next;
} FilterFrame;

// Deferred statements (`defer stmt;`): pushed at the defer site, emitted in
// reverse order when the enclosing function returns.
typedef struct DeferFrame {
	LLVMValueRef memory_slot; /* owner drop or stable unpin; never captured by value */
	int memory_unpin;
	Type *memory_type; /* aggregate drop layout; NULL for an owner or pin */
	struct ASTNode *stmt;
	struct {
		char *name;
		LLVMValueRef slot;
		LLVMTypeRef type;
		struct ASTNode *node;
	} captures[16];
	int capture_count;
	struct DeferFrame *next;
} DeferFrame;

typedef struct StableFrame {
	LLVMValueRef slot, reference, data;
	struct StableFrame *next;
} StableFrame;

typedef struct FunctionSignature {
	LLVMValueRef function;
	ASTNode *declaration;
	Type **parameters;
	Type *return_type;
	struct FunctionSignature *next;
} FunctionSignature;

typedef struct {
	LLVMModuleRef module;
	LLVMBuilderRef builder;
	LLVMContextRef context;
	LLVMTargetMachineRef target_machine;
	LLVMTargetDataRef target_data;
	int uses_memory;
	int memory_metrics;
    int check_only;
    const char *optimization_report;
    const char *executable_path;
    struct StillOptimizationSite *optimization_sites;
    unsigned optimization_site_count;
	StableFrame *stable_stack;
	struct WkyValueLayoutCache *value_layouts;
	struct ASTNode *builtin_tagged_types;
	FunctionSignature *function_signatures;
	unsigned fp_permissions;

	// Debug build enables source-level debug information. Bounds checks
	// are controlled separately and default to safe in every build.
	int debug_build;
	// >0 while emitting an `unchecked { ... }` block: bounds checks are
	// suppressed regardless of debug_build. Nested blocks just count.
	int unchecked_depth;
	int unsafe_depth;
	// Statement list currently being emitted.
	struct ASTNode *cur_stmt_list;
	// Generics (IDEAS 2.2): declared type parameters of the generic fn
	// currently being instantiated ("T" -> concrete Type*), plus the
	// registry of generic fn ASTs awaiting instantiation.
	char *generic_param_names[8];
	Type *generic_param_types[8];
	int generic_param_count;
	int generic_instantiating;
	struct ASTNode *generic_fns[64];
	int generic_fn_count;
	struct GenericImplMethod {
		char *struct_name;
		char *type_param;
		struct ASTNode *fn_decl;
	} generic_impl_methods[64];
	int generic_impl_method_count;

	// Optimization level from -O0..-O3 (default 2). Selects the pass
	// pipeline in still_optimize_and_write; -O3 adds aggressive vectorize
	// + loop unrolling on top of default<O2>.
	int opt_level;

	// Test mode (--test): @main runs #[test] functions instead of user
	// main. Collected during the program walk, runner synthesized after.
	int test_mode;
	int main_argv_views;
	Type *main_ret_ast;
	int uses_print;
	ASTNode *test_fns[256];
	struct ASTNode *program_root; // for comptime fn lookup
	int test_fn_count;

	// Lazily-declared noreturn trap: wky_trap(msg, file, line).
	LLVMValueRef trap_fn;

	// Debug info (-g). DIBuilder + CU/file/subprogram metadata; NULL when
	// debug info is off so every emission site can just check di_builder.
	LLVMDIBuilderRef di_builder;
	LLVMMetadataRef di_cu;
	LLVMMetadataRef di_file;
	// Current source line for instruction locations (updated per statement).
	int di_line;
	int source_line; // expanded source line, retained even without debug info

	// Source filename + full source text for diagnostics (owned by the
	// driver). source_text backs still_diag_line(), which pulls the offending
	// line out of the program for caret rendering.
	const char *source_filename;
	const char *source_text;
	int source_len;

	// Emit at most one unreachable-statement warning per function body.
	int warned_unreachable;

	// Function overloading: functions sharing a bare name (distinct param
	// type lists) are emitted under `name__t1_t2` symbols; call sites
	// resolve through the overload set by argument type. Single-name fns
	// keep their plain symbol -- zero cost for non-overloaded code.
	struct OverloadFn {
		char *bare;     // declared name ("add")
		char *mangled;  // emission symbol ("add__i32_i32")
		ASTNode *decl;  // NODE_FUNC_DECL
		Type **params;  // declared param types
		int nparams;
	} overload_fns[64];
	int overload_fn_count;
	// Bare names that belong to an overload set (>1 entry), for quick checks.
	char *overload_names[32];
	int overload_name_count;
	// While emitting a specific overload, its index (for recursive calls).
	int overloads_active;

	// Every impl method, pre-registered as "Struct__method" before any
	// function body is emitted -- so operator/index overloads inside a body
	// can check existence without emission-order luck.
	char *impl_methods[256];
	int impl_method_count;

	// Arena shared with the parser. Owns all codegen-side heap-ish memory:
	// Scope frames, StructDef/AliasDef nodes, duplicated names. Lifetime
	// is the same as the compiler (typically program lifetime).
	Arena *arena;

	LLVMValueRef current_func;
	LLVMTypeRef current_ret_type;
	Type *current_ret_node_type;

	// Track the current coroutine handle & promise for use in 'drop'
	LLVMValueRef current_coro_hdl;
	LLVMValueRef current_coro_id;
	LLVMValueRef current_promise_ptr;

	Scope *scope_stack;
	Scope *function_locals;
	// Head of the scope chain at FILE level (globals only). Coroutine
	// bodies start from this snapshot so tasks can read globals without
	// inheriting the spawning function's locals.
	Scope *global_scope;
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
		DeferFrame *defers_at_entry;
		struct LoopTargets *next;
	} *loop_stack;

	int in_coroutine;
	LLVMBasicBlockRef coro_cleanup_block;
	LLVMBasicBlockRef coro_finish_block;
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
	LLVMValueRef coro_destroy, coro_free;
	LLVMTypeRef coro_destroy_type, coro_free_type;

	// Optimization & code generation flags (IDEAS 4.1, 4.3, 4.4)
	int enable_lto;
	const char *pgo_gen;
	const char *pgo_use;
	int bounds_check_mode; // 0/2 = safe, 1 = always, -1 = never
	int emit_hash;

	// Loop range tracking for bounds check elimination
	struct LoopRange {
		const char *var_name;
		long long upper_bound;
		int is_inclusive;
		struct LoopRange *parent;
	} *loop_ranges;

} StillCompiler;

// Initialize a compiler. The caller owns `arena` and must keep it alive for
// the lifetime of `c`. The arena is used for all codegen-side heap
// allocations (scope frames, struct/alias registries, duplicated names).
void still_init(StillCompiler *c, const char *module_name, Arena *arena);
// Enable debug-build instrumentation (bounds traps). Call after still_init,
// before still_compile.
void still_set_debug(StillCompiler *c, int debug);
// Source filename used in trap diagnostics.
void still_set_source_file(StillCompiler *c, const char *filename);
void still_compile(StillCompiler *c, ASTNode *root);
// Comptime: evaluate `n` as an integer constant, interpreting calls to pure
// functions with the tree-walking evaluator (recursion-guarded). Returns an
// LLVMConstantRef of the requested width or NULL when not foldable.
LLVMValueRef wky_comptime_eval(StillCompiler *c, ASTNode *n,
								unsigned result_width, int *out_signed);
void still_optimize_and_write(StillCompiler *c, const char *filename);

#endif
