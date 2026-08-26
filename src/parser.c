#include "ast.h"
#include "timbr.h"
#include <diag.h>
#include <lexer.h>
#include <parser.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

char *get_line_text_parser(Parser *p) {
	const char *src = p->lexer->src;
	size_t pos = p->cur.pos;

	// Move to the start of the current line
	size_t start = pos;
	while (start > 0 && src[start - 1] != '\n') {
		start--;
	}

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
static inline bool parser_err(Parser *p) {
	if (p->panic_mode)
		return false;
	p->panic_mode = 1;
	p->had_error = 1;
	return true;
}
static void report_error(Parser *p, const char *fmt, ...) {
	if (!parser_err(p))
		return;
	va_list args;
	va_start(args, fmt);
	char buffer[256];
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	char *lT = get_line_text_parser(p);
	kdiag_error(KAWA_E_PARSE, p->lexer->filename, lT, p->cur.line,
				p->cur.posA, p->cur.len > 0 ? p->cur.len : 1, "%s", buffer);
	free(lT);
	va_end(args);
}

static void synchronize(Parser *p) {
	p->panic_mode = 0;
	while (p->cur.type != TOK_EOF) {
		if (p->prev.type == TOK_SEMICOLON)
			return;
		switch (p->cur.type) {
		case TOK_FN:
		case TOK_LET:
		case TOK_CONST:
		case TOK_EXTERN:
		case TOK_ASM:
		case TOK_STRUCT:
		case TOK_IMPL:
		case TOK_IF:
		case TOK_WHILE:
		case TOK_RETURN:
		case TOK_CASE:	  // switch labels own their bodies
		case TOK_DEFAULT: // -- don't skip past them during recovery
			return;
		default:;
		}
		p->cur = lexer_next(p->lexer);
	}
}

void parser_init(Parser *p, Lexer *l, Arena *a) {
	p->lexer = l;
	p->arena = a;
	p->cur = lexer_next(l);
	p->had_error = 0;
	p->panic_mode = 0;
	p->decl_count = 0;
	p->fn_sig_count = 0;
	p->soa_count = 0;
	p->struct_name_count = 0;
}

static void advance(Parser *p) {
	p->prev = p->cur;
	p->cur = lexer_next(p->lexer);
	if (p->cur.type == TOK_ERROR)
		parser_err(p);
}

static void consume(Parser *p, TokenType t, const char *err) {
	if (p->cur.type == t)
		advance(p);
	else
		report_error(p, err);
}

static ASTNode *find_decl(Parser *p, const char *name) {
	for (int i = p->decl_count - 1; i >= 0; i--) {
		if (strcmp(p->decls[i].name, name) == 0)
			return p->decls[i].node;
	}
	return NULL;
}

// True when `name` is a previously declared const -- used by the [N]
// lookahead so `const N = 8;` can be used as `[N]i32 buf;`.
static int find_decl_is_const(Parser *p, const char *name) {
	ASTNode *decl = find_decl(p, name);
	return decl && decl->type == NODE_VAR_DECL &&
		   decl->data.var_decl.is_const;
}

static int is_type_token(TokenType t) {
	return (t >= TOK_VOID && t <= TOK_F64) || t == TOK_IDENTIFIER ||
		   t == TOK_LBRACKET; // [N]T arrays and []T slices
}
static int is_likely_cast(Parser *p) {
	// Lookahead logic:
	// Case 1: (Primitive) -> e.g. (u64)
	// Case 2: (Ident)     -> e.g. (User)
	// Case 3: (Ident*)    -> e.g. (User*)

	Lexer temp = *p->lexer; // Clone lexer state
	Token t1 = lexer_next(&temp);

	// Primitive types are definitely casts
	if (t1.type >= TOK_VOID && t1.type <= TOK_F64)
		return 1;

	// Identifier types are ambiguous: (x) could be cast (Type) or grouping
	// (var)
	if (t1.type == TOK_IDENTIFIER) {
		Token t2 = lexer_next(&temp);
		// If followed by ')' it's likely a type cast: (User).
		// If followed by '*' AND another identifier/')' it's a pointer
		// cast: (User*) -- `(v * 3)` has an INT literal after the star,
		// which can only be multiplication.
		if (t2.type == TOK_RPAREN) {
			return 1;
		}
		if (t2.type == TOK_STAR) {
			Token t3 = lexer_next(&temp);
			// `(User*)`: the star closes the paren. `(v * 3)`: an operand
			// follows the star, so it's multiplication.
			return t3.type == TOK_RPAREN;
		}
	}
	return 0;
}

static Type *parse_type(Parser *p);

// Array length in `[N]T`: a literal or a const identifier. Const lookup
// goes through p->decls (registered at declaration time), so a const
// must be declared before the array that uses it -- like C.
static long parse_array_len(Parser *p) {
	if (p->cur.type == TOK_INT_LIT) {
		long len = atol(p->cur.text);
		advance(p);
		return len;
	}
	if (p->cur.type == TOK_IDENTIFIER) {
		ASTNode *decl = find_decl(p, p->cur.text);
		if (decl && decl->type == NODE_VAR_DECL &&
			decl->data.var_decl.is_const && decl->data.var_decl.init &&
			decl->data.var_decl.init->type == NODE_LITERAL) {
			long len = (long)decl->data.var_decl.init->data.literal.i_val;
			advance(p);
			return len;
		}
		report_error(p, "Array length must be a literal or const");
		advance(p);
		return 0;
	}
	report_error(p, "Expected array length");
	return 0;
}

static Type *parse_type(Parser *p) {
	Type *t = arena_alloc(p->arena, sizeof(Type));
	TokenType tok = p->cur.type;

	// Prefix array syntax: [N]T. Empty brackets make a slice []T -- a
	// ptr+len view, no ownership.
	if (tok == TOK_LBRACKET) {
		advance(p);
		if (p->cur.type == TOK_RBRACKET) {
			advance(p);
			Type *elem = parse_type(p);
			t->kind = TYPE_SLICE;
			t->inner = elem;
			return t;
		}
		long len = parse_array_len(p);
		consume(p, TOK_RBRACKET, "Expected ']' after array length");
		Type *elem = parse_type(p);
		t->kind = TYPE_ARRAY;
		t->inner = elem;
		t->array_len = len;
		return t;
	}

	if (tok == TOK_U32)
		t->kind = TYPE_U32;
	else if (tok == TOK_I32)
		t->kind = TYPE_I32;
	else if (tok == TOK_F16)
		t->kind = TYPE_F16;
	else if (tok == TOK_BF16)
		t->kind = TYPE_BF16;
	else if (tok == TOK_F32)
		t->kind = TYPE_F32;
	else if (tok == TOK_VOID)
		t->kind = TYPE_VOID;
	else if (tok == TOK_BOOL)
		t->kind = TYPE_BOOL;
	else if (tok == TOK_CHAR)
		t->kind = TYPE_CHAR;
	else if (tok == TOK_I8)
		t->kind = TYPE_I8;
	else if (tok == TOK_U8)
		t->kind = TYPE_U8;
	else if (tok == TOK_I16)
		t->kind = TYPE_I16;
	else if (tok == TOK_U16)
		t->kind = TYPE_U16;
	else if (tok == TOK_I64)
		t->kind = TYPE_I64;
	else if (tok == TOK_U64)
		t->kind = TYPE_U64;
	else if (tok == TOK_F64)
		t->kind = TYPE_F64;
	else if (tok == TOK_STR) {
		// `str` is a builtin: a fat {ptr,len} view over UTF-8 bytes
		// (IDEAS 3) -- same layout as []u8. `.len`/`.data`, content
		// comparison, concat and indexing are primitives on the view.
		// Falls through to the shared postfix handling below, so
		// `str*` (ptr-to-view) also works.
		Type *ch = arena_alloc(p->arena, sizeof(Type));
		ch->kind = TYPE_U8;
		ch->inner = NULL;
		t->kind = TYPE_SLICE;
		t->inner = ch;
	} else if (tok == TOK_IDENTIFIER && strcmp(p->cur.text, "chan") == 0 &&
			   lexer_peek(p->lexer).type == TOK_LANGLE) {
		// chan<T>: a buffered channel over T (IDEAS 3). Cooperative
		// single-thread semantics: blocking ops yield via drop{}.
		advance(p); // 'chan'
		advance(p); // '<'
		Type *elem = parse_type(p);
		consume(p, TOK_RANGLE, "Expected '>' after channel element type");
		t->kind = TYPE_CHAN;
		t->inner = elem;
		return t;
	} else if (tok == TOK_IDENTIFIER) {
		t->kind = TYPE_STRUCT;
		t->name = p->cur.text;
	} else {
		report_error(p, "Expected type");
		return t;
	}
	advance(p);
	// Postfix [N]: fixed-size array type, e.g. [4]i32 or [16]User*
	if (p->cur.type == TOK_LBRACKET) {
		advance(p);
		long len = parse_array_len(p);
		consume(p, TOK_RBRACKET, "Expected ']' after array length");
		Type *arr = arena_alloc(p->arena, sizeof(Type));
		arr->kind = TYPE_ARRAY;
		arr->inner = t;
		arr->array_len = len;
		t = arr;
	}
	TokenType cur_t = p->cur.type;
	while (cur_t == TOK_STAR || cur_t == TOK_AMP) {
		advance(p);
		if (cur_t == TOK_STAR) {
			Type *ptr = arena_alloc(p->arena, sizeof(Type));
			ptr->kind = TYPE_PTR;
			ptr->inner = t;
			t = ptr;
		} else {
			Type *amp = arena_alloc(p->arena, sizeof(Type));
			amp->kind = TYPE_AMP;
			amp->inner = t;
			t = amp;
		}
		cur_t = p->cur.type;
	}
	return t;
}

static ASTNode *parse_postfix(Parser *p);
static int type_is_signed_k(Type *t) {
	if (!t)
		return 0;
	switch (t->kind) {
	case TYPE_I8: case TYPE_I16: case TYPE_I32: case TYPE_I64:
		return 1;
	default:
		return 0;
	}
}

// Shared singleton for expression-level bool results (comparisons, logical
// ops). Values are i1 in IR, so a `let f = a < b;` declares an i1 local --
// no zext to u32, no trunc on use.
static Type *bool_result_type(Parser *p) {
	static Type *cached;
	if (!cached) {
		cached = arena_alloc(p->arena, sizeof(Type));
		cached->kind = TYPE_BOOL;
	}
	return cached;
}

// Integer promotion at the AST level: pick the type both sides coerce to
// without loss -- the wider width; signed wins when widths tie. NULL types
// pass through so untyped operands keep today's i32-default behavior.
static Type *unify_types(Parser *p, Type *a, Type *b) {
	if (!a)
		return b;
	if (!b)
		return a;
	int a_int = (a->kind >= TYPE_I8 && a->kind <= TYPE_U64) ||
				a->kind == TYPE_CHAR || a->kind == TYPE_BOOL;
	int b_int = (b->kind >= TYPE_I8 && b->kind <= TYPE_U64) ||
				b->kind == TYPE_CHAR || b->kind == TYPE_BOOL;
	if (a_int && b_int) {
		static const int rank[] = {
			[TYPE_I8] = 1, [TYPE_U8] = 1, [TYPE_CHAR] = 1,
			[TYPE_I16] = 2, [TYPE_U16] = 2,
			[TYPE_I32] = 3, [TYPE_U32] = 3,
			[TYPE_I64] = 4, [TYPE_U64] = 4,
			[TYPE_BOOL] = 0,
		};
		int ra = rank[a->kind];
		int rb = rank[b->kind];
		if (ra != rb)
			return ra > rb ? a : b;
		return type_is_signed_k(a) ? a : b;
	}
	// Mixed FP widths: the result is the wider precision (f16+bf16 meet
	// at f32, f32+f64 -> f64) -- mirrors the runtime promotion in
	// codegen's binop path so parse-time and IR-time types agree.
	int a_fp = (a->kind >= TYPE_F16 && a->kind <= TYPE_F64);
	int b_fp = (b->kind >= TYPE_F16 && b->kind <= TYPE_F64);
	if (a_fp && b_fp) {
		static const int fp_rank[] = {
			[TYPE_F16] = 1, [TYPE_BF16] = 1, [TYPE_F32] = 2, [TYPE_F64] = 3};
		return fp_rank[a->kind] >= fp_rank[b->kind] ? a : b;
	}
	// Int mixed with FP: usual arithmetic conversions promote the int to
	// the FP side (`2 * 1.5f32` is 3.0f32), same rule as C.
	if (a_int && b_fp)
		return b;
	if (b_int && a_fp)
		return a;
	return a->kind == b->kind ? a : NULL;
}

// 2. Implement parse_unary
static ASTNode *parse_unary(Parser *p) {
	// Prefix channel receive: `<-ch` yields the next element (IDEAS 3).
	if (p->cur.type == TOK_RECV) {
		advance(p);
		ASTNode *n = arena_alloc(p->arena, sizeof(ASTNode));
		n->type = NODE_RECV;
		n->data.recv.chan = parse_unary(p);
		Type *ct = n->data.recv.chan->data_type;
		n->data_type = (ct && ct->inner) ? ct->inner : NULL;
		return n;
	}
	if (p->cur.type == TOK_STAR) {
		advance(p); // Eat '*'
		ASTNode *n = arena_alloc(p->arena, sizeof(ASTNode));
		n->type = NODE_DEREF;
		n->data.deref.expr = parse_unary(p); // Recurse for **ptr

		// Type Inference: If expr is T*, this node is T
		if (n->data.deref.expr->data_type &&
			n->data.deref.expr->data_type->kind == TYPE_PTR) {
			n->data_type = n->data.deref.expr->data_type->inner;
		}
		return n;
	} else if (p->cur.type == TOK_AMP) {
		advance(p); // Eat '&'
		ASTNode *n = arena_alloc(p->arena, sizeof(ASTNode));
		n->type = NODE_AMP;
		n->data.deref.expr = parse_unary(p); // Recurse for &&var

		// Type Inference: If expr is T*, this node is T
		if (n->data.deref.expr->data_type &&
			n->data.deref.expr->data_type->kind == TYPE_AMP) {
			n->data_type = n->data.deref.expr->data_type->inner;
		}
		return n;
	} else if (p->cur.type == TOK_MINUS || p->cur.type == TOK_BANG ||
			   p->cur.type == TOK_TILDE) {
		// Unary minus / logical not / bitwise not. Desugared to binary ops
		// so codegen needs no new node kinds: -x => 0 - x, !x => x == 0,
		// ~x => x ^ all-ones.
		int op = p->cur.type;
		advance(p);
		ASTNode *operand = parse_unary(p);

		if (op == TOK_MINUS && operand->type == NODE_LITERAL &&
			operand->data_type &&
			(operand->data_type->kind == TYPE_U32 ||
			 operand->data_type->kind == TYPE_U64) &&
			operand->data.literal.i64_val > 0) {
			// Negative int literal: fold to a signed literal directly
			// instead of `0 - x` (which would keep u32 typing and turn
			// shifts/comparisons unsigned). u64-typed literals (2147483648
			// and up) become i64 so -2147483648 keeps its sign.
			long long neg = -operand->data.literal.i64_val;
			operand->data.literal.i64_val = neg;
			operand->data.literal.i_val = (int)neg;
			operand->data_type = arena_alloc(p->arena, sizeof(Type));
			operand->data_type->kind =
				neg < INT32_MIN ? TYPE_I64 : TYPE_I32;
			return operand;
		}

		if (op == TOK_TILDE) {
			// ~x => x ^ (-1): all-ones of the operand's width. The literal
			// carries the operand type so the folder handles it at comptime.
			// -1 stored as i64_val keeps every bit set at any width.
			// Bare var refs carry no parse-time type: resolve from the decl
			// table now or `let v = ~x;` infers u32 and truncates.
			if (!operand->data_type &&
				operand->type == NODE_VAR_REF) {
				ASTNode *decl = find_decl(p,
					operand->data.var_ref.name);
				if (decl && decl->data_type)
					operand->data_type = decl->data_type;
			}
			ASTNode *ones = arena_alloc(p->arena, sizeof(ASTNode));
			ones->type = NODE_LITERAL;
			ones->data.literal.i_val = -1;
			ones->data.literal.i64_val = -1;
			ones->data_type = operand->data_type;

			ASTNode *n = arena_alloc(p->arena, sizeof(ASTNode));
			n->type = NODE_BINARY_OP;
			n->data.bin_op.op = TOK_CARET;
			n->data.bin_op.left = operand; // note: operand on the LEFT
			n->data.bin_op.right = ones;
			n->data_type = operand->data_type;
			return n;
		}

		ASTNode *zero = arena_alloc(p->arena, sizeof(ASTNode));
		zero->type = NODE_LITERAL;
		zero->data.literal.i_val = 0;
		zero->data.literal.i64_val = 0;
		zero->data_type = operand->data_type;

		ASTNode *n = arena_alloc(p->arena, sizeof(ASTNode));
		n->type = NODE_BINARY_OP;
		n->data.bin_op.op = (op == TOK_MINUS) ? TOK_MINUS : TOK_ISEQ;
		n->data.bin_op.left = zero;
		n->data.bin_op.right = operand;
		n->data_type = (op == TOK_MINUS) ? operand->data_type
										 : bool_result_type(p);
		return n;
	}
	return parse_postfix(p); // Fall through to postfix/primary
}
static ASTNode *parse_expr(Parser *p);
static ASTNode *parse_statement(Parser *p);
static ASTNode *parse_statement_inner(Parser *p, int stmt_line);
static ASTNode *parse_block(Parser *p);
static ASTNode *parse_grind(Parser *p);
static ASTNode *parse_var_or_expr_no_semi(Parser *p);
static ASTNode *parse_expr_stmt_tail(Parser *p, ASTNode *expr, int need_semi);

static ASTNode *parse_struct_literal(Parser *p);
static int peek_is_struct_literal(Parser *p);
static void parse_enum(Parser *p, ASTNode ***tail);

// Returns 1 if the upcoming '{ ... }' looks like a struct literal.
// Returns 0 if it looks like a block code.
// True when `name` resolves to a variable whose declared type is a
// #[soa]-marked struct.
static int is_soa_struct_var(Parser *p, const char *name) {
	ASTNode *decl = find_decl(p, name);
	if (!decl || !decl->data_type ||
		decl->data_type->kind != TYPE_STRUCT || !decl->data_type->name)
		return 0;
	for (int i = 0; i < p->soa_count; i++)
		if (strcmp(p->soa_structs[i], decl->data_type->name) == 0)
			return 1;
	return 0;
}

// Struct embedding (IDEAS 3): find a direct field of struct `sname` whose
// name is `field`. Returns the embedded FIELD's declaring node or NULL.
static ASTNode *find_struct_field(Parser *p, const char *sname,
								  const char *field);

// Declared type of field `field` on struct `sname` (parse-time view of the
// struct registry; NULL when either is unknown). Powers member-chain
// receiver typing for method calls (`d.Base.who()`).
static Type *find_field_type(Parser *p, Type *struct_t, const char *field) {
	if (!struct_t || struct_t->kind != TYPE_STRUCT || !struct_t->name)
		return NULL;
	for (int si = 0; si < p->struct_name_count; si++) {
		if (strcmp(p->struct_names[si], struct_t->name) != 0)
			continue;
		for (ASTNode *f = p->struct_nodes[si]->data.struct_decl.fields; f;
			 f = f->next)
			if (strcmp(f->data.var_decl.name, field) == 0)
				return f->data_type;
		break;
	}
	return NULL;
}

// Type for a struct declared in this program: TYPE_STRUCT carrying the name.
static Type *find_struct_type_by_name(Parser *p, const char *name) {
	for (int si = 0; si < p->struct_name_count; si++) {
		if (strcmp(p->struct_names[si], name) == 0) {
			Type *t = arena_alloc(p->arena, sizeof(Type));
			t->kind = TYPE_STRUCT;
			t->name = p->struct_names[si];
			return t;
		}
	}
	return NULL;
}

static int fn_sig_known(Parser *p, const char *name) {
	for (int k = p->fn_sig_count - 1; k >= 0; k--)
		if (strcmp(p->fn_sigs[k].name, name) == 0)
			return 1;
	return 0;
}

// Struct-embedding method promotion (IDEAS 3): when `outer` declares no
// method `m`, search its direct embedded structs -- and theirs, depth-first
// (shallowest wins, Go's rule). On success writes the declaring struct's
// name into out_struct and the EMBEDDING FIELD PATH into out_path
// ("Mid" or "Mid.Inner"); the caller rewrites the receiver to obj.<path>.
static int promoted_method_lookup(Parser *p, const char *outer,
								  const char *method, int depth,
								  char *out_struct, size_t ssz,
								  char *out_path, size_t psz) {
	if (depth > 8)
		return 0;
	for (int si = 0; si < p->struct_name_count; si++) {
		if (strcmp(p->struct_names[si], outer) != 0)
			continue;
		ASTNode *st = p->struct_nodes[si];
		for (ASTNode *f = st->data.struct_decl.fields; f; f = f->next) {
			if (!f->data_type || f->data_type->kind != TYPE_STRUCT ||
				!f->data_type->name)
				continue;
			const char *ename = f->data_type->name;
			char qual[256];
			snprintf(qual, sizeof(qual), "%s__%s", ename, method);
			if (fn_sig_known(p, qual)) {
				snprintf(out_struct, ssz, "%s", ename);
				snprintf(out_path, psz, "%s", f->data.var_decl.name);
				return 1;
			}
			if (promoted_method_lookup(p, ename, method, depth + 1,
									   out_struct, ssz, out_path + 0,
									   psz)) {
				// Prepend this hop: path becomes "<f>.<inner path>".
				char tail[256];
				snprintf(tail, sizeof(tail), "%s", out_path);
				snprintf(out_path, psz, "%s.%s", f->data.var_decl.name,
						 tail);
				return 1;
			}
		}
		break;
	}
	return 0;
}

static int peek_is_struct_literal(Parser *p) {
	Lexer temp = *p->lexer; // Clone lexer state to peek without consuming

	// We assume current token is '{'. Skip it.
	Token t = lexer_next(&temp);

	// Case 1: Empty {} -> Ambiguous, default to Block (or empty struct?)
	// In Kawa, empty blocks are common, empty structs less so.
	if (t.type == TOK_RBRACE)
		return 0;

	// Case 2: Designated Initializer: { .field = ... }
	if (t.type == TOK_DOT)
		return 1;

	// Case 3: Scan for separator
	// If we find a comma (,) before a semicolon (;), it is a Struct Literal.
	// If we find a semicolon (;) first, it is a Block.
	int depth = 1;
	while (t.type != TOK_EOF && depth > 0) {
		if (t.type == TOK_LBRACE)
			depth++;
		if (t.type == TOK_RBRACE) {
			depth--;
			if (depth == 0)
				break;
		}

		if (depth == 1) {
			if (t.type == TOK_COMMA)
				return 1; // Found comma at top level -> Struct
			if (t.type == TOK_SEMICOLON)
				return 0; // Found semicolon -> Block
			// If we see keywords like 'return', 'let', 'while', it's a block
			if (t.type == TOK_RETURN || t.type == TOK_LET ||
				t.type == TOK_WHILE)
				return 0;
		}
		t = lexer_next(&temp);
	}

	// Fallback: If we scanned the whole thing and found neither,
	// it's a single expression { expr }. treat as Block (Expression Block)
	// or Struct? Let's default to Block for { 1 } grouping behavior.
	return 0;
}

// ---------------------------------------------------------
// 2. STRUCT LITERAL PARSER
// ---------------------------------------------------------
static ASTNode *parse_struct_literal(Parser *p) {
	ASTNode *n = arena_alloc(p->arena, sizeof(ASTNode));
	n->type = NODE_STRUCT_LITERAL;
	n->data_type = NULL; // Type is usually inferred from context (assignment)

	consume(p, TOK_LBRACE, "Expected '{'");

	StructInitItem *head = NULL;
	StructInitItem **tail = &head;

	while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
		StructInitItem *item = arena_alloc(p->arena, sizeof(StructInitItem));
		item->field_name = NULL;

		// Handle Designated Init: .age = 10
		if (p->cur.type == TOK_DOT) {
			advance(p);
			item->field_name = p->cur.text;
			consume(p, TOK_IDENTIFIER, "Expected field name");
			consume(p, TOK_ASSIGN, "Expected '='");
		} else if (p->cur.type == TOK_IDENTIFIER &&
				   lexer_peek(p->lexer).type != TOK_LPAREN &&
				   lexer_peek(p->lexer).type != TOK_LBRACE) {
			// Field-init shorthand: `Vec { x, y }` means `.x = x, .y = y`
			// -- the identifier names the field AND supplies the value.
			item->field_name = p->cur.text;
			item->value = parse_expr(p);
			item->next = NULL;
			*tail = item;
			tail = &item->next;
			if (p->cur.type == TOK_COMMA)
				advance(p);
			else if (p->cur.type != TOK_RBRACE)
				report_error(p,
							 "Expected ',' or '}' in struct literal");
			continue;
		}

		item->value = parse_expr(p);
		item->next = NULL;
		*tail = item;
		tail = &item->next;

		if (p->cur.type == TOK_COMMA) {
			advance(p);
		} else if (p->cur.type != TOK_RBRACE) {
			report_error(p, "Expected ',' or '}' in struct literal");
			break;
		}
	}
	consume(p, TOK_RBRACE, "Expected '}'");
	n->data.struct_lit.items = head;
	return n;
}

// Prefix channel receive: `<-ch` yields the next element.
static ASTNode *parse_recv(Parser *p) {
	consume(p, TOK_RECV, "<-");
	ASTNode *n = arena_alloc(p->arena, sizeof(ASTNode));
	n->type = NODE_RECV;
	n->data.recv.chan = parse_unary(p);
	if (!n->data_type) {
		// Stamp from the channel's element type when the operand names one.
		Type *ct = n->data.recv.chan->data_type;
		n->data_type = ct ? ct->inner : NULL;
	}
	return n;
}

static ASTNode *parse_primary(Parser *p) {
	ASTNode *n = arena_alloc(p->arena, sizeof(ASTNode));
	if (p->cur.type == TOK_INT_LIT) {
		n->type = NODE_LITERAL;
		n->data_type = arena_alloc(p->arena, sizeof(Type));
		// strtoull + range check, not atoi: atoi silently truncates
		// `4000000000` to garbage and even wraps negatives. Literals type
		// by value like C: i32 when it fits, then u32, u64, and finally an
		// error only past UINT64_MAX -- `i64 x = 10000000000` must work.
		// Radix prefixes (0x/0b/0o) are stripped first; embedded '_'
		// separators are ignored.
		char digits[80];
		const char *dtxt = p->cur.text;
		int base = 10;
		if (dtxt[0] == '0' && (dtxt[1] == 'x' || dtxt[1] == 'X')) {
			base = 16;
			dtxt += 2;
		} else if (dtxt[0] == '0' &&
				   (dtxt[1] == 'b' || dtxt[1] == 'B')) {
			base = 2;
			dtxt += 2;
		} else if (dtxt[0] == '0' &&
				   (dtxt[1] == 'o' || dtxt[1] == 'O')) {
			base = 8;
			dtxt += 2;
		}
		size_t dn = 0;
		for (const char *q = dtxt; *q && dn < sizeof(digits) - 1; q++)
			if (*q != '_')
				digits[dn++] = *q;
		digits[dn] = '\0';
		errno = 0;
		char *end = NULL;
		unsigned long long v = strtoull(digits, &end, base);
		if (errno == ERANGE || *end) {
			report_error(p, "Integer literal out of range for u64");
			n->data_type->kind = TYPE_I64;
			n->data.literal.i64_val = 0;
		} else if (v <= INT32_MAX) {
			n->data_type->kind = TYPE_I32;
			n->data.literal.i64_val = (long long)v;
		} else if (v <= UINT32_MAX) {
			n->data_type->kind = TYPE_U32;
			n->data.literal.i64_val = (long long)v;
		} else {
			// Fits only in 64 bits; the sign bit is fine for u64 values.
			n->data_type->kind = TYPE_U64;
			n->data.literal.i64_val = (long long)v;
		}
		n->data.literal.i_val = (int)n->data.literal.i64_val;
		advance(p);
	} else if (p->cur.type == TOK_FLOAT_LIT) {
		n->type = NODE_LITERAL;
		n->data_type = arena_alloc(p->arena, sizeof(Type));
		// Suffix wins (1.5f32 is exactly f32); unsuffixed defaults to f64
		// so plain numeric code keeps C's default precision.
		switch (p->cur.float_suffix) {
		case 1: n->data_type->kind = TYPE_F16; break;
		case 2: n->data_type->kind = TYPE_F32; break;
		case 4: n->data_type->kind = TYPE_BF16; break;
		default: n->data_type->kind = TYPE_F64; break;
		}
		n->data.literal.f_val = strtod(p->cur.text, NULL);
		advance(p);
	} else if (p->cur.type == TOK_STRING_LIT) {
		n->type = NODE_STRING_LIT;
		n->data.str_lit.s_val = p->cur.text;
		// A string literal IS a str: a fat {ptr,len} view over the
		// constant's bytes (IDEAS 3). The storage keeps a trailing NUL so
		// decaying to char* at C boundaries stays valid. len/data members,
		// content comparison, concat and indexing all come free with the
		// slice representation.
		Type *ch = arena_alloc(p->arena, sizeof(Type));
		ch->kind = TYPE_U8;
		Type *st = arena_alloc(p->arena, sizeof(Type));
		st->kind = TYPE_SLICE;
		st->inner = ch;
		n->data_type = st;
		advance(p);
	} else if (p->cur.type == TOK_TRUE) {
		n->type = NODE_LITERAL;
		n->data.literal.i_val = 1;
		n->data.literal.i64_val = 1;
		advance(p);
	} else if (p->cur.type == TOK_FALSE) {
		n->type = NODE_LITERAL;
		n->data.literal.i_val = 0;
		n->data.literal.i64_val = 0;
		advance(p);
	} else if (p->cur.type == TOK_SIZEOF) {
		advance(p);
		consume(p, TOK_LPAREN, "Expected '(' after sizeof");

		Type *t_val = NULL;
		ASTNode *ctx = NULL;

		// Check if it's a type or an expression.
		// An identifier is only treated as a type when it can't be an
		// expression -- i.e. a primitive type token, or an identifier
		// directly followed by ')' (e.g. sizeof(Car)) or '*' (pointer).
		if (is_type_token(p->cur.type) && p->cur.type != TOK_IDENTIFIER) {
			t_val = parse_type(p);
		} else if (p->cur.type == TOK_IDENTIFIER) {
			Lexer temp = *p->lexer;
			Token nxt = lexer_next(&temp);
			if (nxt.type == TOK_RPAREN || nxt.type == TOK_STAR) {
				t_val = parse_type(p);
			} else {
				ctx = parse_expr(p);
			}
		} else {
			// Expression
			ctx = parse_expr(p);
		}

		consume(p, TOK_RPAREN, "Expected ')'");

		n->type = NODE_SIZEOF;
		n->data.size_of.type_val = t_val;
		n->data.size_of.value = ctx;
		// The RESULT of sizeof is a u32/u64 literal (constant folded usually,
		// or computed) We'll set type as U32 for now
		n->data_type = arena_alloc(p->arena, sizeof(Type));
		n->data_type->kind = TYPE_U32;

	} else if (p->cur.type == TOK_GRIND) {
		advance(p);
		return parse_grind(p);
	} else if (p->cur.type == TOK_LBRACE) {
		// A '{' directly after '=' (or ',' / '(' / ':' / a cast type) can
		// only be an array/struct literal -- statement blocks never appear
		// there. This makes single-element literals `{ 42 }` work.
		int value_position = (p->prev.type == TOK_ASSIGN ||
							  p->prev.type == TOK_COLON_ASSIGN ||
							  p->prev.type == TOK_COMMA ||
							  p->prev.type == TOK_COLON ||
							  p->prev.type == TOK_LPAREN);
		if (value_position || peek_is_struct_literal(p)) {
			return parse_struct_literal(p);
		} else {
			return parse_block(p);
		}
	} else if (p->cur.type == TOK_IDENTIFIER) {
		n->type = NODE_VAR_REF;
		n->data.var_ref.name = p->cur.text;
		n->line = p->cur.line; // diagnostics: undefined-variable carets
		advance(p);
	} else if (p->cur.type == TOK_LPAREN) {
		// [FIX] Use lookahead to distinguish Cast vs Grouping
		if (is_likely_cast(p)) {
			// --- Parse as CAST ---
			advance(p); // eat '('
			Type *cast_type = parse_type(p);
			consume(p, TOK_RPAREN, "Expected ')' after cast type");

			// [CHANGE] Use parse_unary to allow casting things like *ptr
			ASTNode *val = parse_unary(p);

			// Rewrite 'n' as a CAST node
			n->type = NODE_CAST;
			n->data_type = cast_type;
			n->data.cast.val = val;
			return n;
		} else {
			// --- Parse as GROUPING ---
			advance(p); // eat '('
			ASTNode *expr = parse_expr(p);
			consume(p, TOK_RPAREN, "Expected ')'");
			return expr;
		}
	} else if (p->cur.type == TOK_ASM) {
		// asm { "instructions" : "constraints" }; -- inline asm, last
		// resort. The template is emitted verbatim; `$N` refers to the
		// operands in LLVM's syntax. v1 has no operand plumbing: the
		// block is a side-effecting barrier.
		advance(p);
		n->type = NODE_ASM;
		consume(p, TOK_LBRACE, "Expected '{' after asm");
		if (p->cur.type != TOK_STRING_LIT)
			report_error(p, "Expected instruction string");
		char *tmpl = p->cur.text;
		advance(p);
		while (p->cur.type == TOK_STRING_LIT) {
			// Adjacent string literals concatenate (multi-line asm).
			size_t len = strlen(tmpl) + strlen(p->cur.text) + 2;
			char *joined = arena_alloc(p->arena, len);
			snprintf(joined, len, "%s\n%s", tmpl, p->cur.text);
			tmpl = joined;
			advance(p);
		}
		char *constraints = NULL;
		if (p->cur.type == TOK_COLON) {
			advance(p);
			if (p->cur.type != TOK_STRING_LIT)
				report_error(p, "Expected constraint string");
			constraints = p->cur.text;
			advance(p);
		}
		consume(p, TOK_RBRACE, "Expected '}' to close asm");
		n->data.asm_block.asm_template = tmpl;
		n->data.asm_block.constraints = constraints;
	} else if (p->cur.type == TOK_BREW) {
		advance(p);
		n->type = NODE_BREW;
		consume(p, TOK_LBRACE, "{");
		ASTNode *body = arena_alloc(p->arena, sizeof(ASTNode));
		body->type = NODE_BLOCK;
		ASTNode **tail = &body->data.block.stmts;
		while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
			*tail = parse_statement(p);
			if (*tail)
				tail = &(*tail)->next;
		}
		consume(p, TOK_RBRACE, "}");
		n->data.brew.body = body;
		n->data_type = arena_alloc(p->arena, sizeof(Type));
		n->data_type->kind = TYPE_HANDLE;
	} else if (p->cur.type == TOK_SIP) {
		advance(p);
		consume(p, TOK_LPAREN, "(");
		n->type = NODE_SIP;
		n->data.sip.handle = parse_expr(p);
		consume(p, TOK_RPAREN, ")");
	} else if (p->cur.type == TOK_SET) {
		advance(p);
		consume(p, TOK_LBRACE, "{");
		n->type = NODE_SET_LITERAL;
		n->data_type = arena_alloc(p->arena, sizeof(Type));
		n->data_type->kind = TYPE_SET;

		// We need to infer the inner type of the set based on the first element
		ASTNode *head = NULL;
		ASTNode **tail = &head;
		Type *inner_type = NULL;

		while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
			ASTNode *item = parse_expr(p);
			if (!inner_type && item->data_type)
				inner_type = item->data_type;
			*tail = item;
			if (*tail) {
				tail = &(*tail)->next;
				if (p->cur.type == TOK_COMMA)
					advance(p);
			} else
				break;
		}
		// Set the inner type
		if (!inner_type) {
			inner_type = arena_alloc(p->arena, sizeof(Type));
			inner_type->kind = TYPE_U32; // Default to U32 if empty
		}
		n->data_type->inner = inner_type;

		consume(p, TOK_RBRACE, "}");
		n->data.set_lit.items = head;
	} else {
		report_error(p, "Unexpected token in expression: %s", p->cur.text);
		n->type = NODE_LITERAL;
		n->data.literal.i_val = 0;
		n->data.literal.i64_val = 0;
		advance(p);
	}
	return n;
}

static ASTNode *parse_postfix(Parser *p) {
	ASTNode *expr = parse_primary(p);
	while (1) {
		// TypeName { ... } in expression position: a struct literal. The
		// identifier must name a declared struct, so `if (x) { ... }`-style
		// blocks after bare identifiers never reach here.
		if (p->cur.type == TOK_LBRACE && expr->type == NODE_VAR_REF) {
			int names_struct = 0;
			for (int si = 0; si < p->struct_name_count; si++)
				if (strcmp(p->struct_names[si],
						   expr->data.var_ref.name) == 0)
					names_struct = 1;
			if (names_struct) {
				Type *st_t = arena_alloc(p->arena, sizeof(Type));
				st_t->kind = TYPE_STRUCT;
				st_t->name = expr->data.var_ref.name;
				ASTNode *lit = parse_struct_literal(p);
				lit->data_type = st_t;
				return lit;
			}
		}
		if (p->cur.type == TOK_DOT) {
			advance(p);
			// SoA rewrite (IDEAS 2.7): `ps[i].x` on a #[soa] struct value
			// becomes `ps.x[i]` -- member access binds before indexing, so
			// each field is its own contiguous array. Applied at parse time
			// by rebuilding the node chain.
			if (expr->type == NODE_INDEX &&
				expr->data.index.object->type == NODE_VAR_REF &&
				is_soa_struct_var(
					p, expr->data.index.object->data.var_ref.name)) {
				char *mname2 = p->cur.text;
				consume(p, TOK_IDENTIFIER, "Expected member name");
				ASTNode *member2 = arena_alloc(p->arena, sizeof(ASTNode));
				member2->type = NODE_MEMBER_ACCESS;
				member2->data.member_access.object =
					expr->data.index.object;
				member2->data.member_access.member = mname2;
				ASTNode *index2 = arena_alloc(p->arena, sizeof(ASTNode));
				index2->type = NODE_INDEX;
				index2->line = expr->line;
				index2->data.index.object = member2;
				index2->data.index.index = expr->data.index.index;
				expr = index2;
				continue;
			}
			ASTNode *member = arena_alloc(p->arena, sizeof(ASTNode));
			member->type = NODE_MEMBER_ACCESS;
			member->line = p->prev.line;
			member->data.member_access.object = expr;
			member->data.member_access.member = p->cur.text;
			consume(p, TOK_IDENTIFIER, "Expected member name");
			// Slice builtins: `xs.len` is i64, `xs.data` is T*. Bare var
			// refs need the decl-table lookup; typed exprs carry it already.
			if (!member->data_type) {
				Type *obj_t = NULL;
				if (expr->type == NODE_VAR_REF) {
					ASTNode *decl =
						find_decl(p, expr->data.var_ref.name);
					obj_t = decl ? decl->data_type : NULL;
				} else {
					obj_t = expr->data_type;
				}
				if (obj_t && (obj_t->kind == TYPE_SLICE ||
							  obj_t->kind == TYPE_ARRAY)) {
					if (strcmp(member->data.member_access.member,
							   "len") == 0) {
						Type *lt = arena_alloc(p->arena, sizeof(Type));
						lt->kind = TYPE_I64;
						member->data_type = lt;
					} else if (strcmp(member->data.member_access.member,
									  "data") == 0) {
						Type *pt = arena_alloc(p->arena, sizeof(Type));
						pt->kind = TYPE_PTR;
						pt->inner = obj_t->inner;
						member->data_type = pt;
					}
				}
			}
			expr = member;
		} else if (p->cur.type == TOK_ARROW) {
			advance(p);
			// Transform expr->member to (*expr).member
			ASTNode *deref = arena_alloc(p->arena, sizeof(ASTNode));
			deref->type = NODE_DEREF;
			deref->data.deref.expr = expr;

			// Type Inference for Deref
			if (expr->data_type && expr->data_type->kind == TYPE_PTR) {
				deref->data_type = expr->data_type->inner;
			}

			ASTNode *member = arena_alloc(p->arena, sizeof(ASTNode));
			member->type = NODE_MEMBER_ACCESS;
			member->line = p->prev.line;
			member->data.member_access.object = deref;
			member->data.member_access.member = p->cur.text;
			consume(p, TOK_IDENTIFIER, "Expected member name after ->");
			expr = member;
		} else if (p->cur.type == TOK_LBRACKET) {
			advance(p);
			ASTNode *idx = parse_expr(p);
			consume(p, TOK_RBRACKET, "Expected ']' after index");
			ASTNode *index = arena_alloc(p->arena, sizeof(ASTNode));
			index->type = NODE_INDEX;
			index->line = p->cur.line;
			index->data.index.object = expr;
			index->data.index.index = idx;
			// Element type: peel one array/pointer/slice layer.
			if (!index->data_type && expr->type == NODE_VAR_REF) {
				ASTNode *decl = find_decl(p, expr->data.var_ref.name);
				if (decl && decl->data_type &&
					(decl->data_type->kind == TYPE_ARRAY ||
					 decl->data_type->kind == TYPE_PTR ||
					 decl->data_type->kind == TYPE_SLICE ||
					 decl->data_type->kind == TYPE_AMP))
					index->data_type = decl->data_type->inner;
			}
			if (expr->data_type &&
				(expr->data_type->kind == TYPE_ARRAY ||
				 expr->data_type->kind == TYPE_PTR ||
				 expr->data_type->kind == TYPE_SLICE ||
				 expr->data_type->kind == TYPE_AMP)) {
				index->data_type = expr->data_type->inner;
			}
			expr = index;
		} else if (p->cur.type == TOK_LPAREN) {
			advance(p);

			// --- NEW: Detect Method Call and Transform ---
			int is_method_call = 0;
			ASTNode *self_obj = NULL;
			char *struct_name = NULL;
			char *method_name = NULL;

			int is_assoc_call = 0; // Type.fn(...) associated-function call
			if (expr->type == NODE_MEMBER_ACCESS) {
				self_obj = expr->data.member_access.object;
				method_name = expr->data.member_access.member;

				// Associated functions first: `Vec.new(...)` where the
				// object NAMES a struct (not a variable) and `Vec__new`
				// exists. The callee mangles; nothing is injected -- the
				// declared signature IS the argument list.
				if (self_obj->type == NODE_VAR_REF && method_name) {
					const char *tname =
						self_obj->data.var_ref.name;
					int t_is_struct = 0;
					for (int si = 0; si < p->struct_name_count; si++) {
						if (strcmp(p->struct_names[si], tname) == 0) {
							t_is_struct = 1;
							break;
						}
					}
					if (t_is_struct && !find_decl(p, tname)) {
						char amangled[256];
						snprintf(amangled, sizeof(amangled), "%s__%s",
								 tname, method_name);
						if (fn_sig_known(p, amangled)) {
							struct_name = arena_strdup(p->arena, tname);
							is_method_call = 1;
							is_assoc_call = 1;
						}
					}
				}

				// Resolve the receiver's struct type. A bare var ref looks
				// up its declaration; a member chain (`d.Base.who()`) walks
				// each hop's declared field type; a deref peels one pointer.
				Type *recv_t = NULL;
				if (self_obj->type == NODE_VAR_REF) {
					ASTNode *decl = find_decl(p, self_obj->data.var_ref.name);
					recv_t = decl ? decl->data_type : NULL;
					// `c.bump()` where c is Counter*: peel one pointer layer
					// so pointer receivers resolve like value ones.
					if (recv_t && recv_t->kind == TYPE_PTR && recv_t->inner)
						recv_t = recv_t->inner;
				} else if (self_obj->type == NODE_MEMBER_ACCESS) {
					// Walk the chain from the root, following declared
					// field types (embedding paths included).
					ASTNode *root = self_obj;
					while (root->type == NODE_MEMBER_ACCESS)
						root = root->data.member_access.object;
					if (root->type == NODE_VAR_REF ||
						root->type == NODE_DEREF) {
						if (self_obj->data_type) {
							recv_t = self_obj->data_type;
						} else {
							// Rebuild type by walking fields of the root's
							// struct through each named hop.
							ASTNode *chain[32];
							int cn = 0;
							for (ASTNode *h = self_obj;
								 h && h->type == NODE_MEMBER_ACCESS &&
									 cn < 32;
								 h = h->data.member_access.object)
								chain[cn++] = h;
							ASTNode *rdecl =
								root->type == NODE_VAR_REF
									? find_decl(
										  p,
										  root->data.var_ref.name)
									: NULL;
							Type *cur_t =
								rdecl ? rdecl->data_type : NULL;
							if (cur_t && cur_t->kind == TYPE_PTR &&
								cur_t->inner)
								cur_t = cur_t->inner;
							for (int hi = cn - 1; hi >= 0 && cur_t;
								 hi--) {
								const char *hop =
									chain[hi]->data.member_access
										.member;
								Type *next_t =
									find_field_type(p, cur_t, hop);
								cur_t = next_t;
							}
							recv_t = cur_t;
						}
					}
					if (recv_t && recv_t->kind == TYPE_PTR && recv_t->inner)
						recv_t = recv_t->inner;
				} else if (self_obj->type == NODE_DEREF) {
					ASTNode *inner = self_obj->data.deref.expr;
					if (inner->type == NODE_VAR_REF) {
						ASTNode *decl = find_decl(p, inner->data.var_ref.name);
						if (decl && decl->data_type &&
							decl->data_type->kind == TYPE_PTR &&
							decl->data_type->inner &&
							decl->data_type->inner->kind == TYPE_STRUCT) {
							recv_t = decl->data_type->inner;
						}
					}
				}
				if (recv_t && recv_t->kind == TYPE_STRUCT && recv_t->name) {
					struct_name = recv_t->name;
					is_method_call = 1;
				}
			}
			// ---------------------------------------------

			ASTNode *call = arena_alloc(p->arena, sizeof(ASTNode));
			call->type = NODE_CALL;
			call->line = p->prev.line;

			// Parse arguments: positional `expr` or named `name: expr`.
			// Named args carry their label on the node (var_decl.name);
			// codegen matches them to parameters by name and reorders.
			ASTNode *head = NULL;
			ASTNode **tail = &head;
			while (p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF) {
				ASTNode *arg = NULL;
				if (p->cur.type == TOK_IDENTIFIER &&
					lexer_peek(p->lexer).type == TOK_COLON) {
					char *label = p->cur.text;
					advance(p); // name
					advance(p); // ':'
					arg = parse_expr(p);
					if (arg) {
						arg->has_arg_label = 1;
						arg->arg_label = arena_strdup(p->arena, label);
					}
				} else {
					arg = parse_expr(p);
				}
				*tail = arg;
				if (arg) {
					tail = &(*tail)->next;
					if (p->cur.type == TOK_COMMA)
						advance(p);
				} else
					break;
			}
			consume(p, TOK_RPAREN, "Expected ')' after arguments");

			// --- NEW: Rewrite AST for Method Calls ---
			if (is_method_call && struct_name) {
				// 1. Mangle the name: "User" + "_" + "add" -> "User_add"
				// (arena-owned: mangled names live as long as the AST)
				size_t len = strlen(struct_name) + strlen(method_name) + 4;
				char *mangled = arena_alloc(p->arena, len);
				snprintf(mangled, len, "%s__%s", struct_name, method_name);

				// 2. Change Callee to a simple VAR_REF (Function Name)
				ASTNode *new_callee = arena_alloc(p->arena, sizeof(ASTNode));
				new_callee->type = NODE_VAR_REF;
				new_callee->data.var_ref.name = mangled;
				call->data.call.callee = new_callee;

				// 3. Receiver binding. Kawa methods declare the receiver
				// explicitly (`fn u32 add(User a, User b)` called as
				// `k.add(a, b)` binds k->a). Inject the receiver as the
				// first argument ONLY when the declared parameter count is
				// exactly one more than the explicit argument count --
				// otherwise the explicit arguments already carry it (or
				// ignore it), and injecting would shift every parameter.
				int expl_args = 0;
				for (ASTNode *a3 = head; a3; a3 = a3->next)
					expl_args++;
				int inject_self = 0;
				if (!is_assoc_call) {
					for (int si2 = p->fn_sig_count - 1; si2 >= 0; si2--) {
						if (strcmp(p->fn_sigs[si2].name, mangled) == 0) {
							inject_self =
								p->fn_sigs[si2].nparams == expl_args + 1;
							break;
						}
					}
				}
				// Embedding promotion (IDEAS 3): `e.describe()` where
				// describe lives on an EMBEDDED Person retargets to
				// Person__describe with the receiver rewritten to
				// e.Person. Parse-time rewrite -- codegen sees an ordinary
				// method call, so the emitted code costs exactly what a
				// hand-written e.Person.describe() would.
				if (!inject_self && !is_assoc_call) {
					int outer_known = 0;
					for (int k = p->fn_sig_count - 1; k >= 0; k--) {
						if (strcmp(p->fn_sigs[k].name, mangled) == 0) {
							outer_known = 1;
							break;
						}
					}
					if (!outer_known && struct_name) {
						char decl_struct[128], path[256];
						if (promoted_method_lookup(
								p, struct_name, method_name, 0,
								decl_struct, sizeof(decl_struct),
								path, sizeof(path))) {
							size_t qlen = strlen(decl_struct) +
										  strlen(method_name) + 4;
							char *qmangled =
								arena_alloc(p->arena, qlen);
							snprintf(qmangled, qlen, "%s__%s",
									 decl_struct, method_name);
							new_callee->data.var_ref.name = qmangled;
							// Receiver: obj.<path> -- one MEMBER_ACCESS per
							// hop, innermost last.
							ASTNode *recv = self_obj;
							char hop[128];
							int off = 0;
							while (sscanf(path + off, "%127[^.]", hop) == 1) {
								ASTNode *ma = arena_alloc(
									p->arena, sizeof(ASTNode));
								ma->type = NODE_MEMBER_ACCESS;
								ma->data.member_access.object = recv;
								ma->data.member_access.member =
									arena_strdup(p->arena, hop);
								off += strlen(hop);
								if (path[off] == '.')
									off++;
								recv = ma;
							}
							self_obj = recv;
							// Receiver binds to the promoted method's
							// declared parameter.
							inject_self = 1;
						}
					}
				}
				if (inject_self) {
					ASTNode *self_arg = self_obj;
					self_arg->next = head;
					call->data.call.args = self_arg;
				} else {
					call->data.call.args = head;
				}
			} else {
				// Standard function call behavior
				call->data.call.callee = expr;
				call->data.call.args = head;
			}

			// Stamp the call's type from the callee's declared return
			// type, so `let x = f()` infers the real type (f32 stays f32)
			// instead of falling back to u32 and truncating the value.
			{
				ASTNode *leaf = call->data.call.callee;
				while (leaf && leaf->type == NODE_MEMBER_ACCESS)
					leaf = leaf->data.member_access.object;
				const char *nm =
					(leaf && leaf->type == NODE_VAR_REF)
						? leaf->data.var_ref.name
						: NULL;
				if (nm) {
					for (int si = p->fn_sig_count - 1; si >= 0; si--) {
						if (strcmp(p->fn_sigs[si].name, nm) == 0) {
							// Generic template: concrete return type is the
							// first argument's type (single-T rule).
							if (p->fn_sigs[si].ret->kind == TYPE_STRUCT &&
								p->fn_sigs[si].ret->name &&
								strlen(p->fn_sigs[si].ret->name) == 1) {
								call->data_type =
									call->data.call.args
										? call->data.call.args->data_type
										: NULL;
							} else {
								call->data_type = p->fn_sigs[si].ret;
							}
							break;
						}
					}
				}
			}
			// -----------------------------------------

			expr = call;
		} else {
			break;
		}
	}
	return expr;
}

static ASTNode *parse_binop_rhs(Parser *p, int expr_prec, ASTNode *lhs) {
	while (1) {
		int tok_prec = -1;
		if (p->cur.type == TOK_ANDAND)
			tok_prec = 3;
		if (p->cur.type == TOK_OROR)
			tok_prec = 2;
		if (p->cur.type == TOK_PIPE)
			tok_prec = 4; // bitwise: | < ^ < & < compare (C convention)
		if (p->cur.type == TOK_CARET)
			tok_prec = 6;
		if (p->cur.type == TOK_AMP)
			tok_prec = 7;
		if (p->cur.type == TOK_PLUS || p->cur.type == TOK_MINUS)
			tok_prec = 10;
		if (p->cur.type == TOK_SHL || p->cur.type == TOK_SHR)
			tok_prec = 9; // below +/-, above & (C convention)
		if (p->cur.type == TOK_STAR || p->cur.type == TOK_SLASH ||
			p->cur.type == TOK_PERCENT)
			tok_prec = 20;
		if (p->cur.type == TOK_LANGLE || p->cur.type == TOK_RANGLE ||
			p->cur.type == TOK_ISEQ || p->cur.type == TOK_NOTEQ ||
			p->cur.type == TOK_LEQ || p->cur.type == TOK_REQ)
			tok_prec = 5;
		if (p->cur.type == TOK_TILDE_EQ)
			tok_prec = 2;
		if (p->cur.type == TOK_RECV && lhs) {
			// `ch <- v` channel send: the RECV token in operator position
			// after an expression is the send arrow.
			advance(p);
			ASTNode *n = arena_alloc(p->arena, sizeof(ASTNode));
			n->type = NODE_SEND;
			n->data.send.chan = lhs;
			n->data.send.value = parse_expr(p);
			return n;
		}
		if (p->cur.type == TOK_QUESTION)
			tok_prec = 1; // ternary: lowest, checked below
		if (tok_prec < expr_prec)
			return lhs;
		// Ternary: lowest precedence, right-associative. `a ? b : c ? d
		// : e` parses as `a ? b : (c ? d : e)` -- the else branch is
		// itself parsed as a full expression.
		if (p->cur.type == TOK_QUESTION) {
			advance(p);
			ASTNode *then_expr = parse_expr(p);
			consume(p, TOK_COLON, "Expected ':' in ternary");
			ASTNode *else_expr = parse_expr(p);
			ASTNode *ternary = arena_alloc(p->arena, sizeof(ASTNode));
			ternary->type = NODE_TERNARY;
			ternary->data.ternary.cond = lhs;
			ternary->data.ternary.then_expr = then_expr;
			ternary->data.ternary.else_expr = else_expr;
			// The ternary's type is its arms' type (they must agree);
			// without this, `let s = c ? "a" : "b"` inferred u32 and the
			// {ptr,len} view failed every later coercion.
			ternary->data_type = then_expr->data_type
									 ? then_expr->data_type
									 : else_expr->data_type;
			lhs = ternary;
			continue;
		}
		int op = p->cur.type;
		advance(p);
		// Precedence climbing: parse the RHS with strictly higher
		// precedence so `a - b - c` groups left and `a || b == c` binds
		// the comparison into the RHS of ||.
		ASTNode *rhs = parse_binop_rhs(p, tok_prec + 1, parse_unary(p));
		if (op == TOK_TILDE_EQ) {
			ASTNode *pour = arena_alloc(p->arena, sizeof(ASTNode));
			pour->type = NODE_SET_POUR;
			pour->data.set_pour.target = lhs;
			pour->data.set_pour.value = rhs;
			lhs = pour;
		} else {
			ASTNode *bin = arena_alloc(p->arena, sizeof(ASTNode));
			bin->type = NODE_BINARY_OP;
			bin->data.bin_op.op = op;
			bin->data.bin_op.left = lhs;
			bin->data.bin_op.right = rhs;
			switch (op) {
			case TOK_LANGLE: case TOK_RANGLE: case TOK_LEQ:
			case TOK_REQ: case TOK_ISEQ: case TOK_NOTEQ:
			case TOK_ANDAND: case TOK_OROR:
				bin->data_type = bool_result_type(p);
				break;
			default:
				// Arithmetic/bitwise: promote to the wider operand type.
				// Shifts take their width from the LHS only.
				if (op == TOK_SHL || op == TOK_SHR)
					bin->data_type = lhs->data_type;
				else
					bin->data_type =
						unify_types(p, lhs->data_type, rhs->data_type);
				break;
			}
			lhs = bin;
		}
	}
}

static ASTNode *parse_expr(Parser *p) {
	ASTNode *lhs = parse_unary(p); // Changed from parse_postfix(p)
	return parse_binop_rhs(p, 0, lhs);
}

static ASTNode *parse_block(Parser *p) {
	consume(p, TOK_LBRACE, "Expected '{'");
	ASTNode *block = arena_alloc(p->arena, sizeof(ASTNode));
	block->type = NODE_BLOCK;
	ASTNode **tail = &block->data.block.stmts;
	while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
		*tail = parse_statement(p);
		if (*tail)
			tail = &(*tail)->next;
	}
	consume(p, TOK_RBRACE, "Expected '}'");
	return block;
}

static void register_dependencies(Parser *p, ASTNode *expr,
								  ASTNode *orbit_node) {
	if (!expr)
		return;
	if (expr->type == NODE_VAR_REF) {
		ASTNode *src = find_decl(p, expr->data.var_ref.name);
		if (src) {
			Dependency *dep = arena_alloc(p->arena, sizeof(Dependency));
			dep->dependent_node = orbit_node;
			dep->logic_expr = orbit_node->data.var_decl.init;
			dep->next = src->dependents;
			src->dependents = dep;
		}
	}
	if (expr->type == NODE_BINARY_OP) {
		register_dependencies(p, expr->data.bin_op.left, orbit_node);
		register_dependencies(p, expr->data.bin_op.right, orbit_node);
	}
}

static ASTNode *parse_grind(Parser *p) {
	consume(p, TOK_LBRACE, "Expected '{' after grind");
	consume(p, TOK_RETURN, "Grind expects return");
	ASTNode *val = parse_expr(p);
	consume(p, TOK_SEMICOLON, ";");
	consume(p, TOK_RBRACE, "}");
	if (val->type == NODE_BINARY_OP &&
		val->data.bin_op.left->type == NODE_LITERAL &&
		val->data.bin_op.right->type == NODE_LITERAL) {
		long long l = val->data.bin_op.left->data.literal.i64_val;
		long long r = val->data.bin_op.right->data.literal.i64_val;
		long long res = 0;
		if (val->data.bin_op.op == TOK_STAR)
			res = l * r;
		else if (val->data.bin_op.op == TOK_PLUS)
			res = l + r;
		else if (val->data.bin_op.op == TOK_MINUS)
			res = l - r;
		else if (val->data.bin_op.op == TOK_SLASH)
			res = r != 0 ? l / r : 0;
		ASTNode *folded = arena_alloc(p->arena, sizeof(ASTNode));
		folded->type = NODE_LITERAL;
		folded->data.literal.i64_val = res;
		folded->data.literal.i_val = (int)res;
		folded->data_type = val->data_type;
		return folded;
	}
	return val;
}

static ASTNode *parse_statement(Parser *p) {
	int stmt_line = p->cur.line;
	ASTNode *result = parse_statement_inner(p, stmt_line);
	// Stamp every statement with its starting line -- debug locations and
	// runtime diagnostics read this.
	if (result && result->line == 0)
		result->line = stmt_line;
	return result;
}

static ASTNode *parse_statement_inner(Parser *p, int stmt_line) {
	(void)stmt_line;
	if (p->cur.type == TOK_ERROR) {
		synchronize(p);
		return NULL;
	}
	// A case/default label terminates the enclosing switch body's statement
	// list -- it belongs to parse_switch, not to whatever statement loop is
	// running. Returning NULL here lets every body loop stop cleanly.
	if (p->cur.type == TOK_CASE || p->cur.type == TOK_DEFAULT)
		return NULL;

	int is_c_style_decl = 0;
	if (p->cur.type == TOK_LBRACKET) {
		// Array type declaration: [N]T name = ... where N is a literal
		// or a const identifier. `[]T name` is the slice form.
		Lexer temp = *p->lexer;		   // Clone lexer state
		Token t1 = lexer_next(&temp);  // length literal or const name
		if (t1.type == TOK_RBRACKET) {
			// `[]T name` slice form: ']' followed by an element type.
			if (is_type_token(lexer_peek(&temp).type))
				is_c_style_decl = 1;
		}
		Token t2 = lexer_next(&temp); // ']'
		Token t3 = lexer_next(&temp); // element type
		int len_ok = (t1.type == TOK_INT_LIT) ||
					 (t1.type == TOK_IDENTIFIER &&
					  find_decl_is_const(p, t1.text));
		if (len_ok && t2.type == TOK_RBRACKET && is_type_token(t3.type))
			is_c_style_decl = 1;
	} else if (is_type_token(p->cur.type)) {
		Token next = lexer_peek(p->lexer);
		if (next.type == TOK_IDENTIFIER || next.type == TOK_STAR)
			is_c_style_decl = 1;
		if (p->cur.type == TOK_IDENTIFIER &&
			strcmp(p->cur.text, "chan") == 0 &&
			next.type == TOK_LANGLE)
			is_c_style_decl = 1;
	}

	if (p->cur.type == TOK_IDENTIFIER &&
		strcmp(p->cur.text, "unchecked") == 0 &&
		lexer_peek(p->lexer).type == TOK_LBRACE) {
		// unchecked { ... } (IDEAS 2.3): no bounds checks are generated
		// inside, even in debug builds. Contextual keyword -- user code
		// can still name a variable `unchecked`.
		ASTNode *n = arena_alloc(p->arena, sizeof(ASTNode));
		n->type = NODE_UNCHECKED_BLOCK;
		advance(p);
		consume(p, TOK_LBRACE, "Expected '{' after unchecked");
		n->data.block.stmts = NULL;
		if (p->cur.type != TOK_RBRACE) {
			// Reuse parse_block's loop via a synthetic block body.
			ASTNode *body = arena_alloc(p->arena, sizeof(ASTNode));
			body->type = NODE_BLOCK;
			body->data.block.stmts = NULL;
			ASTNode **tail = &body->data.block.stmts;
			while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
				*tail = parse_statement(p);
				if (*tail)
					tail = &(*tail)->next;
			}
			n->data.block.stmts = body;
		}
		consume(p, TOK_RBRACE, "Expected '}' to close unchecked");
		return n;
	}
	if (p->cur.type == TOK_ASM) {
		// Statement-level asm: `asm { "..." : "..." }` with no trailing
		// ';' required (block form, like IDEAS 2.8 shows). The expression
		// branch in parse_unary still covers asm in value position.
		ASTNode *n = arena_alloc(p->arena, sizeof(ASTNode));
		advance(p);
		n->type = NODE_ASM;
		consume(p, TOK_LBRACE, "Expected '{' after asm");
		if (p->cur.type != TOK_STRING_LIT)
			report_error(p, "Expected instruction string");
		char *tmpl = p->cur.text;
		advance(p);
		while (p->cur.type == TOK_STRING_LIT) {
			size_t len = strlen(tmpl) + strlen(p->cur.text) + 2;
			char *joined = arena_alloc(p->arena, len);
			snprintf(joined, len, "%s\n%s", tmpl, p->cur.text);
			tmpl = joined;
			advance(p);
		}
		char *constraints = NULL;
		if (p->cur.type == TOK_COLON) {
			advance(p);
			if (p->cur.type != TOK_STRING_LIT)
				report_error(p, "Expected constraint string");
			constraints = p->cur.text;
			advance(p);
		}
		consume(p, TOK_RBRACE, "Expected '}' to close asm");
		n->data.asm_block.asm_template = tmpl;
		n->data.asm_block.constraints = constraints;
		return n;
	}
	if (p->cur.type == TOK_LET || p->cur.type == TOK_CONST ||
		p->cur.type == TOK_ORBIT || is_c_style_decl) {
		int is_orbit = (p->cur.type == TOK_ORBIT);
		int is_const = (p->cur.type == TOK_CONST);

		Type *type = NULL;
		if (is_c_style_decl) {
			type = parse_type(p);
		} else {
			advance(p); // step over let/const/orbit itself
			// Optional explicit type: `const u32 X = ...`. A type token here
			// must be followed by the variable's identifier (or a pointer
			// star), otherwise it IS the variable name (`const x = ...`).
			if (is_type_token(p->cur.type)) {
				Token after = lexer_peek(p->lexer);
				if (p->cur.type != TOK_IDENTIFIER ||
					after.type == TOK_IDENTIFIER || after.type == TOK_STAR)
					type = parse_type(p);
			}
		}

		char *name = p->cur.text;
		consume(p, TOK_IDENTIFIER, "Expected variable name");

		if (!is_c_style_decl && p->cur.type == TOK_COLON) {
			advance(p);
			type = parse_type(p);
		}

		ASTNode *init = NULL;
		if (is_orbit) {
			consume(p, TOK_COLON_ASSIGN, "Expected ':=' for orbit");
			init = parse_expr(p);
		} else {
			if (p->cur.type == TOK_ASSIGN) {
				advance(p);
				// Optional `comptime` marker (IDEAS 2.1): documents that
				// the initializer must fold. `const X = comptime fib(10);`
				// The marker is advisory -- any const expr that folds,
				// does -- but it makes intent explicit like Zig's.
				if (p->cur.type == TOK_IDENTIFIER &&
					strcmp(p->cur.text, "comptime") == 0 &&
					lexer_peek(p->lexer).type != TOK_COLON_ASSIGN)
					advance(p);
				init = parse_expr(p);
			}
		}

		// A builtin constructor inherits the DECLARED type when its own
		// inference can't know it (`chan<i32> ch = make_chan(4)`).
		if (type && !is_orbit && init && init->type == NODE_CALL &&
			!init->data_type)
			init->data_type = type;
		// FIX: Type Inference
		if (!type) {
			if (init && init->data_type) {
				type = init->data_type; // Infer from expression
			} else if (init && init->type == NODE_CALL && is_const) {
				// Comptime call: the parser has no return-type table entry
				// for pure fns called at comptime... actually fn_sigs covers
				// it; fall through to u32 only when unknown.
				type = arena_alloc(p->arena, sizeof(Type));
				type->kind = TYPE_I64;
			} else {
				type = arena_alloc(p->arena, sizeof(Type));
				type->kind = TYPE_U32; // Fallback
			}
		}

		consume(p, TOK_SEMICOLON, "Expected ';'");

		ASTNode *node = arena_alloc(p->arena, sizeof(ASTNode));
		node->type = NODE_VAR_DECL;
		node->data.var_decl.name = name;
		node->data.var_decl.init = init;
		node->data.var_decl.is_orbit = is_orbit;
		node->data.var_decl.is_const = is_const;
		node->data_type = type;

		if (p->decl_count < 256) {
			p->decls[p->decl_count].name = name;
			p->decls[p->decl_count].node = node;
			p->decl_count++;
		}
		if (is_orbit)
			register_dependencies(p, init, node);
		return node;
	}
	if (p->cur.type == TOK_RETURN) {
		advance(p);
		ASTNode *ret = arena_alloc(p->arena, sizeof(ASTNode));
		ret->type = NODE_RETURN;
		ret->data.ret_stmt.expr =
			(p->cur.type == TOK_SEMICOLON) ? NULL : parse_expr(p);
		consume(p, TOK_SEMICOLON, "Expected ';'");
		return ret;
	}
	if (p->cur.type == TOK_IF) {
		advance(p);
		consume(p, TOK_LPAREN, "Expected '(' after 'if'");
		ASTNode *cond = parse_expr(p);
		consume(p, TOK_RPAREN, "Expected ')' after condition");

		ASTNode *then_block = parse_statement(p);
		ASTNode *else_block = NULL;

		if (p->cur.type == TOK_ELSE) {
			advance(p);
			else_block = parse_statement(p);
		}

		ASTNode *node = arena_alloc(p->arena, sizeof(ASTNode));
		node->type = NODE_IF;
		node->data.if_stmt.cond = cond;
		node->data.if_stmt.then_block = then_block;
		node->data.if_stmt.else_block = else_block;
		return node;
	}
	if (p->cur.type == TOK_WHILE) {
		advance(p);
		consume(p, TOK_LPAREN, "(");
		ASTNode *cond = parse_expr(p);
		consume(p, TOK_RPAREN, ")");
		ASTNode *body = parse_block(p);
		ASTNode *loop = arena_alloc(p->arena, sizeof(ASTNode));
		loop->type = NODE_WHILE;
		loop->data.while_stmt.cond = cond;
		loop->data.while_stmt.body = body;
		return loop;
	}
	if (p->cur.type == TOK_FOR) {
		advance(p);
		consume(p, TOK_LPAREN, "(");
		// for init; cond; step { body } -- any clause may be empty.
		// Declarations and expression statements consume their own ';' via
		// parse_statement/parse_expr_stmt_tail; an empty init consumes it
		// here. After this block p->cur is always just past the first ';'.
		ASTNode *init = NULL;
		if (p->cur.type != TOK_SEMICOLON) {
			init = parse_var_or_expr_no_semi(p);
		} else
			consume(p, TOK_SEMICOLON, ";");

		ASTNode *cond = NULL;
		if (p->cur.type != TOK_SEMICOLON)
			cond = parse_expr(p);
		consume(p, TOK_SEMICOLON, "Expected ';' after for condition");

		ASTNode *step = NULL;
		if (p->cur.type != TOK_RPAREN)
			step = parse_expr_stmt_tail(p, parse_expr(p), 0);
		consume(p, TOK_RPAREN, ")");

		ASTNode *body = parse_block(p);

		ASTNode *loop = arena_alloc(p->arena, sizeof(ASTNode));
		loop->type = NODE_FOR;
		loop->data.for_stmt.init = init;
		loop->data.for_stmt.cond = cond;
		loop->data.for_stmt.step = step;
		loop->data.for_stmt.body = body;
		return loop;
	}
	if (p->cur.type == TOK_SWITCH) {
		advance(p);
		consume(p, TOK_LPAREN, "(");
		ASTNode *value = parse_expr(p);
		consume(p, TOK_RPAREN, ")");
		consume(p, TOK_LBRACE, "Expected '{' after switch value");

		ASTNode *cases_head = NULL;
		ASTNode **cases_tail = &cases_head;

		int seen_default = 0;
		while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
			int case_line = p->cur.line; // `case`/`default` keyword line
			int is_default = 0;
			ASTNode *case_expr = NULL;
			if (p->cur.type == TOK_CASE) {
				advance(p);
				case_expr = parse_expr(p);
			} else if (p->cur.type == TOK_DEFAULT) {
				advance(p);
				is_default = 1;
				if (seen_default)
					report_error(p, "Duplicate 'default' in switch");
				seen_default = 1;
			} else {
				report_error(p,
							 "Expected 'case' or 'default' inside switch");
				break;
			}
			consume(p, TOK_COLON, "Expected ':' after case label");

			// Body: statements until the next case/default/close. C
			// fallthrough semantics come free -- consecutive cases just run
			// into each other because we don't emit anything between them.
			ASTNode *body_head = NULL;
			ASTNode **body_tail = &body_head;
			while (p->cur.type != TOK_CASE && p->cur.type != TOK_DEFAULT &&
				   p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
				ASTNode *s = parse_statement(p);
				if (!s)
					break;
				*body_tail = s;
				body_tail = &s->next;
			}

			ASTNode *cs = arena_alloc(p->arena, sizeof(ASTNode));
			cs->type = NODE_CASE;
			cs->line = case_line;
			cs->data.case_stmt.expr = case_expr;
			// Wrap the statement chain in a NODE_BLOCK: codegen_stmt
			// dispatches one node at a time, and only NODE_BLOCK iterates.
			ASTNode *body_block = arena_alloc(p->arena, sizeof(ASTNode));
			body_block->type = NODE_BLOCK;
			body_block->data.block.stmts = body_head;
			cs->data.case_stmt.body = body_block;

			*cases_tail = cs;
			cases_tail = &cs->next;
			(void)is_default; // default == NULL expr
		}
		consume(p, TOK_RBRACE, "Expected '}' to close switch");

		ASTNode *sw = arena_alloc(p->arena, sizeof(ASTNode));
		sw->type = NODE_SWITCH;
		sw->data.switch_stmt.value = value;
		sw->data.switch_stmt.cases = cases_head;
		return sw;
	}
	if (p->cur.type == TOK_BREAK) {
		advance(p);
		consume(p, TOK_SEMICOLON, "Expected ';' after 'break'");
		ASTNode *brk = arena_alloc(p->arena, sizeof(ASTNode));
		brk->type = NODE_BREAK;
		return brk;
	}
	if (p->cur.type == TOK_CONTINUE) {
		advance(p);
		consume(p, TOK_SEMICOLON, "Expected ';' after 'continue'");
		ASTNode *cont = arena_alloc(p->arena, sizeof(ASTNode));
		cont->type = NODE_CONTINUE;
		return cont;
	}
	if (p->cur.type == TOK_BATCH) {
		advance(p);
		consume(p, TOK_LPAREN, "(");
		char *iter = p->cur.text;
		consume(p, TOK_IDENTIFIER, "Expected iterator var");
		consume(p, TOK_IN, "Expected 'in'");
		ASTNode *coll = parse_expr(p);
		consume(p, TOK_RPAREN, ")");
		ASTNode *batch = arena_alloc(p->arena, sizeof(ASTNode));
		batch->type = NODE_BATCH;
		batch->data.batch.iterator_var = iter;
		batch->data.batch.collection = coll;
		batch->data.batch.body = parse_block(p);
		return batch;
	}
	if (p->cur.type == TOK_DEFER) {
		advance(p);
		ASTNode *defer = arena_alloc(p->arena, sizeof(ASTNode));
		defer->type = NODE_DEFER;
		defer->data.defer.stmt = parse_statement(p);
		return defer;
	}
	if (p->cur.type == TOK_DROP) {
		advance(p);
		ASTNode *drop = arena_alloc(p->arena, sizeof(ASTNode));
		drop->type = NODE_DROP;
		drop->data.drop.val = parse_expr(p);
		consume(p, TOK_SEMICOLON, ";");
		return drop;
	}
	if (p->cur.type == TOK_IDENTIFIER &&
		strcmp(p->cur.text, "select") == 0 &&
		lexer_peek(p->lexer).type == TOK_LBRACE) {
		// select { case v = <- ch: ... ... default: ... } (IDEAS 3).
		// Polls each channel in order; first ready case runs. With no
		// ready case and no default, yields (drop{}) and re-polls --
		// cooperative blocking, zero atomics on the single thread.
		advance(p); // 'select' (contextual keyword)
		consume(p, TOK_LBRACE, "{");
		ASTNode *n = arena_alloc(p->arena, sizeof(ASTNode));
		n->type = NODE_SELECT;
		struct SelectCase *head = NULL, **tail = &head;
		while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
			if (p->cur.type == TOK_DEFAULT) {
				advance(p);
				consume(p, TOK_COLON, ":");
				n->data.select_stmt.has_default = 1;
				n->data.select_stmt.default_body =
					parse_block(p);
				continue;
			}
			int case_is_kw = p->cur.type == TOK_CASE ||
							 (p->cur.type == TOK_IDENTIFIER &&
							  strcmp(p->cur.text, "case") == 0);
			if (!case_is_kw) {
				report_error(p,
							 "Expected 'case' or 'default' in select");
				break;
			}
			advance(p); // 'case'
			char *var_name = NULL;
			Type *var_t = NULL;
			if (p->cur.type == TOK_IDENTIFIER &&
				strcmp(p->cur.text, "default") != 0) {
				var_name = p->cur.text;
				advance(p);
			} else {
				report_error(p, "Expected variable name in select case");
			}
			// `v = <- ch` or `v := <- ch`
			if (p->cur.type == TOK_COLON_ASSIGN) {
				advance(p);
			} else if (p->cur.type == TOK_ASSIGN) {
				advance(p);
			} else {
				report_error(p, "Expected '=' in select case");
			}
			consume(p, TOK_RECV, "Expected '<-' in select case");
			ASTNode *chan_expr = parse_expr(p);
			consume(p, TOK_COLON, ":");
			// Bare channel names arrive untyped here (globals are not in
			// p->decls); resolve against the symbol table so codegen's
			// chan<T> assertion holds. Same fallback NODE_RECV uses.
			if (!chan_expr->data_type &&
				chan_expr->type == NODE_VAR_REF) {
				for (int di = 0; di < p->decl_count; di++) {
					if (strcmp(p->decls[di].name,
							   chan_expr->data.var_ref.name) == 0 &&
						p->decls[di].node->data_type) {
						chan_expr->data_type =
							p->decls[di].node->data_type;
						break;
					}
				}
			}
			struct SelectCase *cs =
				arena_alloc(p->arena, sizeof(struct SelectCase));
			cs->chan = chan_expr;
			cs->body = parse_block(p);
			cs->next = NULL;
			// Bind the received value as a fresh const-like decl.
			if (var_name) {
				Type *ct =
					chan_expr->data_type ? chan_expr->data_type->inner
										 : NULL;
				ASTNode *vd =
					arena_alloc(p->arena, sizeof(ASTNode));
				vd->type = NODE_VAR_DECL;
				vd->data.var_decl.name = var_name;
				vd->data_type = ct;
				var_t = ct;
				cs->var_decl = vd;
			} else {
				cs->var_decl = NULL;
			}
			(void)var_t;
			*tail = cs;
			tail = &cs->next;
		}
		consume(p, TOK_RBRACE, "}");
		n->data.select_stmt.cases = head;
		return n;
	}
	if (p->cur.type == TOK_FILTER) {
		advance(p);
		ASTNode *filt = arena_alloc(p->arena, sizeof(ASTNode));
		filt->type = NODE_FILTER;
		filt->data.filter.try_block = parse_block(p);
		consume(p, TOK_DREGS, "Expected 'dregs'");
		consume(p, TOK_LPAREN, "(");
		filt->data.filter.err_var = p->cur.text;
		consume(p, TOK_IDENTIFIER, "Err var");
		// Typed payload: `dregs (e: ParseErr)`. Optional -- a bare `(e)`
		// keeps the legacy i32 slot so existing code is untouched.
		if (p->cur.type == TOK_COLON) {
			advance(p);
			filt->data.filter.err_type = parse_type(p);
		}
		consume(p, TOK_RPAREN, ")");
		filt->data.filter.catch_block = parse_block(p);
		return filt;
	}
	if (p->cur.type == TOK_PRESS) {
		advance(p);
		char *name = p->cur.text;
		consume(p, TOK_IDENTIFIER, "Name");
		consume(p, TOK_ASSIGN, "=");
		ASTNode *target = parse_expr(p);
		consume(p, TOK_SEMICOLON, ";");
		ASTNode *press = arena_alloc(p->arena, sizeof(ASTNode));
		press->type = NODE_PRESS;
		press->data.press.name = name;
		press->data.press.target = target;
		return press;
	}
	if (p->cur.type == TOK_ALIAS) {
		advance(p);
		char *name = p->cur.text;
		consume(p, TOK_IDENTIFIER, "Alias name");
		consume(p, TOK_ASSIGN, "=");

		// [FIX] Capture the type!
		Type *target_type = parse_type(p);

		consume(p, TOK_SEMICOLON, ";");
		ASTNode *alias = arena_alloc(p->arena, sizeof(ASTNode));
		alias->type = NODE_ALIAS;
		alias->data.alias.name = name;

		// [FIX] Store the target type in the node's data_type field
		alias->data_type = target_type;

		return alias;
	}
	if (p->cur.type == TOK_LBRACE)
		return parse_block(p);
	ASTNode *expr = parse_expr(p);
	return parse_expr_stmt_tail(p, expr, 1);
}

// for-loop init clause: a variable declaration or an expression statement.
// Both consume their own trailing ';' (the for-parser consumes it only for
// an empty init, keeping "p is past the first ';'" invariant afterwards).
static ASTNode *parse_var_or_expr_no_semi(Parser *p) {
	return parse_statement(p);
}
// token is an assignment operator, build NODE_ASSIGN (desugaring compound
// ops to binops). Consumes the trailing ';' only when requested -- for
// clauses are terminated by ';' / ')' respectively.
static ASTNode *parse_expr_stmt_tail(Parser *p, ASTNode *expr,
									 int need_semi) {
	if (p->cur.type == TOK_ASSIGN || p->cur.type == TOK_PLUS_EQ ||
		p->cur.type == TOK_MINUS_EQ || p->cur.type == TOK_STAR_EQ ||
		p->cur.type == TOK_SLASH_EQ || p->cur.type == TOK_AND_EQ ||
		p->cur.type == TOK_OR_EQ || p->cur.type == TOK_XOR_EQ ||
		p->cur.type == TOK_SHL_EQ || p->cur.type == TOK_SHR_EQ) {
		int op = p->cur.type;
		advance(p); // Eat '=' / '+=' / '-=' / '*=' / '/='

		// Validate LHS is an L-Value
		if (expr->type != NODE_VAR_REF && expr->type != NODE_MEMBER_ACCESS &&
			expr->type != NODE_INDEX && expr->type != NODE_DEREF) {
			report_error(p,
						 "Invalid assignment target. Must be variable, field, "
						 "element or dereference.");
		}

		ASTNode *assign = arena_alloc(p->arena, sizeof(ASTNode));
		assign->type = NODE_ASSIGN;
		assign->data.assign.target = expr;

		ASTNode *value = parse_expr(p);
		if (op == TOK_ASSIGN) {
			assign->data.assign.value = value;
		} else {
			// Desugar `x += v` into `x = x + v`. The codegen re-evaluates the
			// target address once, so this stays a single store.
			int bin_op;
			switch (op) {
			case TOK_PLUS_EQ:
				bin_op = TOK_PLUS;
				break;
			case TOK_MINUS_EQ:
				bin_op = TOK_MINUS;
				break;
			case TOK_STAR_EQ:
				bin_op = TOK_STAR;
				break;
			case TOK_AND_EQ:
				bin_op = TOK_AMP;
				break;
			case TOK_OR_EQ:
				bin_op = TOK_PIPE;
				break;
			case TOK_XOR_EQ:
				bin_op = TOK_CARET;
				break;
			case TOK_SHL_EQ:
				bin_op = TOK_SHL;
				break;
			case TOK_SHR_EQ:
				bin_op = TOK_SHR;
				break;
			default:
				bin_op = TOK_SLASH;
				break;
			}
			ASTNode *bin = arena_alloc(p->arena, sizeof(ASTNode));
			bin->type = NODE_BINARY_OP;
			bin->data.bin_op.op = bin_op;
			bin->data.bin_op.left = expr;
			bin->data.bin_op.right = value;
			bin->data_type = expr->data_type;
			assign->data.assign.value = bin;
		}
		if (need_semi)
			consume(p, TOK_SEMICOLON, "Expected ';'");
		return assign;
	}
	if (need_semi)
		consume(p, TOK_SEMICOLON, "Expected ';'");
	return expr;
}
void parse_function(Parser *p, ASTNode ***tail, char *prefix) {
	// Attributes directly above `fn` attach to it: #[test], #[ignore].
	int attr_is_test = 0;
	int attr_is_ignored = 0;
	while (p->cur.type == TOK_ATTRIBUTE) {
		if (strcmp(p->cur.text, "test") == 0)
			attr_is_test = 1;
		else if (strcmp(p->cur.text, "ignore") == 0)
			attr_is_ignored = 1;
		advance(p);
	}
	int is_pure = (p->cur.type == TOK_PURE);
	if (is_pure)
		advance(p);
	consume(p, TOK_FN, "Expected 'fn'");

	int is_drip = (p->cur.type == TOK_DRIP);
	if (is_drip)
		advance(p);

	Type *ret_type = NULL;
	if (is_type_token(p->cur.type)) {
		Token next = lexer_peek(p->lexer);
		if (next.type == TOK_IDENTIFIER) {
			ret_type = parse_type(p);
		}
	}
	char *func_name = p->cur.text;
	if (prefix) {
		size_t len = strlen(prefix) + strlen(func_name) + 4;
		char *mangled = arena_alloc(p->arena, len);
		snprintf(mangled, len, "%s__%s", prefix, func_name);
		func_name = mangled;
	}
	consume(p, TOK_IDENTIFIER, "Expected func name");
	consume(p, TOK_LPAREN, "Expected '('");

	ASTNode *args_head = NULL;
	ASTNode **args_tail = &args_head;
	while (p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF) {
		Type *arg_type = NULL;
		char *arg_name = NULL;

		// `self` receiver: bare `self` takes the impl struct by value,
		// `self*` by pointer. The parameter is materialized under the name
		// "self" so bodies read like Rust (`self.age`, `self->age`) with
		// zero ABI difference from an explicit first parameter -- call-site
		// injection already handles exactly this shape.
		if (p->cur.type == TOK_IDENTIFIER && prefix &&
			strcmp(p->cur.text, "self") == 0) {
			advance(p); // 'self'
			int by_ptr = (p->cur.type == TOK_STAR);
			if (by_ptr)
				advance(p);
			Type *st = find_struct_type_by_name(p, prefix);
			if (!st) {
				report_error(p, "`self` used outside of an impl block");
				return;
			}
			Type *t;
			if (by_ptr) {
				// Pointer receiver: TYPE_PTR -> struct type.
				t = arena_alloc(p->arena, sizeof(Type));
				t->kind = TYPE_PTR;
				t->name = NULL;
				t->inner = st;
			} else {
				t = st;
			}
			arg_type = t;
			arg_name = "self";
			// Optional explicit alias (`self* v`): renames the binding so
			// bodies can keep C-style `v->x` spellings.
			if (p->cur.type == TOK_IDENTIFIER &&
				lexer_peek(p->lexer).type != TOK_DOT) {
				arg_name = p->cur.text;
				advance(p);
			}
		} else if (is_type_token(p->cur.type)) {
			arg_type = parse_type(p);
			arg_name = p->cur.text;
			consume(p, TOK_IDENTIFIER, "Arg name");
		} else {
			arg_name = p->cur.text;
			consume(p, TOK_IDENTIFIER, "Arg name");
			consume(p, TOK_COLON, ":");
			arg_type = parse_type(p);
		}

		ASTNode *arg = arena_alloc(p->arena, sizeof(ASTNode));
		arg->type = NODE_VAR_DECL;
		arg->data.var_decl.name = arg_name;
		arg->data_type = arg_type;
		*args_tail = arg;
		args_tail = &arg->next;
		if (p->cur.type == TOK_COMMA)
			advance(p);
	}

	ASTNode *fn_node = arena_alloc(p->arena, sizeof(ASTNode));
	fn_node->type = NODE_FUNC_DECL;
	fn_node->line = p->prev.line; // `fn` keyword line, for debug info
	fn_node->data.func.is_pure = is_pure;
	fn_node->data.func.is_drip = is_drip;
	fn_node->data.func.is_test = attr_is_test;
	fn_node->data.func.is_ignored = attr_is_ignored;
	fn_node->data.func.name = func_name;
	fn_node->data.func.ret_type = ret_type;
	fn_node->data.func.args = args_head;

	consume(p, TOK_RPAREN, "Expected ')'");
	// Record the signature BEFORE parsing the body so recursive
	// `let x = name(...)` calls inside infer the declared return type.
	// Generic templates (return type is a bare `T`) are excluded: their
	// concrete type only exists per instantiation.
	if (p->fn_sig_count < 128 && ret_type) {
		int np = 0;
		for (ASTNode *a = args_head; a; a = a->next)
			np++;
		p->fn_sigs[p->fn_sig_count].name = func_name;
		p->fn_sigs[p->fn_sig_count].ret = ret_type;
		p->fn_sigs[p->fn_sig_count].nparams = np;
		p->fn_sig_count++;
	}
	// Register parameters so member/index typing (`argv[0].len`) can see
	// their declared types.
	for (ASTNode *a = args_head; a; a = a->next) {
		if (p->decl_count < 256) {
			p->decls[p->decl_count].name = a->data.var_decl.name;
			p->decls[p->decl_count].node = a;
			p->decl_count++;
		}
	}
	fn_node->data.func.body = parse_block(p);
	**tail = fn_node;
	*tail = &fn_node->next;
}
// `enum Name { A, B = 5, C }` -- a scoped set of i32 constants. Members
// desugar to plain `const i32 Name_member = v;` declarations, so codegen
// needs no new machinery: they fold like any other const, work as array
// lengths, and cost nothing at runtime. Auto-increment continues from the
// last explicit value (C rules).
static void parse_enum(Parser *p, ASTNode ***tail) {
	advance(p); // eat `enum`
	char *name = p->cur.text;
	consume(p, TOK_IDENTIFIER, "Expected enum name");
	consume(p, TOK_LBRACE, "Expected '{' after enum name");

	long next_value = 0;
	ASTNode *fields_head = NULL;
	ASTNode **fields_tail = &fields_head;
	ASTNode ***outer_tail = tail; // keep the program-chain handle handy

	while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
		char *member = p->cur.text;
		consume(p, TOK_IDENTIFIER, "Expected enum member name");

		long value = next_value;
		if (p->cur.type == TOK_ASSIGN) {
			advance(p);
			int negative = 0;
			if (p->cur.type == TOK_MINUS) {
				negative = 1;
				advance(p);
			}
			if (p->cur.type != TOK_INT_LIT) {
				report_error(p, "Enum member value must be an integer literal");
				if (p->cur.type != TOK_COMMA && p->cur.type != TOK_RBRACE)
					advance(p);
				value = next_value;
			} else {
				value = atol(p->cur.text);
				if (negative)
					value = -value;
				advance(p);
			}
		}
		next_value = value + 1;

		// Synthesize `const i32 Name_member = value;` -- the qualified name
		// keeps members of different enums from colliding.
		char full_name[256];
		snprintf(full_name, sizeof(full_name), "%s_%s", name, member);

		Type *i32_t = arena_alloc(p->arena, sizeof(Type));
		i32_t->kind = TYPE_I32;

		ASTNode *lit = arena_alloc(p->arena, sizeof(ASTNode));
		lit->type = NODE_LITERAL;
		lit->data_type = i32_t;
		lit->data.literal.i_val = (int)value;
		lit->data.literal.i64_val = value;

		ASTNode *member_decl = arena_alloc(p->arena, sizeof(ASTNode));
		member_decl->type = NODE_VAR_DECL;
		member_decl->data.var_decl.name =
			arena_alloc(p->arena, strlen(full_name) + 1);
		strcpy(member_decl->data.var_decl.name, full_name);
		member_decl->data.var_decl.init = lit;
		member_decl->data.var_decl.is_const = 1;
		member_decl->data_type = i32_t;

		// Register in the decl table so `[Color_RED]i8 buf;` and other
		// const-identifier lookups resolve.
		if (p->decl_count < 256) {
			p->decls[p->decl_count].name = member_decl->data.var_decl.name;
			p->decls[p->decl_count].node = member_decl;
			p->decl_count++;
		}

		*fields_tail = member_decl;
		fields_tail = &member_decl->next;

		// Members live on the program chain too, so kawa_compile's ordinary
		// NODE_VAR_DECL pass emits them (as folded i32 consts). The
		// NODE_ENUM_DECL marker follows them for tooling/lookup.
		**outer_tail = member_decl;
		*outer_tail = &member_decl->next;

		if (p->cur.type == TOK_COMMA)
			advance(p);
		else
			break;
	}
	consume(p, TOK_RBRACE, "Expected '}' after enum members");
	consume(p, TOK_SEMICOLON, "Expected ';' after enum");

	ASTNode *en = arena_alloc(p->arena, sizeof(ASTNode));
	en->type = NODE_ENUM_DECL;
	en->data.enum_decl.name = name;
	en->data.enum_decl.fields = fields_head;

	**outer_tail = en;
	*outer_tail = &en->next;
}

ASTNode *parse_program(Parser *p) {
	ASTNode *prog = arena_alloc(p->arena, sizeof(ASTNode));
	prog->type = NODE_PROGRAM;
	ASTNode **tail = &prog->next;

	while (p->cur.type != TOK_EOF) {
		if (p->cur.type == TOK_ERROR) {
			synchronize(p);
			continue;
		}

		if (p->cur.type == TOK_FN || p->cur.type == TOK_PURE ||
			p->cur.type == TOK_ATTRIBUTE) {
			// #[soa] belongs to a struct declaration, not a function.
			int leading_soa = 0;
			if (p->cur.type == TOK_ATTRIBUTE &&
				strcmp(p->cur.text, "soa") == 0) {
				Lexer la = *p->lexer;
				Token nt = lexer_next(&la);
				if (nt.type == TOK_STRUCT)
					leading_soa = 1;
			}
			if (!leading_soa)
				parse_function(p, &tail, NULL);
			else
				goto parse_soa_struct;
		} else if (p->cur.type == TOK_EXTERN) {
			// extern "c" fn ret name(args...);  -- a C symbol declaration.
			// No body is parsed or emitted; the linker resolves it. Exact
			// prototypes matter (a wrong one is UB), so the declared types
			// flow straight into the LLVM function type.
			advance(p);
			char *cc = NULL;
			if (p->cur.type == TOK_STRING_LIT) {
				cc = p->cur.text;
				advance(p);
			}
			consume(p, TOK_FN, "Expected 'fn' after extern");

			Type *ret_type = NULL;
			if (is_type_token(p->cur.type)) {
				Token next = lexer_peek(p->lexer);
				if (next.type == TOK_IDENTIFIER)
					ret_type = parse_type(p);
			}
			char *fn_name = p->cur.text;
			consume(p, TOK_IDENTIFIER, "Expected function name");
			consume(p, TOK_LPAREN, "Expected '('");

			ASTNode *args_head = NULL;
			ASTNode **args_tail = &args_head;
			int is_variadic = 0;
			while (p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF) {
				if (p->cur.type == TOK_DOT) {
					// `...` lexes as three DOT tokens.
					is_variadic = 1;
					while (p->cur.type == TOK_DOT)
						advance(p);
					break;
				}
				Type *arg_type = NULL;
				char *arg_name = NULL;
				if (is_type_token(p->cur.type)) {
					// C-style: `f64 x` -- type first.
					arg_type = parse_type(p);
					arg_name = p->cur.text;
					consume(p, TOK_IDENTIFIER, "Arg name");
				} else {
					// Kawa-style: `x: f64`.
					arg_name = p->cur.text;
					consume(p, TOK_IDENTIFIER, "Arg name");
					consume(p, TOK_COLON, ":");
					arg_type = parse_type(p);
				}

				ASTNode *arg = arena_alloc(p->arena, sizeof(ASTNode));
				arg->type = NODE_VAR_DECL;
				arg->data.var_decl.name = arg_name;
				arg->data_type = arg_type;
				*args_tail = arg;
				args_tail = &arg->next;
				if (p->cur.type == TOK_COMMA)
					advance(p);
				else
					break;
			}
			consume(p, TOK_RPAREN, "Expected ')'");
			consume(p, TOK_SEMICOLON,
					"Expected ';' after extern declaration (no body)");

			ASTNode *node = arena_alloc(p->arena, sizeof(ASTNode));
			node->type = NODE_EXTERN_FN;
			node->data.extern_fn.name = fn_name;
			node->data.extern_fn.ret_type = ret_type;
			node->data.extern_fn.args = args_head;
			node->data.extern_fn.is_variadic = is_variadic;
			(void)cc; // v1: only the C ABI exists; kept for future ABIs
			*tail = node;
			tail = &node->next;

			// Register the signature for let-inference too.
			if (p->fn_sig_count < 128 && ret_type) {
				int np2 = 0;
				for (ASTNode *a2 = args_head; a2; a2 = a2->next)
					np2++;
				p->fn_sigs[p->fn_sig_count].name = fn_name;
				p->fn_sigs[p->fn_sig_count].ret = ret_type;
				p->fn_sigs[p->fn_sig_count].nparams = np2;
				p->fn_sig_count++;
			}
		} else if (p->cur.type == TOK_STRUCT) {
parse_soa_struct:
			int soa_attr = 0;
			while (p->cur.type == TOK_ATTRIBUTE) {
				if (strcmp(p->cur.text, "soa") == 0)
					soa_attr = 1;
				advance(p);
			}
			consume(p, TOK_STRUCT, "Expected 'struct'");
			ASTNode *st = arena_alloc(p->arena, sizeof(ASTNode));
			st->type = NODE_STRUCT_DECL;
			st->data.struct_decl.name = p->cur.text;
			consume(p, TOK_IDENTIFIER, "Struct name");
			if (p->struct_name_count < 64) {
				p->struct_names[p->struct_name_count] =
					st->data.struct_decl.name;
				p->struct_nodes[p->struct_name_count] = st;
				p->struct_name_count++;
			}
			consume(p, TOK_LBRACE, "{");

			ASTNode *fields_head = NULL;
			ASTNode **fields_tail = &fields_head;

			while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
				Type *f_type = parse_type(p);
				// Embedded struct (IDEAS 3): `struct Employee { Person; u32
				// badge; }`. A type followed directly by ';' embeds it --
				// Go-style composition. The field is stored under the
				// struct's own name so field promotion can find it.
				if (f_type->kind == TYPE_STRUCT && f_type->name &&
					p->cur.type == TOK_SEMICOLON) {
					consume(p, TOK_SEMICOLON, ";");
					ASTNode *emb = arena_alloc(p->arena, sizeof(ASTNode));
					emb->type = NODE_VAR_DECL;
					emb->data.var_decl.name = f_type->name;
					emb->data_type = f_type;
					*fields_tail = emb;
					fields_tail = &emb->next;
					continue;
				}
				char *f_name = p->cur.text;
				consume(p, TOK_IDENTIFIER, "Field name");
				consume(p, TOK_SEMICOLON, ";");

				ASTNode *field = arena_alloc(p->arena, sizeof(ASTNode));
				field->type = NODE_VAR_DECL;
				field->data.var_decl.name = f_name;
				field->data_type = f_type;
				*fields_tail = field;
				fields_tail = &field->next;
			}
			consume(p, TOK_RBRACE, "}");
			st->data.struct_decl.fields = fields_head;
			st->data.struct_decl.is_soa = soa_attr;
			if (soa_attr && p->soa_count < 64)
				p->soa_structs[p->soa_count++] = st->data.struct_decl.name;

			*tail = st;
			tail = &st->next;
		} else if (p->cur.type == TOK_ENUM) {
			parse_enum(p, &tail);
		} else if (p->cur.type == TOK_IMPORT) {
			advance(p);
			if (p->cur.type == TOK_STRING_LIT) {
				// `import "lib/file.kawa";` -- a real multi-file import.
				// The driver expands these before compilation (see
				// expand_imports); the parser just validates the shape.
				char *path = p->cur.text;
				consume(p, TOK_STRING_LIT, "Expected quoted path");
				consume(p, TOK_SEMICOLON, ";");
				ASTNode *imp = arena_alloc(p->arena, sizeof(ASTNode));
				imp->type = NODE_IMPORT;
				imp->data.import.lib_name = path;
				*tail = imp;
				tail = &imp->next;
			} else {
				// `import stdc;` -- module-namespace marker only; stdc.*
				// calls resolve to C symbols in codegen.
				char *name = p->cur.text;
				consume(p, TOK_IDENTIFIER, "Lib name or quoted path");
				consume(p, TOK_SEMICOLON, ";");
				ASTNode *imp = arena_alloc(p->arena, sizeof(ASTNode));
				imp->type = NODE_IMPORT;
				imp->data.import.lib_name = name;
				*tail = imp;
				tail = &imp->next;
			}
		} else if (p->cur.type == TOK_ALIAS) {
			ASTNode *s = parse_statement(p);
			*tail = s;
			tail = &s->next;
		} else if (p->cur.type == TOK_IMPL) {
			advance(p);
			ASTNode *st = arena_alloc(p->arena, sizeof(ASTNode));
			st->type = NODE_IMPL_BLOCK;
			st->data.impl.struct_name = p->cur.text;
			consume(p, TOK_IDENTIFIER, "Impl struct name");
			consume(p, TOK_LBRACE, "{");

			ASTNode *methods_head = NULL;
			ASTNode **methods_tail = &methods_head;

			while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
				parse_function(p, &methods_tail, st->data.impl.struct_name);
			}
			consume(p, TOK_RBRACE, "}");
			st->data.impl.methods = methods_head;

			*tail = st;
			tail = &st->next;
		} else {
			int is_global_decl = 0;
			if (p->cur.type == TOK_CONST || p->cur.type == TOK_ORBIT ||
				p->cur.type == TOK_LET) {
				// let/const/orbit are always declarations at file scope.
				is_global_decl = 1;
			} else if (p->cur.type == TOK_LBRACKET) {
				// Mirror parse_statement's lookahead: [ N ] T name
				Lexer temp = *p->lexer;
				Token t1 = lexer_next(&temp);
				Token t2 = lexer_next(&temp);
				Token t3 = lexer_next(&temp);
				int len_ok = (t1.type == TOK_INT_LIT) ||
							 (t1.type == TOK_IDENTIFIER &&
							  find_decl_is_const(p, t1.text));
				if (len_ok && t2.type == TOK_RBRACKET &&
					is_type_token(t3.type))
					is_global_decl = 1;
			} else if (is_type_token(p->cur.type)) {
				Token next = lexer_peek(p->lexer);
				if (next.type == TOK_IDENTIFIER || next.type == TOK_STAR ||
					next.type == TOK_LBRACKET)
					is_global_decl = 1;
				if (p->cur.type == TOK_IDENTIFIER &&
					strcmp(p->cur.text, "chan") == 0 &&
					next.type == TOK_LANGLE)
					is_global_decl = 1;
			}

			if (is_global_decl) {
				ASTNode *s = parse_statement(p);
				*tail = s;
				tail = &s->next;
			} else {
				report_error(p, "Unexpected top-level token: %s", p->cur.text);
				advance(p);
			}
		}
	}
	return prog;
}
