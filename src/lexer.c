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
static inline char peek2(Lexer *l) { return l->src[l->pos + 1]; }
static inline char peek3(Lexer *l) { return l->src[l->pos + 2]; }
static inline int is_at_end(Lexer *l) { return l->pos >= l->len; }

// Hex digit value, or -1 if not a hex digit. Used by \xNN escapes.
static inline int hex_val(char ch) {
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	return -1;
}

static Token make_token(Lexer *l, TokenType type, char *text) {
	Token t = {0};
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

		// `#` comments: shell-script shebangs (`#!/usr/bin/env kawa run`)
		// become possible. Only a line comment -- `#` has no other meaning
		// in the grammar today.
		if (c == '#') {
			if (peek(l) == '[') {
				// Attribute: scan to the closing bracket and hand the
				// inner text to the parser as one token.
				advance(l); // consume '['
				size_t start = l->pos;
				while (!is_at_end(l) && peek(l) != ']')
					advance(l);
				size_t len = l->pos - start;
				Token t = make_token(l, TOK_ATTRIBUTE, NULL);
				t.text = arena_alloc(l->arena, len + 1);
				memcpy(t.text, l->src + start, len);
				t.text[len] = '\0';
				if (peek(l) == ']')
					advance(l); // consume ']'
				return t;
			}
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
		if (c == '+') {
			if (peek(l) == '=') {
				advance(l);
				return make_token(l, TOK_PLUS_EQ, "+=");
			}
			return make_token(l, TOK_PLUS, "+");
		}
		if (c == '-') {
			if (peek(l) == '>') {
				advance(l);
				return make_token(l, TOK_ARROW, "->");
			}
			if (peek(l) == '=') {
				advance(l);
				return make_token(l, TOK_MINUS_EQ, "-=");
			}
			return make_token(l, TOK_MINUS, "-");
		}
		if (c == '*') {
			if (peek(l) == '=') {
				advance(l);
				return make_token(l, TOK_STAR_EQ, "*=");
			}
			return make_token(l, TOK_STAR, "*");
		}
		if (c == '&') {
			if (peek(l) == '&') {
				advance(l);
				return make_token(l, TOK_ANDAND, "&&");
			}
			if (peek(l) == '=') {
				advance(l);
				return make_token(l, TOK_AND_EQ, "&=");
			}
			return make_token(l, TOK_AMP, "&");
		}
		if (c == '|') {
			if (peek(l) == '|') {
				advance(l);
				return make_token(l, TOK_OROR, "||");
			}
			if (peek(l) == '=') {
				advance(l);
				return make_token(l, TOK_OR_EQ, "|=");
			}
			return make_token(l, TOK_PIPE, "|");
		}
		if (c == '^') {
			if (peek(l) == '=') {
				advance(l);
				return make_token(l, TOK_XOR_EQ, "^=");
			}
			return make_token(l, TOK_CARET, "^");
		}
		if (c == '/') {
			if (peek(l) == '=') {
				advance(l);
				return make_token(l, TOK_SLASH_EQ, "/=");
			}
			return make_token(l, TOK_SLASH, "/");
		}

		if (c == '~') {
			if (peek(l) == '=') {
				advance(l);
				return make_token(l, TOK_TILDE_EQ, "~=");
			}
			return make_token(l, TOK_TILDE, "~");
		}

		if (c == '=') {
			if (peek(l) == '=') {
				advance(l);
				return make_token(l, TOK_ISEQ, "==");
			}
			return make_token(l, TOK_ASSIGN, "=");
		}
		if (c == '<') {
			if (peek(l) == '<' ) {
				advance(l);
				if (peek(l) == '=') {
					advance(l);
					return make_token(l, TOK_SHL_EQ, "<<=");
				}
				return make_token(l, TOK_SHL, "<<");
			}
			if (peek(l) == '=') {
				advance(l);
				return make_token(l, TOK_LEQ, "<=");
			}
			return make_token(l, TOK_LANGLE, "<");
		}
		if (c == '>') {
			if (peek(l) == '>') {
				advance(l);
				if (peek(l) == '=') {
					advance(l);
					return make_token(l, TOK_SHR_EQ, ">>=");
				}
				return make_token(l, TOK_SHR, ">>");
			}
			if (peek(l) == '=') {
				advance(l);
				return make_token(l, TOK_REQ, ">=");
			}
			return make_token(l, TOK_RANGLE, ">");
		}
		if (c == '%')
			return make_token(l, TOK_PERCENT, "%");

		if (c == '?')
			return make_token(l, TOK_QUESTION, "?");

		if (c == '!') {
			if (peek(l) == '=') {
				advance(l);
				return make_token(l, TOK_NOTEQ, "!=");
			}
			return make_token(l, TOK_BANG, "!");
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
					case '0':
						buffer[idx++] = '\0';
						break;
					case '"':
						buffer[idx++] = '"';
						break;
					case '\'':
						buffer[idx++] = '\'';
						break;
					case '\\':
						buffer[idx++] = '\\';
						break;
					case 'x': {
						// \xNN -- exactly two hex digits.
						int hi = hex_val(peek(l));
						if (hi < 0)
							return error_token(
								l, "\\x needs two hex digits");
						advance(l);
						int lo = hex_val(peek(l));
						if (lo < 0)
							return error_token(
								l, "\\x needs two hex digits");
						advance(l);
						buffer[idx++] = (char)(hi * 16 + lo);
						break;
					}
					default:
						// Unknown escape: keep the character but drop the
						// backslash (matches common practice; avoids a
						// silent literal backslash in output).
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

		// Radix prefixes: 0x/0X hex, 0b/0B binary, 0o octal. A bare
		// leading 0 stays decimal (no C-style auto-octal surprises).
		if (c == '0' && (peek(l) == 'x' || peek(l) == 'X' ||
						 peek(l) == 'b' || peek(l) == 'B' ||
						 peek(l) == 'o' || peek(l) == 'O')) {
			char pfx = peek(l);
			int (*valid)(int) =
				(pfx == 'x' || pfx == 'X') ? isxdigit : isdigit;
			char *start = &l->src[l->pos - 1]; // include '0' in token text
			advance(l); // consume prefix
			if (!valid(peek(l)))
				return error_token(l, "Digit expected after radix prefix");
			size_t len = 2;
			while (valid(peek(l)) || peek(l) == '_') {
				advance(l);
				len++;
			}
			char *text = arena_alloc(l->arena, len + 1);
			memcpy(text, start, len);
			text[len] = '\0';
			return make_token(l, TOK_INT_LIT, text);
		}

		if (isdigit(c)) {
			// Decimal literals: '_' is a digit separator (1_000_000 --
			// skipped entirely so strtod never sees it), '.' begins the
			// fraction. A trailing suffix f16/bf16/f32/f64 types the
			// literal explicitly (`1.5f32`, `5f32`).
			char *start = &l->src[l->pos - 1];
			int saw_fp = 0; // has fraction or exponent -> float literal
			while (isdigit(peek(l)) || peek(l) == '_' || peek(l) == '.') {
				if (peek(l) == '_') { // separator: consume, keep out of text
					advance(l);
					continue;
				}
				if (peek(l) == '.') {
					// Second '.' ends the number: `1.f` member access and
					// `arr[0].len` must lex as number-then-dot.
					if (saw_fp)
						break;
					saw_fp = 1;
				}
				advance(l);
			}
			// Exponent: 1e10, 1.5e-3 -- no separators inside.
			if (peek(l) == 'e' || peek(l) == 'E') {
				size_t save_pos = l->pos;
				advance(l); // 'e'
				if (peek(l) == '+' || peek(l) == '-')
					advance(l);
				size_t digits_here = 0;
				while (isdigit(peek(l))) {
					advance(l);
					digits_here++;
				}
				if (digits_here > 0) {
					saw_fp = 1;
				} else {
					// `1e` with no exponent digits: rewind; `e` starts an
					// identifier (hex-style naming like `1error` stays a
					// syntax error at parse time, not a lexer one).
					l->pos = save_pos;
				}
			}
			// Compact into the token text: digits/dots only, separators
			// dropped. Copy from [start, l->pos) so the suffix scan below
			// sees the position after the number.
			size_t raw_len = (size_t)(&l->src[l->pos] - start);
			char *text = arena_alloc(l->arena, raw_len + 1);
			size_t len = 0;
			for (size_t i = 0; i < raw_len; i++) {
				char ch = start[i];
				if (ch == '_')
					continue;
				text[len++] = ch;
			}
			text[len] = '\0';
			TokenType tt = saw_fp ? TOK_FLOAT_LIT : TOK_INT_LIT;
			// Float suffix: exact match of bf16 | f16 | f32 | f64 right
			// after the number. Consumed into the token; the parser maps
			// t.float_suffix to the literal's TypeKind.
			int fkind = -1;
			if (peek(l) == 'f' || (peek(l) == 'b' && peek2(l) == 'f')) {
				if (peek(l) == 'b') {
					if (l->src[l->pos + 2] == '1' && l->src[l->pos + 3] == '6')
						fkind = 4; // bf16
				} else if ((peek2(l) == '1' && peek3(l) == '6')) {
					fkind = 1;
				} else if ((peek2(l) == '3' && peek3(l) == '2')) {
					fkind = 2;
				} else if ((peek2(l) == '6' && peek3(l) == '4')) {
					fkind = 3;
				}
				if (fkind > 0) {
					size_t slen = (fkind == 4) ? 4 : 3;
					for (size_t si = 0; si < slen; si++)
						advance(l);
					Token t = make_token(l, tt, text);
					t.float_suffix = fkind;
					return t;
				}
			}
			return make_token(l, tt, text);
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
			else if (strcmp(text, "enum") == 0)
				type = TOK_ENUM;
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
			else if (strcmp(text, "for") == 0)
				type = TOK_FOR;
			else if (strcmp(text, "break") == 0)
				type = TOK_BREAK;
			else if (strcmp(text, "continue") == 0)
				type = TOK_CONTINUE;
			else if (strcmp(text, "switch") == 0)
				type = TOK_SWITCH;
			else if (strcmp(text, "case") == 0)
				type = TOK_CASE;
			else if (strcmp(text, "default") == 0)
				type = TOK_DEFAULT;
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
			else if (strcmp(text, "f16") == 0)
				type = TOK_F16;
			else if (strcmp(text, "bf16") == 0)
				type = TOK_BF16;
			else if (strcmp(text, "f32") == 0)
				type = TOK_F32;
			else if (strcmp(text, "f64") == 0)
				type = TOK_F64;
			else if (strcmp(text, "float") == 0)
				type = TOK_F32;
			else if (strcmp(text, "double") == 0)
				type = TOK_F64;
			else if (strcmp(text, "int") == 0)
				type = TOK_I32;
			else if (strcmp(text, "str") == 0)
				type = TOK_STR;

			return make_token(l, type, text);
		}

		return error_token(l, "Unexpected character");
	}
	return make_token(l, TOK_EOF, "");
}

Token lexer_peek(Lexer *l) {
	size_t save_pos = l->pos;
	size_t save_posA = l->posA;
	size_t save_posL = l->posL;
	int save_line = l->line;
	int save_err = l->had_error;
	Token t = lexer_next(l);
	l->pos = save_pos;
	l->posA = save_posA;
	l->posL = save_posL;
	l->line = save_line;
	l->had_error = save_err;
	return t;
}
