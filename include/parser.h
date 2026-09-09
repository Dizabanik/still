#ifndef KAWA_PARSER_H
#define KAWA_PARSER_H
#include "ast.h"
#include "lexer.h"

typedef struct {
	Lexer *lexer;
	Arena *arena;
	Token cur;
	Token prev;
	int had_error;
	int panic_mode;

	// Scoped Symbol Tracking for Parsing (Simplified)
	struct {
		char *name;
		ASTNode *node;
	} decls[1024];
	int decl_count;

	// Function signatures seen so far (name -> declared return type +
	// parameter count). Lets a `let x = f(...)` infer the call's type at
	// parse time; NULL ret entries (void/unknown) fall back to the old
	// behavior. nparams drives method-call receiver injection: inject iff
	// params == explicit args + 1.
	struct {
		char *name;
		Type *ret;
		int nparams;
	} fn_sigs[512];
	int fn_sig_count;

	// Names of `#[soa]` structs (IDEAS 2.7): indexing a value of such a
	// struct type rewrites member access to per-field array access
	// (ps[i].x -> ps.x[i]), turning AoS-style code into SoA memory layout.
	char *soa_structs[64];
	int soa_count;

	// Declared struct names: `ParseErr { .code = 1 }` in expression position
	// is a struct literal when ParseErr names one of these. Without the
	// registry, `{` after an identifier has no way to know it's not a block.
	// nodes[i] is the NODE_STRUCT_DECL, kept for embedding promotion
	// (IDEAS 3): method lookup falls back through embedded fields.
	char *struct_names[128];
	ASTNode *struct_nodes[128];
	int struct_name_count;

	const char *cur_module;

	struct {
		char *name;
		char *type_param;
		ASTNode *node;
		Type *instantiations[16];
		int inst_count;
	} generic_structs[32];
	int generic_struct_count;

	struct {
		char *struct_name;
		char *type_param;
		ASTNode *node;
	} generic_impls[32];
	int generic_impl_count;

	ASTNode ***prog_tail;
} Parser;

void parser_init(Parser *p, Lexer *l, Arena *a);
ASTNode *parse_program(Parser *p);

#endif
