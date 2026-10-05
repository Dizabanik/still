#ifndef STILL_CODEGEN_INTERNAL_H
#define STILL_CODEGEN_INTERNAL_H

#include "arena.h"
#include "ast.h"
#include "codegen.h"
#include "diag.h"
#include "timbr.h"
#include <inttypes.h>
#include <lexer.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <llvm-c/Support.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The context-free C APIs use LLVM's global context, even when the function
// belongs to our private context. Mixing them leaves non-canonical types in
// optimization passes (e.g. a bitcast from i64 to a different i64 object).
static inline LLVMBasicBlockRef wky_append_block(LLVMValueRef fn, const char *name) {
	return LLVMAppendBasicBlockInContext(LLVMGetTypeContext(LLVMTypeOf(fn)), fn, name);
}

// --- Diagnostics helpers ---
// Coded errors anchored to the AST node's source line (rendered through
// still_diag_line from the driver-provided source text). `still_error_named` anchors the
// caret to a whole-word name on that line; plain still_diag_* still work for
// non-AST contexts.
#define still_error(code, node, ...)                                                   \
	still_diag_error_node(code, c->source_filename ? c->source_filename : "<wky>",  \
				   node,                                                        \
				   __VA_ARGS__)
#define still_error_named(code, node, name_, ...)                                           \
	still_diag_error_named(code,                                                     \
					  c->source_filename ? c->source_filename : "<wky>",       \
					  (node) && (node)->line > 0 ? (node)->line : 0,            \
					  name_, __VA_ARGS__)
#define still_warn(code, node, ...)                                                  \
	still_diag_warn_node(code, c->source_filename ? c->source_filename : "<wky>",  \
				  node,                                                         \
				  __VA_ARGS__)

// --- Struct Registry ---
// Allocated from the StillCompiler's arena.
typedef struct StructDef {
	char *name;
	LLVMTypeRef type;
	struct {
		char *name;
		LLVMTypeRef type;
		Type *ast_type;
		ASTNode *default_expr; // field default (`f32 zoom = 1.0;`), or NULL
	} fields[64];
	int field_count;
	struct StructDef *next;
} StructDef;
typedef struct {
	unsigned *indices;
	uint64_t provided;
	StructInitItem *spread;
} LiteralPlan;
LiteralPlan wky_literal_plan(StillCompiler *c,ASTNode *n,LLVMTypeRef type);
void wky_literal_context(StillCompiler *c,ASTNode *n,Type *type);
LLVMValueRef wky_codegen_literal(StillCompiler *c,ASTNode *n);

// --- Alias Registry ---
// Allocated from the StillCompiler's arena.
typedef struct AliasDef {
	char *name;
	Type *target;
	struct AliasDef *next;
} AliasDef;

// --- codegen_types.c ---
void register_alias(StillCompiler *c, const char *name, Type *target);
Type *resolve_alias_type(StillCompiler *c, const char *name);
Type *wky_resolve_type(StillCompiler *c, Type *type);
Type *wky_concrete_type(StillCompiler *c, Type *type);
void register_struct(StillCompiler *c, const char *name, LLVMTypeRef type,
					 ASTNode *fields);
Type *index_base_struct_type(StillCompiler *c, ASTNode *obj);
int get_field_index(StillCompiler *c, LLVMTypeRef struct_type,
					const char *field_name);
LLVMTypeRef get_field_type(StillCompiler *c, LLVMTypeRef struct_type,
						   const char *field_name);
// Struct-embedding promotion (IDEAS 3): locate `field` inside a direct
// embedded-struct field of `struct_type`. Returns 1 and fills both indices
// on success.
int try_promoted_field(StillCompiler *c, LLVMTypeRef struct_type,
					   const char *field, int *out_mid, int *out_field);
// Direct-field existence check (promotion never shadows these).
int has_direct_field(StillCompiler *c, LLVMTypeRef struct_type,
					 const char *field);
// Name of the field at `index` in `struct_type`.
const char *sd_field_name(StillCompiler *c, LLVMTypeRef struct_type,
						  int index);
// Registry lookup exposed for method promotion (codegen_expr).
StructDef *find_struct_def_pub(StillCompiler *c, LLVMTypeRef struct_type);
LLVMTypeRef get_llvm_type(StillCompiler *c, Type *t);
int type_is_signed(StillCompiler *c, Type *t);

// --- function overloading (codegen_types.c) ---
int wky_types_same(Type *a, Type *b);
void collect_overloads(StillCompiler *c, ASTNode *root);
void collect_impl_methods(StillCompiler *c, ASTNode *root);
int impl_has_method(StillCompiler *c, const char *struct_name,
					const char *method);
int is_overloaded_name(StillCompiler *c, const char *bare);
const char *resolve_overload(StillCompiler *c, ASTNode *call,
							 const char *bare, ASTNode *args);
int resolve_overload_arg_count(ASTNode *call);

// True for every floating-point kind (f16/bf16/f32/f64). Centralizes the
// kind check so new FP widths can't miss a coercion/promotion site.
static inline int is_fp_kind(LLVMTypeKind k) {
	return k == LLVMHalfTypeKind || k == LLVMBFloatTypeKind ||
		   k == LLVMFloatTypeKind || k == LLVMDoubleTypeKind;
}

// Shared integer folder (codegen_scope.c; used by comptime too). Returns 1
// and writes *out on success. Wrap/UB semantics match build_int_binop.
int fold_int_binop(int tok, unsigned long long a, unsigned long long b,
				   int lhs_signed, int rhs_signed, unsigned width, unsigned long long *out);

// --- codegen_intrinsics.c (built-in reductions) ---
// Emits `internal <acc> @wky.<op>(ptr data, i64 len[, ptr d2, i64 len2])`
// with a canonical vectorizer-perfect reduction loop. op is one of
// "sum"/"max"/"min"/"dot"; arity 1 except dot=2.
LLVMValueRef wky_emit_reduction_fn(StillCompiler *c, const char *op,
									Type *elem_ast, int arity);

// Saturating qadd/qsub/qmul for i8/u8/i16/u16 -> llvm.*.sat call; NULL when
// the type or op is out of scope (wider ints keep normal wrap semantics).
LLVMValueRef wky_build_sat_op(StillCompiler *c, const char *op,
							   Type *elem_ast, LLVMValueRef l,
							   LLVMValueRef r);

// Lazily-defined trap (bounds checks etc.); declared here for channel
// blocking-failure traps in codegen_expr.
LLVMValueRef get_or_declare_trap_fn(StillCompiler *c);

// --- codegen_metadata.c ---
void init_metadata(StillCompiler *c);
void attach_tbaa(StillCompiler *c, LLVMValueRef instr, LLVMTypeRef type);
void set_branch_weights(StillCompiler *c, LLVMValueRef br_instr,
						unsigned true_weight, unsigned false_weight);
void set_fast_math(StillCompiler *c, LLVMValueRef instr);
LLVMValueRef wky_integer_op(StillCompiler *c, ASTNode *n, int op,
                            LLVMValueRef left, LLVMValueRef right,
                            int is_signed, int policy);
LLVMValueRef wky_numeric_builtin(StillCompiler *c, ASTNode *n, const char *name);
void wky_check_conversion(StillCompiler *c, LLVMValueRef value, Type *source,
                           LLVMTypeRef destination, Type *dest_ast);

// --- codegen_debug.c (-g) ---
void still_di_init(StillCompiler *c, const char *source_filename);
void still_di_finalize(StillCompiler *c);
LLVMMetadataRef still_di_subprogram(StillCompiler *c, const char *name,
								   unsigned line, ASTNode *fn_node);
void still_di_attach_subprogram(StillCompiler *c, const char *name,
							   ASTNode *fn_node);
void still_di_set_location(StillCompiler *c, int line);

void emit_check_or_trap(StillCompiler *c, ASTNode *n, LLVMValueRef ok, const char *message);

// Managed memory has one lowering boundary and an opaque runtime descriptor.
int wky_is_managed(Type *t);
int wky_is_owner(Type *t);
LLVMValueRef wky_memory_layout(StillCompiler *c, Type *type);
int wky_value_contains_arena(StillCompiler *c, Type *type);
int wky_expr_may_invalidate(StillCompiler *c, ASTNode *node);
void wky_memory_defer_value(StillCompiler *c, LLVMValueRef slot, Type *type);
void wky_memory_cleanup_value(StillCompiler *c, LLVMValueRef slot, Type *type);
void wky_memory_store_value(StillCompiler *c, LLVMValueRef slot, LLVMValueRef value,
                             Type *type, LLVMValueRef container);
int wky_contains_managed(StillCompiler *c, Type *t, int owners_only);
Type *wky_expr_type(StillCompiler *c, ASTNode *n);
LLVMValueRef wky_memory_builtin(StillCompiler *c, ASTNode *n, const char *name);
LLVMValueRef wky_memory_binary(StillCompiler *c, ASTNode *n);
LLVMValueRef wky_memory_address(StillCompiler *c, ASTNode *n, ASTNode *base,
                                ASTNode *index, LLVMTypeRef *out_type, LLVMValueRef *container);
LLVMValueRef wky_memory_lvalue(StillCompiler *c, ASTNode *n, LLVMTypeRef *out_type,
                               LLVMValueRef *container);
LLVMValueRef wky_memory_write_address(StillCompiler *c, LLVMValueRef slot,
                                      LLVMTypeRef type, LLVMValueRef container);
void wky_memory_store_owner(StillCompiler *c, LLVMValueRef slot,
                             LLVMValueRef value, LLVMValueRef container);
void wky_check_value_type(StillCompiler *c, ASTNode *n, Type *type);
LLVMValueRef wky_memory_value(StillCompiler *c, ASTNode *n);
void wky_memory_cleanup(StillCompiler *c, LLVMValueRef slot, int unpin);
void wky_memory_defer(StillCompiler *c, LLVMValueRef slot, int unpin);
void wky_memory_stable(StillCompiler *c, ASTNode *n);
void wky_verify_ownership(StillCompiler *c, ASTNode *function);
LLVMValueRef wky_generic_function(StillCompiler *c, ASTNode *call, const char *name);

// --- codegen_scope.c ---
char *get_var_path(StillCompiler *c, const char *s);
const char *resolve_type_name(StillCompiler *c, ASTNode *n);
void scope_push(StillCompiler *c, const char *name, LLVMValueRef val,
				LLVMTypeRef type, ASTNode *node);
Scope *scope_find(StillCompiler *c, const char *name);
LLVMValueRef create_entry_block_alloca(StillCompiler *c, LLVMTypeRef type,
									   const char *name);

// Lvalue resolution: returns the ADDRESS of the storage denoted by `n`
// (alloca for locals, GEP for fields, pointer value for derefs) and, via
// `out_type`, the type of the VALUE stored there.
LLVMValueRef get_address(StillCompiler *c, ASTNode *n, LLVMTypeRef *out_type);

// Rvalue evaluation for lvalue-shaped nodes (var refs, member access,
// derefs, &:address-of). Loads exactly once from the resolved address.
LLVMValueRef value_of_lvalue(StillCompiler *c, ASTNode *n);

// Numeric/pointer coercion between an existing LLVM value and a destination
// type. AST types (when available) drive signedness; NULL falls back to
// unsigned semantics.
LLVMValueRef coerce_value(StillCompiler *c, LLVMValueRef v, Type *src_ast,
						  LLVMTypeRef dst, Type *dst_ast);

// Normalize a condition value to i1 (int != 0, ptr != null).
LLVMValueRef cond_to_bool(StillCompiler *c, LLVMValueRef cond);

// Constant global-initializer support (codegen_scope.c). Decides whether a
// top-level initializer can be folded into an LLVM constant, and does so.
int global_init_is_constant(StillCompiler *c, ASTNode *n);
LLVMValueRef const_eval_global_init(StillCompiler *c, ASTNode *n,
									LLVMTypeRef dst, Type *dst_ast);
// Fold an expression to a compile-time i64 (literals, consts/enum members,
// const arithmetic). Returns 0 when the expr isn't a compile-time integer.
int const_eval_i64(StillCompiler *c, ASTNode *n, long long *out);

// --- codegen_expr.c ---
void trigger_orbit_updates(StillCompiler *c, ASTNode *origin_node);
LLVMValueRef codegen_expr(StillCompiler *c, ASTNode *n);
LLVMValueRef codegen_index_overload(StillCompiler *c, ASTNode *n);
LLVMValueRef build_binop(StillCompiler *c, ASTNode *n, LLVMValueRef l,
						 LLVMValueRef r);

// --- codegen_stmt.c ---
void codegen_stmt(StillCompiler *c, ASTNode *n);

// --- codegen_func.c ---
void wky_verify_safety(StillCompiler *c, LLVMTargetMachineRef machine);
void wky_verify_effects(StillCompiler *c);
void codegen_func_decl(StillCompiler *c, ASTNode *cur,
					   const char *implicit_self_struct);

// --- codegen_coro.c ---
LLVMValueRef codegen_brew(StillCompiler *c, ASTNode *n);
LLVMValueRef build_coro_frame(StillCompiler *c, LLVMValueRef fn,
							  int promise_index, LLVMBasicBlockRef *cleanup_bb,
							  LLVMBasicBlockRef *suspend_bb);

// Same, with heap elision: when use_stack_frame is set the coroutine frame
// is allocated in the caller's stack (exact size via llvm.coro.size) and no
// malloc path exists in the emitted code.
LLVMValueRef build_coro_frame_ex(StillCompiler *c, LLVMValueRef fn,
								 int promise_index,
								 LLVMBasicBlockRef *cleanup_bb,
								 LLVMBasicBlockRef *suspend_bb,
								 int use_stack_frame);
void finish_coro_body(StillCompiler *c, LLVMBasicBlockRef cleanup_bb,
					  LLVMBasicBlockRef suspend_bb);

// --- Tagged Unions, Match & Defer ---
ASTNode *find_enum_decl(StillCompiler *c, const char *name);
EnumVariant *find_enum_variant(ASTNode *enum_decl, const char *variant_name);
int get_enum_max_payload_words(StillCompiler *c, const char *name);
void run_defer_frame(StillCompiler *c, DeferFrame *d);
void codegen_match(StillCompiler *c, ASTNode *n, LLVMValueRef res_slot, LLVMTypeRef res_type);

// --- Whisky Fast Runtime Declarations ---
LLVMValueRef declare_wky_runtime_fn(StillCompiler *c, const char *name);

typedef struct StillOptimizationSnapshot StillOptimizationSnapshot;
LLVMMetadataRef still_report_site(StillCompiler *c,ASTNode *node,const char *kind,
                                 const char *detail,const char *omission);
void still_report_attach(StillCompiler *c,LLVMValueRef instruction,LLVMMetadataRef site);
StillOptimizationSnapshot *still_report_snapshot(StillCompiler *c);
void still_report_write(StillCompiler *c,StillOptimizationSnapshot *before,const char *pipeline);

#endif
