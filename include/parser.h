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
	} decls[256];
	int decl_count;

	// Function signatures seen so far (name -> declared return type).
	// Lets a `let x = f(...)` infer the call's type at parse time; NULL
	// ret entries (void/unknown) fall back to the old behavior.
	struct {
		char *name;
		Type *ret;
	} fn_sigs[128];
	int fn_sig_count;
} Parser;

void parser_init(Parser *p, Lexer *l, Arena *a);
ASTNode *parse_program(Parser *p);

#endif
