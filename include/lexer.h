#ifndef KAWA_LEXER_H
#define KAWA_LEXER_H

#include "arena.h"
#include <stddef.h>

typedef enum {
	TOK_EOF,
	TOK_ERROR,
	// Keywords
	TOK_FN,
	TOK_LET,
	TOK_CONST,
	TOK_ORBIT,
	TOK_BREW,
	TOK_SIP,
	TOK_SET,
	TOK_RETURN,
	TOK_IF,
	TOK_ELSE,
	TOK_WHILE,
	TOK_PURE,
	TOK_STRUCT,
	TOK_IMPL,
	TOK_BATCH,
	TOK_DEFER,
	TOK_DRIP,
	TOK_DROP,
	TOK_FILTER,
	TOK_DREGS,
	TOK_PRESS,
	TOK_ALIAS,
	TOK_GRIND,
	TOK_IMPORT,
	TOK_SIZEOF,
	TOK_MUT,
	// Primitive Types
	TOK_VOID,
	TOK_BOOL,
	TOK_CHAR,
	TOK_I8,
	TOK_I16,
	TOK_I32,
	TOK_I64,
	TOK_U8,
	TOK_U16,
	TOK_U32,
	TOK_U64,
	TOK_F32,
	TOK_F64,
	// Literals
	TOK_IDENTIFIER,
	TOK_INT_LIT,
	TOK_FLOAT_LIT,
	TOK_STRING_LIT,
	TOK_CHAR_LIT,
	TOK_TRUE,
	TOK_FALSE,
	// Operators
	TOK_ASSIGN,
	TOK_COLON_ASSIGN, // = and :=
	TOK_PLUS,
	TOK_MINUS,
	TOK_STAR,
	TOK_SLASH,
	TOK_TILDE_EQ, // ~= (Pour)
	TOK_DOT,	  // . (Member access)
	TOK_LBRACE,
	TOK_RBRACE,
	TOK_LPAREN,
	TOK_RPAREN,
	TOK_LBRACKET,
	TOK_RBRACKET,
	TOK_COMMA,
	TOK_SEMICOLON,
	TOK_COLON,
	TOK_LANGLE,
	TOK_RANGLE, // < >
	TOK_IN,		// 'in' for loops
	TOK_ISEQ,	// ==
	TOK_LEQ,
	TOK_REQ, // <= and >=

} TokenType;

typedef struct {
	TokenType type;
	char *text;
	int line;
	size_t posA;
	size_t pos;
	size_t len;
} Token;

typedef struct {
	char *src;
	size_t pos;
	size_t len;
	int line;
	size_t posA;
	size_t posL;
	Arena *arena;
	int had_error;
	char *filename;
} Lexer;

void lexer_init(Lexer *l, char *src, Arena *a, char *filename);
Token lexer_next(Lexer *l);
Token lexer_peek(Lexer *l);

#endif
