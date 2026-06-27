#ifndef KAWA_CODEGEN_INTERNAL_H
#define KAWA_CODEGEN_INTERNAL_H

#include "codegen.h"
#include "arena.h"
#include "ast.h"
#include "timbr.h"
#include <_inttypes.h>
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
typedef struct StructDef {
	char *name;
	LLVMTypeRef type;
	struct {
		char *name;
		LLVMTypeRef type;
	} fields[32];
	int field_count;
	struct StructDef *next;
} StructDef;

extern StructDef *struct_defs;

// --- Alias Registry ---
typedef struct AliasDef {
	char *name;
	Type *target;
	struct AliasDef *next;
} AliasDef;

extern AliasDef *alias_defs;

// --- Metadata Cache ---
extern LLVMValueRef tbaa_root;
extern LLVMValueRef tbaa_scalar;
extern LLVMValueRef tbaa_ptr;
extern LLVMValueRef tbaa_int;

// --- codegen_types.c ---
void register_alias(const char *name, Type *target);
Type *resolve_alias_type(const char *name);
void register_struct(const char *name, LLVMTypeRef type);
int get_field_index(KawaCompiler *c, LLVMTypeRef struct_type,
					const char *field_name);
LLVMTypeRef get_field_type(KawaCompiler *c, LLVMTypeRef struct_type,
						   const char *field_name);
LLVMTypeRef get_llvm_type(KawaCompiler *c, Type *t);

// --- codegen_metadata.c ---
void init_metadata(KawaCompiler *c);
void attach_tbaa(KawaCompiler *c, LLVMValueRef instr, LLVMTypeRef type);
void set_branch_weights(KawaCompiler *c, LLVMValueRef br_instr,
						int true_weight, int false_weight);
void set_fast_math(LLVMValueRef instr);
void add_loop_metadata(KawaCompiler *c, LLVMValueRef branch_instr);

// --- codegen_scope.c ---
char *get_var_path(const char *s);
const char *resolve_type_name(KawaCompiler *c, ASTNode *n);
void scope_push(KawaCompiler *c, const char *name, LLVMValueRef val,
				LLVMTypeRef type, ASTNode *node);
Scope *scope_find(KawaCompiler *c, const char *name);
LLVMValueRef get_address(KawaCompiler *c, ASTNode *n, LLVMTypeRef *out_type);
LLVMValueRef create_entry_block_alloca(KawaCompiler *c, LLVMTypeRef type,
									   const char *name);

// --- codegen_expr.c ---
void trigger_orbit_updates(KawaCompiler *c, ASTNode *origin_node);
LLVMValueRef codegen_expr(KawaCompiler *c, ASTNode *n);

// --- codegen_stmt.c ---
void codegen_stmt(KawaCompiler *c, ASTNode *n);

// --- codegen_func.c ---
void codegen_func_decl(KawaCompiler *c, ASTNode *cur,
					   const char *implicit_self_struct);

#endif
