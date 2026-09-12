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
	TOK_EXTERN,
	TOK_ASM,
	TOK_ORBIT,
	TOK_BREW,
	TOK_SIP,
	TOK_SET,
	TOK_RETURN,
	TOK_IF,
	TOK_ELSE,
	TOK_WHILE,
	TOK_FOR,
	TOK_BREAK,
	TOK_CONTINUE,
	TOK_SWITCH,
	TOK_CASE,
	TOK_DEFAULT,
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
	TOK_ENUM,
	TOK_PUB,
	TOK_MATCH,
	// Primitive Types
	TOK_VOID,
	TOK_STR,
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
	TOK_F16,
	TOK_BF16,
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
	TOK_PLUS_EQ,  // +=
	TOK_MINUS_EQ, // -=
	TOK_STAR_EQ,  // *=
	TOK_SLASH_EQ, // /=
	TOK_TILDE_EQ, // ~= (Pour)
	TOK_ARROW,	  // -> (Arrow)
	TOK_FAT_ARROW, // => (Fat arrow)
	TOK_DOT,	  // . (Member access)
	TOK_DOTDOT, // .. (struct-literal spread / range)
	TOK_DOTDOTEQ, // ..= (inclusive range)
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
	TOK_RANGLE,	 // < >
	TOK_ANDAND,	 // &&
	TOK_OROR,	 // ||
	TOK_ATTRIBUTE, // #[...] attribute, text holds the inner content
	TOK_BANG,	 // ! (logical not)
	TOK_PERCENT, // % (modulo)
	TOK_QUESTION, // ? (ternary)
	TOK_RECV,	 // <- (channel receive / send operator half)
	TOK_IN,		 // 'in' for loops
	TOK_ISEQ,	 // ==
	TOK_NOTEQ,	 // !=
	TOK_LEQ,
	TOK_REQ,	 // <= and >=
	TOK_AMP,	 // & (address-of / bitwise and)
	TOK_PIPE,	 // |
	TOK_CARET,	 // ^
	TOK_TILDE,	 // ~ (bitwise not)
	TOK_SHL,	 // <<
	TOK_SHR,	 // >>
	TOK_AND_EQ,	 // &=
	TOK_OR_EQ,	 // |=
	TOK_XOR_EQ,	 // ^=
	TOK_SHL_EQ,	 // <<=
	TOK_SHR_EQ,  // >>=
	TOK_PERCENT_EQ, // %=
} TokenType;

typedef struct {
	TokenType type;
	char *text;
	int line;
	size_t posA;
	size_t pos;
	size_t len;
	// Float literal suffix: 0=none, 1=f16, 2=f32, 3=f64, 4=bf16.
	// Set only on TOK_FLOAT_LIT / TOK_INT_LIT with an explicit suffix.
	int float_suffix;
	size_t string_len; // decoded bytes, including embedded NULs
	const char *filename;
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
