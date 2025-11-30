#include "timbr.h"
#include <ctype.h>
#include <lexer.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lexer_init(Lexer *l, char *src, Arena *a, char *filename) {
	l->src = src;
	l->len = strlen(src);
	l->pos = 0;
	l->posA = 0;
	l->posL = 0;
	l->line = 1;
	l->arena = a;
	l->had_error = 0;
	l->filename = filename;
}

static inline char advance(Lexer *l) { return l->src[l->pos++]; }
static inline char peek(Lexer *l) { return l->src[l->pos]; }
static inline int is_at_end(Lexer *l) { return l->pos >= l->len; }

static Token make_token(Lexer *l, TokenType type, char *text) {
	Token t;
	t.type = type;
	t.text = text;
	t.line = l->line;
	t.posA = l->posA - l->posL;
	t.pos = l->posA;
	t.len = l->pos - l->posL;
	return t;
}

char *get_line_text_lexer(Lexer *l) {
	const char *src = l->src;
	size_t pos = l->pos;

	size_t start = l->posL;

	// Move to the end of the current line
	size_t end = pos;
	while (src[end] != '\n' && src[end] != '\0') {
		end++;
	}

	size_t len = end - start;

	char *ret = malloc(len + 1);
	memcpy(ret, src + start, len);
	ret[len] = '\0';

	return ret;
}
static Token error_token(Lexer *l, const char *msg) {
	char *lT = get_line_text_lexer(l);
	timbr_diagnostic(TIMBR_ERROR, "Lexer Error", l->filename, lT, l->line,
					 l->posA - l->posL, l->pos - l->posA + 1, msg);
	free(lT);
	l->had_error = 1;
	return make_token(l, TOK_ERROR, "Error");
}

Token lexer_next(Lexer *l) {
	while (!is_at_end(l)) {
		char c = advance(l);
		l->posA = l->pos;

		if (c == '\n') {
			l->line++;
			l->posL = l->pos;
			continue;
		}
		if (isspace(c))
			continue;

		if (c == '/' && peek(l) == '/') {
			while (peek(l) != '\n' && !is_at_end(l))
				advance(l);
			continue;
		}

		if (c == '{')
			return make_token(l, TOK_LBRACE, "{");
		if (c == '}')
			return make_token(l, TOK_RBRACE, "}");
		if (c == '(')
			return make_token(l, TOK_LPAREN, "(");
		if (c == ')')
			return make_token(l, TOK_RPAREN, ")");
		if (c == '[')
			return make_token(l, TOK_LBRACKET, "[");
		if (c == ']')
			return make_token(l, TOK_RBRACKET, "]");
		if (c == ';')
			return make_token(l, TOK_SEMICOLON, ";");
		if (c == ',')
			return make_token(l, TOK_COMMA, ",");
		if (c == '.')
			return make_token(l, TOK_DOT, ".");
		if (c == ':') {
			if (peek(l) == '=') {
				advance(l);
				return make_token(l, TOK_COLON_ASSIGN, ":=");
			}
			return make_token(l, TOK_COLON, ":");
		}
		if (c == '+')
			return make_token(l, TOK_PLUS, "+");
		if (c == '-')
			return make_token(l, TOK_MINUS, "-");
		if (c == '*')
			return make_token(l, TOK_STAR, "*");
		if (c == '/')
			return make_token(l, TOK_SLASH, "/");

		if (c == '~') {
			if (peek(l) == '=') {
				advance(l);
				return make_token(l, TOK_TILDE_EQ, "~=");
			}
			return error_token(l, "Unexpected character '~'");
		}

		if (c == '=') {
			if (peek(l) == '=') {
				advance(l);
				return make_token(l, TOK_ISEQ, "==");
			}
			return make_token(l, TOK_ASSIGN, "=");
		}
		if (c == '<') {
			if (peek(l) == '=') {
				advance(l);
				return make_token(l, TOK_LEQ, "<=");
			}
			return make_token(l, TOK_LANGLE, "<");
		}
		if (c == '>') {
			if (peek(l) == '=') {
				advance(l);
				return make_token(l, TOK_REQ, ">=");
			}
			return make_token(l, TOK_LANGLE, ">");
		}

		// Strings with Escape Sequences
		if (c == '"') {
			char buffer[1024];
			int idx = 0;
			while (peek(l) != '"' && !is_at_end(l)) {
				char ch = advance(l);
				if (ch == '\\') {
					char esc = advance(l);
					switch (esc) {
					case 'n':
						buffer[idx++] = '\n';
						break;
					case 't':
						buffer[idx++] = '\t';
						break;
					case 'r':
						buffer[idx++] = '\r';
						break;
					case '"':
						buffer[idx++] = '"';
						break;
					case '\\':
						buffer[idx++] = '\\';
						break;
					default:
						buffer[idx++] = esc;
						break;
					}
				} else {
					buffer[idx++] = ch;
				}
			}
			if (is_at_end(l))
				return error_token(l, "Unterminated string");
			advance(l);
			buffer[idx] = '\0';
			return make_token(l, TOK_STRING_LIT,
							  arena_strdup(l->arena, buffer));
		}

		// FIX: Character Literals ('x')
		if (c == '\'') {
			char val = advance(l);
			if (val == '\\') {
				char esc = advance(l);
				if (esc == 'n')
					val = '\n';
				else if (esc == 't')
					val = '\t';
				else if (esc == '0')
					val = '\0';
			}
			if (peek(l) != '\'')
				return error_token(l, "Expected closing '");
			advance(l);
			// Store as INT_LIT for simplicity in parser/codegen
			char int_str[16];
			sprintf(int_str, "%d", (int)val);
			return make_token(l, TOK_INT_LIT, arena_strdup(l->arena, int_str));
		}

		if (isdigit(c)) {
			char *start = &l->src[l->pos - 1];
			size_t len = 1;
			while (isdigit(peek(l)) || peek(l) == '.') {
				advance(l);
				len++;
			}
			char *text = arena_alloc(l->arena, len + 1);
			memcpy(text, start, len);
			text[len] = '\0';
			// Identify if float
			if (strchr(text, '.'))
				return make_token(l, TOK_FLOAT_LIT, text);
			return make_token(l, TOK_INT_LIT, text);
		}

		if (isalpha(c) || c == '$' || c == '_') {
			char *start = &l->src[l->pos - 1];
			size_t len = 1;
			while (isalnum(peek(l)) || peek(l) == '_') {
				advance(l);
				len++;
			}

			char *text = arena_alloc(l->arena, len + 1);
			memcpy(text, start, len);
			text[len] = '\0';

			TokenType type = TOK_IDENTIFIER;
			if (strcmp(text, "fn") == 0)
				type = TOK_FN;
			else if (strcmp(text, "let") == 0)
				type = TOK_LET;
			else if (strcmp(text, "const") == 0)
				type = TOK_CONST;
			else if (strcmp(text, "mut") == 0)
				type = TOK_MUT;
			else if (strcmp(text, "pure") == 0)
				type = TOK_PURE;
			else if (strcmp(text, "struct") == 0)
				type = TOK_STRUCT;
			else if (strcmp(text, "impl") == 0)
				type = TOK_IMPL;
			else if (strcmp(text, "orbit") == 0)
				type = TOK_ORBIT;
			else if (strcmp(text, "brew") == 0)
				type = TOK_BREW;
			else if (strcmp(text, "sip") == 0)
				type = TOK_SIP;
			else if (strcmp(text, "drip") == 0)
				type = TOK_DRIP;
			else if (strcmp(text, "drop") == 0)
				type = TOK_DROP;
			else if (strcmp(text, "set") == 0)
				type = TOK_SET;
			else if (strcmp(text, "batch") == 0)
				type = TOK_BATCH;
			else if (strcmp(text, "defer") == 0)
				type = TOK_DEFER;
			else if (strcmp(text, "filter") == 0)
				type = TOK_FILTER;
			else if (strcmp(text, "dregs") == 0)
				type = TOK_DREGS;
			else if (strcmp(text, "press") == 0)
				type = TOK_PRESS;
			else if (strcmp(text, "alias") == 0)
				type = TOK_ALIAS;
			else if (strcmp(text, "grind") == 0)
				type = TOK_GRIND;
			else if (strcmp(text, "import") == 0)
				type = TOK_IMPORT;
			else if (strcmp(text, "sizeof") == 0)
				type = TOK_SIZEOF;
			else if (strcmp(text, "return") == 0)
				type = TOK_RETURN;
			else if (strcmp(text, "if") == 0)
				type = TOK_IF;
			else if (strcmp(text, "else") == 0)
				type = TOK_ELSE;
			else if (strcmp(text, "while") == 0)
				type = TOK_WHILE;
			else if (strcmp(text, "in") == 0)
				type = TOK_IN;
			else if (strcmp(text, "true") == 0)
				type = TOK_TRUE;
			else if (strcmp(text, "false") == 0)
				type = TOK_FALSE;

			// Type Keywords
			else if (strcmp(text, "void") == 0)
				type = TOK_VOID;
			else if (strcmp(text, "bool") == 0)
				type = TOK_BOOL;
			else if (strcmp(text, "char") == 0)
				type = TOK_CHAR;
			else if (strcmp(text, "i8") == 0)
				type = TOK_I8;
			else if (strcmp(text, "i16") == 0)
				type = TOK_I16;
			else if (strcmp(text, "i32") == 0)
				type = TOK_I32;
			else if (strcmp(text, "i64") == 0)
				type = TOK_I64;
			else if (strcmp(text, "u8") == 0)
				type = TOK_U8;
			else if (strcmp(text, "u16") == 0)
				type = TOK_U16;
			else if (strcmp(text, "u32") == 0)
				type = TOK_U32;
			else if (strcmp(text, "u64") == 0)
				type = TOK_U64;
			else if (strcmp(text, "f32") == 0)
				type = TOK_F32;
			else if (strcmp(text, "f64") == 0)
				type = TOK_F64;

			return make_token(l, type, text);
		}

		return error_token(l, "Unexpected character");
	}
	return make_token(l, TOK_EOF, "");
}

Token lexer_peek(Lexer *l) {
	size_t save_pos = l->pos;
	int save_line = l->line;
	int save_err = l->had_error;
	Token t = lexer_next(l);
	l->pos = save_pos;
	l->line = save_line;
	l->had_error = save_err;
	return t;
}
