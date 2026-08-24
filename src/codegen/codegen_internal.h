#ifndef KAWA_CODEGEN_INTERNAL_H
#define KAWA_CODEGEN_INTERNAL_H

#include "arena.h"
#include "ast.h"
#include "codegen.h"
#include "timbr.h"
#include <inttypes.h>
#include <lexer.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Struct Registry ---
// Allocated from the KawaCompiler's arena.
typedef struct StructDef {
	char *name;
	LLVMTypeRef type;
	struct {
		char *name;
		LLVMTypeRef type;
	} fields[64];
	int field_count;
	struct StructDef *next;
} StructDef;

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
void register_struct(KawaCompiler *c, const char *name, LLVMTypeRef type,
					 ASTNode *fields);
int get_field_index(KawaCompiler *c, LLVMTypeRef struct_type,
					const char *field_name);
LLVMTypeRef get_field_type(KawaCompiler *c, LLVMTypeRef struct_type,
						   const char *field_name);
LLVMTypeRef get_llvm_type(KawaCompiler *c, Type *t);
int type_is_signed(KawaCompiler *c, Type *t);

// --- codegen_metadata.c ---
void init_metadata(KawaCompiler *c);
void attach_tbaa(KawaCompiler *c, LLVMValueRef instr, LLVMTypeRef type);
void set_branch_weights(KawaCompiler *c, LLVMValueRef br_instr,
						unsigned true_weight, unsigned false_weight);
void set_fast_math(LLVMValueRef instr);

// --- codegen_debug.c (-g) ---
void kawa_di_init(KawaCompiler *c, const char *source_filename);
void kawa_di_finalize(KawaCompiler *c);
LLVMMetadataRef kawa_di_subprogram(KawaCompiler *c, const char *name,
								   unsigned line, ASTNode *fn_node);
void kawa_di_attach_subprogram(KawaCompiler *c, const char *name,
							   ASTNode *fn_node);
void kawa_di_set_location(KawaCompiler *c, int line);

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
LLVMValueRef build_binop(KawaCompiler *c, ASTNode *n, LLVMValueRef l,
						 LLVMValueRef r);

// --- codegen_stmt.c ---
void codegen_stmt(KawaCompiler *c, ASTNode *n);

// --- codegen_func.c ---
void codegen_func_decl(KawaCompiler *c, ASTNode *cur,
					   const char *implicit_self_struct);

// --- codegen_coro.c ---
LLVMValueRef codegen_brew(KawaCompiler *c, ASTNode *n);
LLVMValueRef build_coro_frame(KawaCompiler *c, LLVMValueRef fn,
							  int promise_index, LLVMBasicBlockRef *cleanup_bb,
							  LLVMBasicBlockRef *suspend_bb);
void finish_coro_body(KawaCompiler *c, LLVMBasicBlockRef cleanup_bb,
					  LLVMBasicBlockRef suspend_bb);

#endif
