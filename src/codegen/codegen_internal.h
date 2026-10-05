#ifndef KAWA_CODEGEN_INTERNAL_H
#define KAWA_CODEGEN_INTERNAL_H

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
static inline LLVMBasicBlockRef kawa_append_block(LLVMValueRef fn, const char *name) {
	return LLVMAppendBasicBlockInContext(LLVMGetTypeContext(LLVMTypeOf(fn)), fn, name);
}

// --- Diagnostics helpers ---
// Coded errors anchored to the AST node's source line (rendered through
// kdiag_line from the driver-provided source text). `knerr` anchors the
// caret to a whole-word name on that line; plain kdiag_* still work for
// non-AST contexts.
#define kerr(code, node, ...)                                                   \
	kdiag_error_node(code, c->source_filename ? c->source_filename : "<kawa>",  \
				   node,                                                        \
				   __VA_ARGS__)
#define knerr(code, node, name_, ...)                                           \
	kdiag_error_named(code,                                                     \
					  c->source_filename ? c->source_filename : "<kawa>",       \
					  (node) && (node)->line > 0 ? (node)->line : 0,            \
					  name_, __VA_ARGS__)
#define kwarn(code, node, ...)                                                  \
	kdiag_warn_node(code, c->source_filename ? c->source_filename : "<kawa>",  \
				  node,                                                         \
				  __VA_ARGS__)

// --- Struct Registry ---
// Allocated from the KawaCompiler's arena.
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
LiteralPlan kawa_literal_plan(KawaCompiler *c,ASTNode *n,LLVMTypeRef type);
void kawa_literal_context(KawaCompiler *c,ASTNode *n,Type *type);
LLVMValueRef kawa_codegen_literal(KawaCompiler *c,ASTNode *n);

// --- Alias Registry ---
// Allocated from the KawaCompiler's arena.
typedef struct AliasDef {
	char *name;
	Type *target;
	struct AliasDef *next;
} AliasDef;

// --- codegen_types.c ---
void register_alias(KawaCompiler *c, const char *name, Type *target);
Type *resolve_alias_type(KawaCompiler *c, const char *name);
Type *kawa_resolve_type(KawaCompiler *c, Type *type);
Type *kawa_concrete_type(KawaCompiler *c, Type *type);
void register_struct(KawaCompiler *c, const char *name, LLVMTypeRef type,
					 ASTNode *fields);
Type *index_base_struct_type(KawaCompiler *c, ASTNode *obj);
int get_field_index(KawaCompiler *c, LLVMTypeRef struct_type,
					const char *field_name);
LLVMTypeRef get_field_type(KawaCompiler *c, LLVMTypeRef struct_type,
						   const char *field_name);
// Struct-embedding promotion (IDEAS 3): locate `field` inside a direct
// embedded-struct field of `struct_type`. Returns 1 and fills both indices
// on success.
int try_promoted_field(KawaCompiler *c, LLVMTypeRef struct_type,
					   const char *field, int *out_mid, int *out_field);
// Direct-field existence check (promotion never shadows these).
int has_direct_field(KawaCompiler *c, LLVMTypeRef struct_type,
					 const char *field);
// Name of the field at `index` in `struct_type`.
const char *sd_field_name(KawaCompiler *c, LLVMTypeRef struct_type,
						  int index);
// Registry lookup exposed for method promotion (codegen_expr).
StructDef *find_struct_def_pub(KawaCompiler *c, LLVMTypeRef struct_type);
LLVMTypeRef get_llvm_type(KawaCompiler *c, Type *t);
int type_is_signed(KawaCompiler *c, Type *t);

// --- function overloading (codegen_types.c) ---
int kawa_types_same(Type *a, Type *b);
void collect_overloads(KawaCompiler *c, ASTNode *root);
void collect_impl_methods(KawaCompiler *c, ASTNode *root);
int impl_has_method(KawaCompiler *c, const char *struct_name,
					const char *method);
int is_overloaded_name(KawaCompiler *c, const char *bare);
const char *resolve_overload(KawaCompiler *c, ASTNode *call,
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
// Emits `internal <acc> @kawa.<op>(ptr data, i64 len[, ptr d2, i64 len2])`
// with a canonical vectorizer-perfect reduction loop. op is one of
// "sum"/"max"/"min"/"dot"; arity 1 except dot=2.
LLVMValueRef kawa_emit_reduction_fn(KawaCompiler *c, const char *op,
									Type *elem_ast, int arity);

// Saturating qadd/qsub/qmul for i8/u8/i16/u16 -> llvm.*.sat call; NULL when
// the type or op is out of scope (wider ints keep normal wrap semantics).
LLVMValueRef kawa_build_sat_op(KawaCompiler *c, const char *op,
							   Type *elem_ast, LLVMValueRef l,
							   LLVMValueRef r);

// Lazily-defined trap (bounds checks etc.); declared here for channel
// blocking-failure traps in codegen_expr.
LLVMValueRef get_or_declare_trap_fn(KawaCompiler *c);

// --- codegen_metadata.c ---
void init_metadata(KawaCompiler *c);
void attach_tbaa(KawaCompiler *c, LLVMValueRef instr, LLVMTypeRef type);
void set_branch_weights(KawaCompiler *c, LLVMValueRef br_instr,
						unsigned true_weight, unsigned false_weight);
void set_fast_math(KawaCompiler *c, LLVMValueRef instr);
LLVMValueRef kawa_integer_op(KawaCompiler *c, ASTNode *n, int op,
                            LLVMValueRef left, LLVMValueRef right,
                            int is_signed, int policy);
LLVMValueRef kawa_numeric_builtin(KawaCompiler *c, ASTNode *n, const char *name);
void kawa_check_conversion(KawaCompiler *c, LLVMValueRef value, Type *source,
                           LLVMTypeRef destination, Type *dest_ast);

// --- codegen_debug.c (-g) ---
void kawa_di_init(KawaCompiler *c, const char *source_filename);
void kawa_di_finalize(KawaCompiler *c);
LLVMMetadataRef kawa_di_subprogram(KawaCompiler *c, const char *name,
								   unsigned line, ASTNode *fn_node);
void kawa_di_attach_subprogram(KawaCompiler *c, const char *name,
							   ASTNode *fn_node);
void kawa_di_set_location(KawaCompiler *c, int line);

void emit_check_or_trap(KawaCompiler *c, ASTNode *n, LLVMValueRef ok, const char *message);

// Managed memory has one lowering boundary and an opaque runtime descriptor.
int kawa_is_managed(Type *t);
int kawa_is_owner(Type *t);
int kawa_contains_managed(KawaCompiler *c, Type *t, int owners_only);
Type *kawa_expr_type(KawaCompiler *c, ASTNode *n);
LLVMValueRef kawa_memory_builtin(KawaCompiler *c, ASTNode *n, const char *name);
LLVMValueRef kawa_memory_binary(KawaCompiler *c, ASTNode *n);
LLVMValueRef kawa_memory_address(KawaCompiler *c, ASTNode *n, ASTNode *base,
                                ASTNode *index, LLVMTypeRef *out_type, LLVMValueRef *container);
LLVMValueRef kawa_memory_lvalue(KawaCompiler *c, ASTNode *n, LLVMTypeRef *out_type,
                               LLVMValueRef *container);
LLVMValueRef kawa_memory_write_address(KawaCompiler *c, LLVMValueRef slot,
                                      LLVMTypeRef type, LLVMValueRef container);
void kawa_memory_store_owner(KawaCompiler *c, LLVMValueRef slot,
                             LLVMValueRef value, LLVMValueRef container);
void kawa_check_value_type(KawaCompiler *c, ASTNode *n, Type *type);
LLVMValueRef kawa_memory_value(KawaCompiler *c, ASTNode *n);
void kawa_memory_cleanup(KawaCompiler *c, LLVMValueRef slot, int unpin);
void kawa_memory_defer(KawaCompiler *c, LLVMValueRef slot, int unpin);
void kawa_memory_stable(KawaCompiler *c, ASTNode *n);
void kawa_verify_ownership(KawaCompiler *c, ASTNode *function);
LLVMValueRef kawa_generic_function(KawaCompiler *c, ASTNode *call, const char *name);

// --- codegen_scope.c ---
char *get_var_path(KawaCompiler *c, const char *s);
const char *resolve_type_name(KawaCompiler *c, ASTNode *n);
void scope_push(KawaCompiler *c, const char *name, LLVMValueRef val,
				LLVMTypeRef type, ASTNode *node);
Scope *scope_find(KawaCompiler *c, const char *name);
LLVMValueRef create_entry_block_alloca(KawaCompiler *c, LLVMTypeRef type,
									   const char *name);

// Lvalue resolution: returns the ADDRESS of the storage denoted by `n`
// (alloca for locals, GEP for fields, pointer value for derefs) and, via
// `out_type`, the type of the VALUE stored there.
LLVMValueRef get_address(KawaCompiler *c, ASTNode *n, LLVMTypeRef *out_type);

// Rvalue evaluation for lvalue-shaped nodes (var refs, member access,
// derefs, &:address-of). Loads exactly once from the resolved address.
LLVMValueRef value_of_lvalue(KawaCompiler *c, ASTNode *n);

// Numeric/pointer coercion between an existing LLVM value and a destination
// type. AST types (when available) drive signedness; NULL falls back to
// unsigned semantics.
LLVMValueRef coerce_value(KawaCompiler *c, LLVMValueRef v, Type *src_ast,
						  LLVMTypeRef dst, Type *dst_ast);

// Normalize a condition value to i1 (int != 0, ptr != null).
LLVMValueRef cond_to_bool(KawaCompiler *c, LLVMValueRef cond);

// Constant global-initializer support (codegen_scope.c). Decides whether a
// top-level initializer can be folded into an LLVM constant, and does so.
int global_init_is_constant(KawaCompiler *c, ASTNode *n);
LLVMValueRef const_eval_global_init(KawaCompiler *c, ASTNode *n,
									LLVMTypeRef dst, Type *dst_ast);
// Fold an expression to a compile-time i64 (literals, consts/enum members,
// const arithmetic). Returns 0 when the expr isn't a compile-time integer.
int const_eval_i64(KawaCompiler *c, ASTNode *n, long long *out);

// --- codegen_expr.c ---
void trigger_orbit_updates(KawaCompiler *c, ASTNode *origin_node);
LLVMValueRef codegen_expr(KawaCompiler *c, ASTNode *n);
LLVMValueRef codegen_index_overload(KawaCompiler *c, ASTNode *n);
LLVMValueRef build_binop(KawaCompiler *c, ASTNode *n, LLVMValueRef l,
						 LLVMValueRef r);

// --- codegen_stmt.c ---
void codegen_stmt(KawaCompiler *c, ASTNode *n);

// --- codegen_func.c ---
void kawa_verify_safety(KawaCompiler *c, LLVMTargetMachineRef machine);
void kawa_verify_effects(KawaCompiler *c);
void codegen_func_decl(KawaCompiler *c, ASTNode *cur,
					   const char *implicit_self_struct);

// --- codegen_coro.c ---
LLVMValueRef codegen_brew(KawaCompiler *c, ASTNode *n);
LLVMValueRef build_coro_frame(KawaCompiler *c, LLVMValueRef fn,
							  int promise_index, LLVMBasicBlockRef *cleanup_bb,
							  LLVMBasicBlockRef *suspend_bb);

// Same, with heap elision: when use_stack_frame is set the coroutine frame
// is allocated in the caller's stack (exact size via llvm.coro.size) and no
// malloc path exists in the emitted code.
LLVMValueRef build_coro_frame_ex(KawaCompiler *c, LLVMValueRef fn,
								 int promise_index,
								 LLVMBasicBlockRef *cleanup_bb,
								 LLVMBasicBlockRef *suspend_bb,
								 int use_stack_frame);
void finish_coro_body(KawaCompiler *c, LLVMBasicBlockRef cleanup_bb,
					  LLVMBasicBlockRef suspend_bb);

// --- Tagged Unions, Match & Defer ---
ASTNode *find_enum_decl(KawaCompiler *c, const char *name);
EnumVariant *find_enum_variant(ASTNode *enum_decl, const char *variant_name);
int get_enum_max_payload_words(KawaCompiler *c, const char *name);
void run_defer_frame(KawaCompiler *c, DeferFrame *d);
void codegen_match(KawaCompiler *c, ASTNode *n, LLVMValueRef res_slot, LLVMTypeRef res_type);

// --- Kawa Fast Runtime Declarations ---
LLVMValueRef declare_kawa_runtime_fn(KawaCompiler *c, const char *name);

typedef struct KawaOptimizationSnapshot KawaOptimizationSnapshot;
LLVMMetadataRef kawa_report_site(KawaCompiler *c,ASTNode *node,const char *kind,
                                 const char *detail,const char *omission);
void kawa_report_attach(KawaCompiler *c,LLVMValueRef instruction,LLVMMetadataRef site);
KawaOptimizationSnapshot *kawa_report_snapshot(KawaCompiler *c);
void kawa_report_write(KawaCompiler *c,KawaOptimizationSnapshot *before,const char *pipeline);

#endif
