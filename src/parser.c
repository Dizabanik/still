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
	memset(p, 0, sizeof(Parser));
	p->lexer = l;
	p->arena = a;
	p->cur = lexer_next(l);
	p->had_error = 0;
	p->panic_mode = 0;
	p->decl_count = 0;
	p->fn_sig_count = 0;
	p->soa_count = 0;
	p->struct_name_count = 0;
	p->cur_module = p->cur.filename ? p->cur.filename : l->filename;
	p->generic_struct_count = 0;
	p->generic_impl_count = 0;
	p->prog_tail = NULL;
}

static void advance(Parser *p) {
	p->prev = p->cur;
	p->cur = lexer_next(p->lexer);
	if (p->cur.filename)
		p->cur_module = p->cur.filename;
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
static int is_ident_like(TokenType t) {
	return t == TOK_IDENTIFIER || t == TOK_SET || t == TOK_DROP ||
		   t == TOK_PRESS || t == TOK_FILTER || t == TOK_BREW ||
		   t == TOK_SIP || t == TOK_ALIAS;
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
static ASTNode *parse_match(Parser *p);
static const char *type_to_suffix(Arena *arena, Type *t);
static Type *deduce_node_type(Parser *p, ASTNode *n);
static Type *get_or_create_tuple_type(Parser *p, Type **types, int count);
static Type *clone_and_subst_type(Arena *arena, Type *src, int param_count,
								   char **params, Type **concretes,
								   const char *gen_struct,
								   const char *inst_struct);
static ASTNode *clone_and_subst_node(Parser *p, ASTNode *src, int param_count,
									 char **params, Type **concretes,
									 const char *gen_struct,
									 const char *inst_struct);
static char *instantiate_struct_if_needed(Parser *p, const char *gen_name,
										  Type **concretes, int concrete_count);
__attribute__((unused)) static char *instantiate_struct_if_needed_single(Parser *p, const char *gen_name,
												 Type *concrete);
static ASTNode *parse_destructuring_let(Parser *p);
static void parse_const_decl(Parser *p, ASTNode ***tail, const char *prefix,
							 int is_pub);
static void parse_function(Parser *p, ASTNode ***tail, char *prefix,
						   int is_pub);

// Array length in `[N]T`: a literal or a const identifier. Const lookup
// goes through p->decls (registered at declaration time), so a const
// must be declared before the array that uses it -- like C.
// Supports both `N` and `Mat4.DIM`.
static long parse_array_len(Parser *p) {
	if (p->cur.type == TOK_INT_LIT) {
		long len = atol(p->cur.text);
		advance(p);
		return len;
	}
	if (p->cur.type == TOK_IDENTIFIER) {
		char *id1 = p->cur.text;
		advance(p);
		char name_buf[256];
		if (p->cur.type == TOK_DOT) {
			advance(p);
			char *id2 = p->cur.text;
			consume(p, TOK_IDENTIFIER, "Expected identifier after '.' in array length");
			snprintf(name_buf, sizeof(name_buf), "%s__%s", id1, id2);
		} else {
			snprintf(name_buf, sizeof(name_buf), "%s", id1);
		}
		ASTNode *decl = find_decl(p, name_buf);
		if (decl && decl->type == NODE_VAR_DECL &&
			decl->data.var_decl.is_const && decl->data.var_decl.init &&
			decl->data.var_decl.init->type == NODE_LITERAL) {
			long len = (long)decl->data.var_decl.init->data.literal.i64_val;
			return len;
		}
		report_error(p, "Array length must be a literal or const");
		return 0;
	}
	report_error(p, "Expected array length");
	return 0;
}

static int is_enum_name(Parser *p, const char *name) {
	if (!name) return 0;
	for (int i = 0; i < p->enum_count; i++) {
		if (p->enums[i].name && strcmp(p->enums[i].name, name) == 0)
			return 1;
	}
	return 0;
}

static EnumVariant *find_enum_variant_in_parser(Parser *p, const char *enum_name, const char *variant_name) {
	if (!variant_name) return NULL;
	for (int i = 0; i < p->enum_count; i++) {
		if (enum_name && p->enums[i].name && strcmp(p->enums[i].name, enum_name) != 0)
			continue;
		for (EnumVariant *ev = p->enums[i].variants; ev; ev = ev->next) {
			if (ev->name && strcmp(ev->name, variant_name) == 0)
				return ev;
		}
	}
	return NULL;
}

static const char *find_enum_name_by_variant(Parser *p, const char *variant_name) {
	if (!variant_name) return NULL;
	for (int i = 0; i < p->enum_count; i++) {
		for (EnumVariant *ev = p->enums[i].variants; ev; ev = ev->next) {
			if (ev->name && strcmp(ev->name, variant_name) == 0)
				return p->enums[i].name;
		}
	}
	return NULL;
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
	} else if (tok == TOK_LPAREN) {
		advance(p); // eat '('
		if (p->cur.type == TOK_RPAREN) {
			advance(p);
			t->kind = TYPE_VOID;
			goto handle_type_postfix;
		}
		Type *tuple_elems[16];
		int elem_count = 0;
		while (!p->had_error && p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF) {
			if (elem_count < 16) {
				tuple_elems[elem_count++] = parse_type(p);
			}
			if (p->cur.type == TOK_COMMA) {
				advance(p);
			} else {
				break;
			}
		}
		consume(p, TOK_RPAREN, "Expected ')' after tuple type");
		Type *tup = get_or_create_tuple_type(p, tuple_elems, elem_count);
		*t = *tup;
		goto handle_type_postfix;
	} else if (tok == TOK_IDENTIFIER) {
		char *sname = p->cur.text;
		int is_gen = 0;
		for (int gi = 0; gi < p->generic_struct_count; gi++) {
			if (strcmp(p->generic_structs[gi].name, sname) == 0) {
				is_gen = 1;
				break;
			}
		}
		if (is_gen && lexer_peek(p->lexer).type == TOK_LPAREN) {
			advance(p); // 'Pair'
			consume(p, TOK_LPAREN, "Expected '(' after generic struct name");
			Type *concretes[8];
			int concrete_count = 0;
			while (!p->had_error && p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF) {
				if (concrete_count < 8) {
					concretes[concrete_count++] = parse_type(p);
				}
				if (p->cur.type == TOK_COMMA) {
					advance(p);
				} else {
					break;
				}
			}
			consume(p, TOK_RPAREN, "Expected ')' after generic type argument(s)");
			char *inst_name = instantiate_struct_if_needed(p, sname, concretes, concrete_count);
			t->kind = TYPE_STRUCT;
			t->name = inst_name;
			goto handle_type_postfix;
		} else if (is_enum_name(p, sname)) {
			t->kind = TYPE_ENUM;
			t->name = sname;
		} else {
			t->kind = TYPE_STRUCT;
			t->name = sname;
		}
	} else {
		report_error(p, "Expected type");
		return t;
	}
	advance(p);
handle_type_postfix:
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

static int fn_sig_known(Parser *p, const char *name);

// --- Operator overloading (IDEAS 1.1) --------------------------------
// Whitelisted operators map to fixed impl method names: `+` -> self_add,
// `-` (binary) -> self_sub, unary `-` -> self_neg, `*` -> self_mul,
// `/` -> self_div, `==`/`!=`/`<`/`<=`/`>`/`>=` -> self_eq/self_ne/
// self_lt/self_le/self_gt/self_ge. No free-form symbols; numeric
// primitives never participate.

static const char *op_method_name(int op) {
	switch (op) {
	case TOK_PLUS: return "self_add";
	case TOK_MINUS: return "self_sub";
	case TOK_STAR: return "self_mul";
	case TOK_SLASH: return "self_div";
	case TOK_PERCENT: return "self_mod";
	case TOK_AMP: return "self_bitand";
	case TOK_PIPE: return "self_bitor";
	case TOK_CARET: return "self_bitxor";
	case TOK_SHL: return "self_shl";
	case TOK_SHR: return "self_shr";
	case TOK_ISEQ: return "self_eq";
	case TOK_NOTEQ: return "self_ne";
	case TOK_LANGLE: return "self_lt";
	case TOK_LEQ: return "self_le";
	case TOK_RANGLE: return "self_gt";
	case TOK_REQ: return "self_ge";
	default: return NULL;
	}
}

// The struct type of an expression operand, or NULL. Bare var refs and
// member accesses resolve through the decl table; single-letter names are
// generic parameters ("T"), never operator-overload receivers.
static Type *operand_struct_type(Parser *p, ASTNode *e) {
	if (!e)
		return NULL;
	Type *t = e->data_type;
	if (!t && e->type == NODE_VAR_REF) {
		ASTNode *decl = find_decl(p, e->data.var_ref.name);
		if (decl && decl->data_type)
			t = decl->data_type;
	}
	if (!t) {
		t = deduce_node_type(p, e);
	}
	if (!t || t->kind != TYPE_STRUCT || !t->name || strlen(t->name) <= 1)
		return NULL;
	return t;
}

static ASTNode *try_unary_op_overload(Parser *p, int op, ASTNode *operand) {
	const char *method = NULL;
	if (op == TOK_MINUS) method = "self_neg";
	else if (op == TOK_TILDE) method = "self_bitnot";
	else if (op == TOK_BANG) method = "self_not";
	if (!method) return NULL;
	Type *ot = operand_struct_type(p, operand);
	if (!ot) return NULL;
	char mangled[256];
	snprintf(mangled, sizeof(mangled), "%s__%s", ot->name, method);
	if (!fn_sig_known(p, mangled)) return NULL;
	ASTNode *call = arena_alloc(p->arena, sizeof(ASTNode));
	call->type = NODE_CALL;
	call->line = operand->line;
	ASTNode *callee = arena_alloc(p->arena, sizeof(ASTNode));
	callee->type = NODE_VAR_REF;
	callee->data.var_ref.name = arena_strdup(p->arena, mangled);
	call->data.call.callee = callee;
	operand->next = NULL;
	call->data.call.args = operand;
	for (int k = p->fn_sig_count - 1; k >= 0; k--) {
		if (strcmp(p->fn_sigs[k].name, mangled) == 0) {
			call->data_type = p->fn_sigs[k].ret;
			break;
		}
	}
	if (!call->data_type) {
		call->data_type = (op == TOK_BANG) ? bool_result_type(p) : ot;
	}
	return call;
}

// If either operand is a struct providing the whitelisted operator method,
// rewrite `l OP r` into a direct call of that method. Returns the call node
// or NULL to keep the ordinary binop.
static ASTNode *try_op_overload(Parser *p, int op, ASTNode *lhs,
								ASTNode *rhs) {
	const char *method = op_method_name(op);
	if (!method)
		return NULL;

	// LHS receiver first (`v1 + v2` calls Vec2__self_add(v1, v2)); a bare
	// scalar on the left with an overloaded right (`2 * v`) falls back to
	// the right side's method -- the method still takes both operands.
	Type *lt = operand_struct_type(p, lhs);
	Type *rt = rhs ? operand_struct_type(p, rhs) : NULL;
	Type *recv = lt ? lt : rt;
	if (!recv)
		return NULL;
	// Both overloaded but different structs: ambiguity is the user's to
	// resolve by defining exactly one; prefer LHS when both match.
	char mangled[256];
	snprintf(mangled, sizeof(mangled), "%s__%s", recv->name, method);
	if (!fn_sig_known(p, mangled)) {
		if (op == TOK_PERCENT) {
			snprintf(mangled, sizeof(mangled), "%s__self_rem", recv->name);
		}
		if (!fn_sig_known(p, mangled)) {
			if (!lt && rt) {
				snprintf(mangled, sizeof(mangled), "%s__%s", rt->name,
						 method);
				if (!fn_sig_known(p, mangled) && op == TOK_PERCENT) {
					snprintf(mangled, sizeof(mangled), "%s__self_rem", rt->name);
				}
				if (!fn_sig_known(p, mangled))
					return NULL;
			} else {
				return NULL;
			}
		}
	}

	// Build `Struct__method(lhs, rhs)` as an ordinary call node.
	ASTNode *call = arena_alloc(p->arena, sizeof(ASTNode));
	call->type = NODE_CALL;
	call->line = lhs->line;
	ASTNode *callee = arena_alloc(p->arena, sizeof(ASTNode));
	callee->type = NODE_VAR_REF;
	callee->data.var_ref.name = arena_strdup(p->arena, mangled);
	call->data.call.callee = callee;
	lhs->next = rhs;
	rhs->next = NULL;
	call->data.call.args = lhs;
	// Result type: comparisons yield bool; others come from the sig.
	for (int k = p->fn_sig_count - 1; k >= 0; k--) {
		if (strcmp(p->fn_sigs[k].name, mangled) == 0) {
			if (p->fn_sigs[k].ret &&
				p->fn_sigs[k].ret->kind == TYPE_STRUCT &&
				p->fn_sigs[k].ret->name &&
				strlen(p->fn_sigs[k].ret->name) == 1) {
				// Generic template: leave untyped for codegen inference.
				return call;
			}
			call->data_type = p->fn_sigs[k].ret;
			break;
		}
	}
	if (!call->data_type)
		call->data_type =
			(op == TOK_ISEQ || op == TOK_NOTEQ || op == TOK_LANGLE ||
			 op == TOK_LEQ || op == TOK_RANGLE || op == TOK_REQ)
				? bool_result_type(p)
				: recv;
	return call;
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
	if (p->cur.type == TOK_DOTDOT || p->cur.type == TOK_DOTDOTEQ) {
		int is_inc = (p->cur.type == TOK_DOTDOTEQ);
		advance(p);
		ASTNode *rnode = arena_alloc(p->arena, sizeof(ASTNode));
		rnode->type = NODE_RANGE;
		rnode->data.range.left = NULL;
		rnode->data.range.right = parse_unary(p);
		rnode->data.range.is_inclusive = is_inc;
		rnode->data_type = rnode->data.range.right ? rnode->data.range.right->data_type : NULL;
		return rnode;
	}
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

		Type *inner = n->data.deref.expr->data_type;
		if (inner) {
			Type *ptr = arena_alloc(p->arena, sizeof(Type));
			ptr->kind = TYPE_PTR;
			ptr->inner = inner;
			n->data_type = ptr;
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

		ASTNode *uov = try_unary_op_overload(p, op, operand);
		if (uov)
			return uov;

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
__attribute__((unused)) static ASTNode *find_struct_field(Parser *p, const char *sname,
								  const char *field);

// Declared type of field `field` on struct `sname` (parse-time view of the
// struct registry; NULL when either is unknown). Powers member-chain
// receiver typing for method calls (`d.Base.who()`).
static Type *find_field_type(Parser *p, Type *struct_t, const char *field) {
	if (!struct_t || struct_t->kind != TYPE_STRUCT || !struct_t->name || !field)
		return NULL;
	char alt_field[32] = {0};
	if (field[0] >= '0' && field[0] <= '9') {
		snprintf(alt_field, sizeof(alt_field), "_%s", field);
	}
	for (int si = 0; si < p->struct_name_count; si++) {
		if (strcmp(p->struct_names[si], struct_t->name) != 0)
			continue;
		for (ASTNode *f = p->struct_nodes[si]->data.struct_decl.fields; f;
			 f = f->next) {
			if (strcmp(f->data.var_decl.name, field) == 0 ||
				(alt_field[0] && strcmp(f->data.var_decl.name, alt_field) == 0))
				return f->data_type;
		}
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
	for (int gi = 0; gi < p->generic_struct_count; gi++) {
		if (strcmp(p->generic_structs[gi].name, name) == 0) {
			Type *t = arena_alloc(p->arena, sizeof(Type));
			t->kind = TYPE_STRUCT;
			t->name = p->generic_structs[gi].name;
			return t;
		}
	}
	return NULL;
}

static const char *type_to_suffix(Arena *arena, Type *t) {
	if (!t)
		return "unknown";
	switch (t->kind) {
	case TYPE_VOID: return "void";
	case TYPE_BOOL: return "bool";
	case TYPE_CHAR: return "char";
	case TYPE_I8:   return "i8";
	case TYPE_U8:   return "u8";
	case TYPE_I16:  return "i16";
	case TYPE_U16:  return "u16";
	case TYPE_I32:  return "i32";
	case TYPE_U32:  return "u32";
	case TYPE_I64:  return "i64";
	case TYPE_U64:  return "u64";
	case TYPE_F16:  return "f16";
	case TYPE_BF16: return "bf16";
	case TYPE_F32:  return "f32";
	case TYPE_F64:  return "f64";
	case TYPE_STRUCT:
		return t->name ? t->name : "struct";
	case TYPE_PTR: {
		const char *in = type_to_suffix(arena, t->inner);
		size_t len = strlen(in) + 5;
		char *buf = arena_alloc(arena, len);
		snprintf(buf, len, "%s_ptr", in);
		return buf;
	}
	case TYPE_AMP: {
		const char *in = type_to_suffix(arena, t->inner);
		size_t len = strlen(in) + 5;
		char *buf = arena_alloc(arena, len);
		snprintf(buf, len, "%s_ref", in);
		return buf;
	}
	case TYPE_SLICE: {
		const char *in = type_to_suffix(arena, t->inner);
		size_t len = strlen(in) + 7;
		char *buf = arena_alloc(arena, len);
		snprintf(buf, len, "%s_slice", in);
		return buf;
	}
	case TYPE_ARRAY: {
		const char *in = type_to_suffix(arena, t->inner);
		size_t len = strlen(in) + 24;
		char *buf = arena_alloc(arena, len);
		snprintf(buf, len, "%s_arr%ld", in, t->array_len);
		return buf;
	}
	default:
		return "val";
	}
}

static Type *get_or_create_tuple_type(Parser *p, Type **types, int count) {
	if (count == 0) {
		Type *t = arena_alloc(p->arena, sizeof(Type));
		t->kind = TYPE_VOID;
		return t;
	}
	char buf[512];
	int off = snprintf(buf, sizeof(buf), "tuple_");
	for (int i = 0; i < count; i++) {
		const char *sfx = types[i] ? type_to_suffix(p->arena, types[i]) : "val";
		off += snprintf(buf + off, sizeof(buf) - off, "_%s", sfx);
	}
	char *name = arena_strdup(p->arena, buf);
	for (int i = 0; i < p->struct_name_count; i++) {
		if (strcmp(p->struct_names[i], name) == 0) {
			Type *t = arena_alloc(p->arena, sizeof(Type));
			t->kind = TYPE_STRUCT;
			t->name = name;
			return t;
		}
	}

	ASTNode *st = arena_alloc(p->arena, sizeof(ASTNode));
	st->type = NODE_STRUCT_DECL;
	st->data.struct_decl.name = name;
	st->is_pub = 1;
	st->module_name = p->cur_module;

	ASTNode *f_head = NULL;
	ASTNode **f_tail = &f_head;
	for (int i = 0; i < count; i++) {
		ASTNode *f = arena_alloc(p->arena, sizeof(ASTNode));
		f->type = NODE_VAR_DECL;
		char fld_name[32];
		snprintf(fld_name, sizeof(fld_name), "_%d", i);
		f->data.var_decl.name = arena_strdup(p->arena, fld_name);
		f->data_type = types[i];
		f->is_pub = 1;
		f->module_name = p->cur_module;
		*f_tail = f;
		f_tail = &f->next;
	}
	st->data.struct_decl.fields = f_head;

	if (p->struct_name_count < 128) {
		p->struct_names[p->struct_name_count] = name;
		p->struct_nodes[p->struct_name_count] = st;
		p->struct_name_count++;
	}

	if (p->prog_tail && *p->prog_tail) {
		**p->prog_tail = st;
		*p->prog_tail = &st->next;
	}

	Type *t = arena_alloc(p->arena, sizeof(Type));
	t->kind = TYPE_STRUCT;
	t->name = name;
	return t;
}

static Type *clone_and_subst_type(Arena *arena, Type *src, int param_count,
								   char **params, Type **concretes,
								   const char *gen_struct,
								   const char *inst_struct) {
	if (!src)
		return NULL;
	if (src->kind == TYPE_STRUCT && src->name) {
		for (int i = 0; i < param_count; i++) {
			if (params && params[i] && concretes && concretes[i] && strcmp(src->name, params[i]) == 0)
				return concretes[i];
		}
		if (gen_struct && inst_struct && strcmp(src->name, gen_struct) == 0) {
			Type *res = arena_alloc(arena, sizeof(Type));
			res->kind = TYPE_STRUCT;
			res->name = arena_strdup(arena, inst_struct);
			return res;
		}
	}
	Type *dst = arena_alloc(arena, sizeof(Type));
	dst->kind = src->kind;
	dst->array_len = src->array_len;
	dst->name = src->name ? arena_strdup(arena, src->name) : NULL;
	dst->inner = clone_and_subst_type(arena, src->inner, param_count, params,
									  concretes, gen_struct, inst_struct);
	return dst;
}

static ASTNode *clone_and_subst_node(Parser *p, ASTNode *src, int param_count,
									 char **params, Type **concretes,
									 const char *gen_struct,
									 const char *inst_struct) {
	if (!src)
		return NULL;

	ASTNode *dst = arena_alloc(p->arena, sizeof(ASTNode));
	dst->type = src->type;
	dst->line = src->line;
	dst->is_pub = src->is_pub;
	dst->module_name = src->module_name;
	dst->data_type = clone_and_subst_type(p->arena, src->data_type, param_count,
										  params, concretes, gen_struct, inst_struct);

	switch (src->type) {
	case NODE_FUNC_DECL: {
		dst->data.func.is_pure = src->data.func.is_pure;
		dst->data.func.is_drip = src->data.func.is_drip;
		dst->data.func.is_test = src->data.func.is_test;
		dst->data.func.is_ignored = src->data.func.is_ignored;

		const char *old_name = src->data.func.name;
		if (old_name && gen_struct && inst_struct) {
			size_t glen = strlen(gen_struct);
			if (strncmp(old_name, gen_struct, glen) == 0 &&
				old_name[glen] == '_' && old_name[glen + 1] == '_') {
				const char *subname = old_name + glen + 2;
				size_t nlen = strlen(inst_struct) + strlen(subname) + 4;
				char *mangled = arena_alloc(p->arena, nlen);
				snprintf(mangled, nlen, "%s__%s", inst_struct, subname);
				dst->data.func.name = mangled;
			} else {
				dst->data.func.name = arena_strdup(p->arena, old_name);
			}
		} else {
			dst->data.func.name = old_name ? arena_strdup(p->arena, old_name) : NULL;
		}

		dst->data.func.ret_type = clone_and_subst_type(
			p->arena, src->data.func.ret_type, param_count, params, concretes, gen_struct,
			inst_struct);

		ASTNode *args_head = NULL;
		ASTNode **args_tail = &args_head;
		for (ASTNode *a = src->data.func.args; a; a = a->next) {
			ASTNode *ca = clone_and_subst_node(p, a, param_count, params, concretes,
											   gen_struct, inst_struct);
			*args_tail = ca;
			args_tail = &ca->next;
		}
		dst->data.func.args = args_head;
		dst->data.func.body = clone_and_subst_node(p, src->data.func.body,
												   param_count, params, concretes, gen_struct,
												   inst_struct);
		break;
	}
	case NODE_VAR_DECL: {
		dst->data.var_decl.name = src->data.var_decl.name
									  ? arena_strdup(p->arena, src->data.var_decl.name)
									  : NULL;
		dst->data.var_decl.is_orbit = src->data.var_decl.is_orbit;
		dst->data.var_decl.is_const = src->data.var_decl.is_const;
		dst->data.var_decl.init = clone_and_subst_node(
			p, src->data.var_decl.init, param_count, params, concretes, gen_struct,
			inst_struct);
		dst->data.var_decl.field_default = clone_and_subst_node(
			p, src->data.var_decl.field_default, param_count, params, concretes, gen_struct,
			inst_struct);
		break;
	}
	case NODE_BLOCK: {
		ASTNode *stmts_head = NULL;
		ASTNode **stmts_tail = &stmts_head;
		for (ASTNode *s = src->data.block.stmts; s; s = s->next) {
			ASTNode *cs = clone_and_subst_node(p, s, param_count, params, concretes,
											   gen_struct, inst_struct);
			*stmts_tail = cs;
			stmts_tail = &cs->next;
		}
		dst->data.block.stmts = stmts_head;
		break;
	}
	case NODE_RETURN:
		dst->data.ret_stmt.expr = clone_and_subst_node(
			p, src->data.ret_stmt.expr, param_count, params, concretes, gen_struct,
			inst_struct);
		break;
	case NODE_ASSIGN:
		dst->data.assign.target = clone_and_subst_node(
			p, src->data.assign.target, param_count, params, concretes, gen_struct,
			inst_struct);
		dst->data.assign.value = clone_and_subst_node(
			p, src->data.assign.value, param_count, params, concretes, gen_struct,
			inst_struct);
		break;
	case NODE_BINARY_OP:
		dst->data.bin_op.op = src->data.bin_op.op;
		dst->data.bin_op.left = clone_and_subst_node(
			p, src->data.bin_op.left, param_count, params, concretes, gen_struct,
			inst_struct);
		dst->data.bin_op.right = clone_and_subst_node(
			p, src->data.bin_op.right, param_count, params, concretes, gen_struct,
			inst_struct);
		break;
	case NODE_CALL: {
		dst->data.call.callee = clone_and_subst_node(
			p, src->data.call.callee, param_count, params, concretes, gen_struct,
			inst_struct);
		ASTNode *args_head = NULL;
		ASTNode **args_tail = &args_head;
		for (ASTNode *a = src->data.call.args; a; a = a->next) {
			ASTNode *ca = clone_and_subst_node(p, a, param_count, params, concretes,
											   gen_struct, inst_struct);
			ca->has_arg_label = a->has_arg_label;
			ca->arg_label = a->arg_label ? arena_strdup(p->arena, a->arg_label) : NULL;
			*args_tail = ca;
			args_tail = &ca->next;
		}
		dst->data.call.args = args_head;
		break;
	}
	case NODE_MEMBER_ACCESS:
		dst->data.member_access.object = clone_and_subst_node(
			p, src->data.member_access.object, param_count, params, concretes, gen_struct,
			inst_struct);
		dst->data.member_access.member = src->data.member_access.member
											 ? arena_strdup(p->arena, src->data.member_access.member)
											 : NULL;
		break;
	case NODE_INDEX:
		dst->data.index.object = clone_and_subst_node(
			p, src->data.index.object, param_count, params, concretes, gen_struct,
			inst_struct);
		dst->data.index.index = clone_and_subst_node(
			p, src->data.index.index, param_count, params, concretes, gen_struct,
			inst_struct);
		break;
	case NODE_VAR_REF: {
		const char *vname = src->data.var_ref.name;
		if (vname && gen_struct && inst_struct) {
			if (strcmp(vname, gen_struct) == 0) {
				dst->data.var_ref.name = arena_strdup(p->arena, inst_struct);
			} else {
				size_t glen = strlen(gen_struct);
				if (strncmp(vname, gen_struct, glen) == 0 &&
					vname[glen] == '_' && vname[glen + 1] == '_') {
					const char *subname = vname + glen + 2;
					size_t nlen = strlen(inst_struct) + strlen(subname) + 4;
					char *mangled = arena_alloc(p->arena, nlen);
					snprintf(mangled, nlen, "%s__%s", inst_struct, subname);
					dst->data.var_ref.name = mangled;
				} else {
					dst->data.var_ref.name = arena_strdup(p->arena, vname);
				}
			}
		} else {
			dst->data.var_ref.name = vname ? arena_strdup(p->arena, vname) : NULL;
		}
		break;
	}
	case NODE_STRUCT_LITERAL: {
		StructInitItem *items_head = NULL;
		StructInitItem **items_tail = &items_head;
		for (StructInitItem *it = src->data.struct_lit.items; it; it = it->next) {
			StructInitItem *cit = arena_alloc(p->arena, sizeof(StructInitItem));
			cit->field_name = it->field_name ? arena_strdup(p->arena, it->field_name) : NULL;
			cit->value = clone_and_subst_node(p, it->value, param_count, params, concretes,
											  gen_struct, inst_struct);
			cit->spread_from = clone_and_subst_node(p, it->spread_from, param_count,
													params, concretes, gen_struct,
													inst_struct);
			cit->next = NULL;
			*items_tail = cit;
			items_tail = &cit->next;
		}
		dst->data.struct_lit.items = items_head;
		break;
	}
	case NODE_IF:
		dst->data.if_stmt.cond = clone_and_subst_node(
			p, src->data.if_stmt.cond, param_count, params, concretes, gen_struct,
			inst_struct);
		dst->data.if_stmt.then_block = clone_and_subst_node(
			p, src->data.if_stmt.then_block, param_count, params, concretes, gen_struct,
			inst_struct);
		dst->data.if_stmt.else_block = clone_and_subst_node(
			p, src->data.if_stmt.else_block, param_count, params, concretes, gen_struct,
			inst_struct);
		break;
	case NODE_WHILE:
		dst->data.while_stmt.cond = clone_and_subst_node(
			p, src->data.while_stmt.cond, param_count, params, concretes, gen_struct,
			inst_struct);
		dst->data.while_stmt.body = clone_and_subst_node(
			p, src->data.while_stmt.body, param_count, params, concretes, gen_struct,
			inst_struct);
		break;
	case NODE_FOR:
		dst->data.for_stmt.init = clone_and_subst_node(
			p, src->data.for_stmt.init, param_count, params, concretes, gen_struct,
			inst_struct);
		dst->data.for_stmt.cond = clone_and_subst_node(
			p, src->data.for_stmt.cond, param_count, params, concretes, gen_struct,
			inst_struct);
		dst->data.for_stmt.step = clone_and_subst_node(
			p, src->data.for_stmt.step, param_count, params, concretes, gen_struct,
			inst_struct);
		dst->data.for_stmt.body = clone_and_subst_node(
			p, src->data.for_stmt.body, param_count, params, concretes, gen_struct,
			inst_struct);
		break;
	case NODE_LITERAL:
		dst->data.literal = src->data.literal;
		break;
	case NODE_STRING_LIT:
		dst->data.str_lit = src->data.str_lit; // immutable arena bytes
		break;
	case NODE_CAST:
		dst->data.cast.val = clone_and_subst_node(
			p, src->data.cast.val, param_count, params, concretes, gen_struct,
			inst_struct);
		break;
	case NODE_DEREF:
	case NODE_AMP:
		dst->data.deref.expr = clone_and_subst_node(
			p, src->data.deref.expr, param_count, params, concretes, gen_struct,
			inst_struct);
		break;
	case NODE_SIZEOF:
		dst->data.size_of.type_val = clone_and_subst_type(
			p->arena, src->data.size_of.type_val, param_count, params, concretes, gen_struct,
			inst_struct);
		dst->data.size_of.value = clone_and_subst_node(
			p, src->data.size_of.value, param_count, params, concretes, gen_struct,
			inst_struct);
		break;
	default:
		break;
	}
	return dst;
}

static char *instantiate_struct_if_needed(Parser *p, const char *gen_name,
										  Type **concretes, int concrete_count) {
	if (!gen_name || !concretes || concrete_count <= 0)
		return (char *)gen_name;

	int g_idx = -1;
	for (int i = 0; i < p->generic_struct_count; i++) {
		if (strcmp(p->generic_structs[i].name, gen_name) == 0) {
			g_idx = i;
			break;
		}
	}
	if (g_idx < 0)
		return (char *)gen_name;

	int nparams = p->generic_structs[g_idx].type_param_count;
	if (nparams <= 0) nparams = 1;
	char **params = p->generic_structs[g_idx].type_params;

	for (int ci = 0; ci < concrete_count; ci++) {
		if (concretes[ci] && concretes[ci]->kind == TYPE_STRUCT && concretes[ci]->name) {
			for (int pi = 0; pi < nparams; pi++) {
				if (params[pi] && strcmp(concretes[ci]->name, params[pi]) == 0)
					return (char *)gen_name;
			}
		}
	}

	char buf[512];
	int offset = snprintf(buf, sizeof(buf), "%s", gen_name);
	for (int ci = 0; ci < concrete_count; ci++) {
		const char *sfx = concretes[ci] ? type_to_suffix(p->arena, concretes[ci]) : "val";
		offset += snprintf(buf + offset, sizeof(buf) - offset, "%s%s", (ci == 0 ? "__" : "_"), sfx);
	}
	char *inst_name = arena_strdup(p->arena, buf);

	for (int i = 0; i < p->struct_name_count; i++) {
		if (strcmp(p->struct_names[i], inst_name) == 0)
			return inst_name;
	}

	if (p->generic_structs[g_idx].inst_count < 32) {
		int ic = p->generic_structs[g_idx].inst_count;
		p->generic_structs[g_idx].instantiations[ic] = concretes[0];
		for (int ci = 0; ci < concrete_count && ci < 8; ci++) {
			p->generic_structs[g_idx].instantiations_multi[ic][ci] = concretes[ci];
		}
		p->generic_structs[g_idx].inst_count++;
	}

	ASTNode *tmpl = p->generic_structs[g_idx].node;
	ASTNode *inst_fields = NULL;
	ASTNode **f_tail = &inst_fields;
	for (ASTNode *f = tmpl->data.struct_decl.fields; f; f = f->next) {
		ASTNode *cf = arena_alloc(p->arena, sizeof(ASTNode));
		cf->type = NODE_VAR_DECL;
		cf->data.var_decl.name = f->data.var_decl.name;
		cf->data_type = clone_and_subst_type(p->arena, f->data_type, nparams,
											 params, concretes, gen_name, inst_name);
		cf->is_pub = f->is_pub;
		cf->module_name = f->module_name;
		*f_tail = cf;
		f_tail = &cf->next;
	}

	ASTNode *st = arena_alloc(p->arena, sizeof(ASTNode));
	st->type = NODE_STRUCT_DECL;
	st->data.struct_decl.name = inst_name;
	st->data.struct_decl.fields = inst_fields;
	st->data.struct_decl.is_soa = tmpl->data.struct_decl.is_soa;
	st->is_pub = tmpl->is_pub;
	st->module_name = tmpl->module_name;

	if (p->struct_name_count < 128) {
		p->struct_names[p->struct_name_count] = inst_name;
		p->struct_nodes[p->struct_name_count] = st;
		p->struct_name_count++;
	}

	if (p->prog_tail && *p->prog_tail) {
		**p->prog_tail = st;
		*p->prog_tail = &st->next;
	}

	for (int gi = 0; gi < p->generic_impl_count; gi++) {
		if (strcmp(p->generic_impls[gi].struct_name, gen_name) != 0)
			continue;
		ASTNode *impl_node = p->generic_impls[gi].node;
		int iparam_count = p->generic_impls[gi].type_param_count;
		if (iparam_count <= 0) iparam_count = 1;
		char **iparams = p->generic_impls[gi].type_params;

		ASTNode *inst_methods = NULL;
		ASTNode **m_tail = &inst_methods;
		for (ASTNode *m = impl_node->data.impl.methods; m; m = m->next) {
			ASTNode *cm = clone_and_subst_node(p, m, iparam_count, iparams, concretes,
											   gen_name, inst_name);
			*m_tail = cm;
			m_tail = &cm->next;

			if (cm->type == NODE_FUNC_DECL) {
				if (p->fn_sig_count < 512 && cm->data.func.ret_type) {
					int np = 0;
					for (ASTNode *a = cm->data.func.args; a; a = a->next)
						np++;
					p->fn_sigs[p->fn_sig_count].name = cm->data.func.name;
					p->fn_sigs[p->fn_sig_count].ret = cm->data.func.ret_type;
					p->fn_sigs[p->fn_sig_count].nparams = np;
					p->fn_sig_count++;
				}
				if (p->decl_count < 1024) {
					p->decls[p->decl_count].name = cm->data.func.name;
					p->decls[p->decl_count].node = cm;
					p->decl_count++;
				}
			}
		}

		ASTNode *ib = arena_alloc(p->arena, sizeof(ASTNode));
		ib->type = NODE_IMPL_BLOCK;
		ib->data.impl.struct_name = inst_name;
		ib->data.impl.methods = inst_methods;
		ib->is_pub = impl_node->is_pub;
		ib->module_name = impl_node->module_name;

		if (p->prog_tail && *p->prog_tail) {
			**p->prog_tail = ib;
			*p->prog_tail = &ib->next;
		}
	}

	return inst_name;
}

static char *instantiate_struct_if_needed_single(Parser *p, const char *gen_name,
												 Type *concrete) {
	Type *arr[1] = { concrete };
	return instantiate_struct_if_needed(p, gen_name, arr, 1);
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
			if (t.type == TOK_COLON)
				return 1; // Found colon at top level -> Struct
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

	while (!p->had_error && p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
		StructInitItem *item = arena_alloc(p->arena, sizeof(StructInitItem));
		item->field_name = NULL;
		item->spread_from = NULL;

		// `..base` spread (IDEAS 1.3): remaining fields copy from `base`.
		if (p->cur.type == TOK_DOTDOT) {
			advance(p);
			item->spread_from = parse_expr(p);
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

		// Handle Designated Init: .age = 10
		if (p->cur.type == TOK_DOT) {
			advance(p);
			item->field_name = p->cur.text;
			if (is_ident_like(p->cur.type))
				advance(p);
			else
				consume(p, TOK_IDENTIFIER, "Expected field name");
			consume(p, TOK_ASSIGN, "Expected '='");
			item->value = parse_expr(p);
		} else if (is_ident_like(p->cur.type) &&
				   lexer_peek(p->lexer).type == TOK_COLON) {
			item->field_name = p->cur.text;
			advance(p);
			consume(p, TOK_COLON, "Expected ':'");
			item->value = parse_expr(p);
		} else if (is_ident_like(p->cur.type) &&
				   lexer_peek(p->lexer).type == TOK_ASSIGN) {
			item->field_name = p->cur.text;
			advance(p);
			consume(p, TOK_ASSIGN, "Expected '='");
			item->value = parse_expr(p);
		} else if (is_ident_like(p->cur.type) &&
				   (lexer_peek(p->lexer).type == TOK_COMMA ||
					lexer_peek(p->lexer).type == TOK_RBRACE)) {
			// Field-init shorthand: `Vec { x, y }` means `.x = x, .y = y`
			// -- the identifier names the field AND supplies the value.
			item->field_name = p->cur.text;
			item->value = parse_expr(p);
		} else {
			item->value = parse_expr(p);
		}

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
__attribute__((unused)) static ASTNode *parse_recv(Parser *p) {
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
		} else if (v <= (unsigned long long)INT64_MAX) {
			n->data_type->kind = TYPE_I64;
			n->data.literal.i64_val = (long long)v;
		} else {
			// Fits only in 64 bits unsigned.
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
		n->data.str_lit.len = p->cur.string_len;
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
	} else if (p->cur.type == TOK_MATCH) {
		return parse_match(p);
	} else if (p->cur.type == TOK_IDENTIFIER) {
		char *id_name = p->cur.text;
		EnumVariant *ev = find_enum_variant_in_parser(p, NULL, id_name);
		Token next_tok = lexer_peek(p->lexer);
		if (ev && ev->payload_count == 0 && next_tok.type != TOK_LPAREN && !find_decl(p, id_name)) {
			advance(p); // eat identifier
			ASTNode *call = arena_alloc(p->arena, sizeof(ASTNode));
			call->type = NODE_CALL;
			call->line = p->prev.line;
			ASTNode *callee = arena_alloc(p->arena, sizeof(ASTNode));
			callee->type = NODE_VAR_REF;
			callee->data.var_ref.name = id_name;
			call->data.call.callee = callee;
			call->data.call.args = NULL;
			const char *ename = find_enum_name_by_variant(p, id_name);
			Type *en_t = arena_alloc(p->arena, sizeof(Type));
			en_t->kind = TYPE_ENUM;
			en_t->name = (char *)ename;
			call->data_type = en_t;
			return call;
		}
		int is_gen = 0;
		for (int gi = 0; gi < p->generic_struct_count; gi++) {
			if (strcmp(p->generic_structs[gi].name, id_name) == 0) {
				is_gen = 1;
				break;
			}
		}
		if (is_gen && lexer_peek(p->lexer).type == TOK_LPAREN) {
			Lexer temp = *p->lexer;
			lexer_next(&temp); // eat '('
			Token t = lexer_next(&temp);
			if (is_type_token(t.type) || t.type == TOK_LPAREN) {
				advance(p); // eat generic struct name
				consume(p, TOK_LPAREN, "Expected '(' after generic struct name");
				Type *concretes[8];
				int concrete_count = 0;
				while (!p->had_error && p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF) {
					if (concrete_count < 8) {
						concretes[concrete_count++] = parse_type(p);
					}
					if (p->cur.type == TOK_COMMA)
						advance(p);
					else
						break;
				}
				consume(p, TOK_RPAREN, "Expected ')' after generic type arguments");
				char *inst_name = instantiate_struct_if_needed(p, id_name, concretes, concrete_count);
				n->type = NODE_VAR_REF;
				n->data.var_ref.name = inst_name;
				n->line = p->prev.line;
				Type *st_t = arena_alloc(p->arena, sizeof(Type));
				st_t->kind = TYPE_STRUCT;
				st_t->name = inst_name;
				n->data_type = st_t;
				return n;
			}
		}
		n->type = NODE_VAR_REF;
		n->data.var_ref.name = p->cur.text;
		ASTNode *bound_decl = find_decl(p, p->cur.text);
		if (bound_decl && (bound_decl->type == NODE_VAR_DECL || bound_decl->type == NODE_CONST_DECL))
			n->data_type = bound_decl->data_type;
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
			// --- Parse as GROUPING or TUPLE ---
			advance(p); // eat '('
			if (p->cur.type == TOK_RPAREN) {
				advance(p);
				Type *u_type = get_or_create_tuple_type(p, NULL, 0);
				ASTNode *lit = arena_alloc(p->arena, sizeof(ASTNode));
				lit->type = NODE_STRUCT_LITERAL;
				lit->data_type = u_type;
				lit->data.struct_lit.items = NULL;
				return lit;
			}
			ASTNode *first_expr = parse_expr(p);
			if (p->cur.type == TOK_COMMA) {
				StructInitItem *items_head = arena_alloc(p->arena, sizeof(StructInitItem));
				items_head->field_name = arena_strdup(p->arena, "_0");
				items_head->value = first_expr;
				items_head->spread_from = NULL;
				items_head->next = NULL;
				StructInitItem **items_tail = &items_head->next;

				Type *types[16];
				int elem_count = 1;
				types[0] = deduce_node_type(p, first_expr);

				advance(p); // eat ','
				while (!p->had_error && p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF) {
					ASTNode *elem_expr = parse_expr(p);
					StructInitItem *item = arena_alloc(p->arena, sizeof(StructInitItem));
					char fld[32];
					snprintf(fld, sizeof(fld), "_%d", elem_count);
					item->field_name = arena_strdup(p->arena, fld);
					item->value = elem_expr;
					item->spread_from = NULL;
					item->next = NULL;
					*items_tail = item;
					items_tail = &item->next;

					if (elem_count < 16) {
						types[elem_count] = deduce_node_type(p, elem_expr);
						elem_count++;
					}

					if (p->cur.type == TOK_COMMA)
						advance(p);
					else
						break;
				}
				consume(p, TOK_RPAREN, "Expected ')' after tuple elements");

				int all_typed = 1;
				for (int i = 0; i < elem_count; i++) {
					if (!types[i]) { all_typed = 0; break; }
				}
				Type *tup_type = NULL;
				if (all_typed) {
					tup_type = get_or_create_tuple_type(p, types, elem_count);
				}

				ASTNode *tup_node = arena_alloc(p->arena, sizeof(ASTNode));
				tup_node->type = NODE_STRUCT_LITERAL;
				tup_node->data_type = tup_type;
				tup_node->data.struct_lit.items = items_head;
				return tup_node;
			}
			consume(p, TOK_RPAREN, "Expected ')'");
			return first_expr;
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
		while (!p->had_error && p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
			*tail = parse_statement(p);
			while (*tail)
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

		while (!p->had_error && p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
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

static Type *deduce_node_type(Parser *p, ASTNode *n) {
	if (!n) return NULL;
	if (n->data_type) return n->data_type;
	if (n->type == NODE_VAR_REF) {
		ASTNode *decl = find_decl(p, n->data.var_ref.name);
		if (decl && decl->data_type) return decl->data_type;
		if (decl && decl->type == NODE_VAR_DECL && decl->data.var_decl.init)
			return deduce_node_type(p, decl->data.var_decl.init);
	}
	if (n->type == NODE_LITERAL) {
		return n->data_type;
	}
	if (n->type == NODE_STRING_LIT) {
		Type *st = arena_alloc(p->arena, sizeof(Type));
		st->kind = TYPE_SLICE;
		Type *ch = arena_alloc(p->arena, sizeof(Type));
		ch->kind = TYPE_U8;
		st->inner = ch;
		return st;
	}
	if (n->type == NODE_BINARY_OP) {
		Type *lt = deduce_node_type(p, n->data.bin_op.left);
		Type *rt = deduce_node_type(p, n->data.bin_op.right);
		if (lt) return lt;
		if (rt) return rt;
	}
	if (n->type == NODE_CALL) {
		ASTNode *callee = n->data.call.callee;
		if (callee && callee->type == NODE_VAR_REF) {
			const char *name = callee->data.var_ref.name;
			for (int si = p->fn_sig_count - 1; si >= 0; si--) {
				if (strcmp(p->fn_sigs[si].name, name) == 0) {
					return p->fn_sigs[si].ret;
				}
			}
		}
	}
	if (n->type == NODE_MEMBER_ACCESS) {
		Type *ot = deduce_node_type(p, n->data.member_access.object);
		if (ot) {
			while (ot->kind == TYPE_PTR || ot->kind == TYPE_AMP) {
				if (!ot->inner) break;
				ot = ot->inner;
			}
			if ((ot->kind == TYPE_SLICE || ot->kind == TYPE_ARRAY) &&
				n->data.member_access.member &&
				strcmp(n->data.member_access.member, "len") == 0) {
				Type *t = arena_alloc(p->arena, sizeof(Type));
				t->kind = TYPE_I64;
				return t;
			}
			if (ot->kind == TYPE_STRUCT && n->data.member_access.member) {
				return find_field_type(p, ot, n->data.member_access.member);
			}
		}
	}
	if (n->type == NODE_CAST) {
		return n->data_type;
	}
	if (n->type == NODE_STRUCT_LITERAL) {
		return n->data_type;
	}
	return NULL;
}
static ASTNode *make_call_node(Parser *p, int line, const char *fn_name, ASTNode *args) {
	ASTNode *callee = arena_alloc(p->arena, sizeof(ASTNode));
	callee->type = NODE_VAR_REF;
	callee->line = line;
	callee->data.var_ref.name = arena_strdup(p->arena, fn_name);

	ASTNode *call = arena_alloc(p->arena, sizeof(ASTNode));
	call->type = NODE_CALL;
	call->line = line;
	call->data.call.callee = callee;
	call->data.call.args = args;
	Type *void_t = arena_alloc(p->arena, sizeof(Type));
	void_t->kind = TYPE_VOID;
	call->data_type = void_t;
	call->next = NULL;
	return call;
}

static ASTNode *make_str_lit_node(Parser *p, int line, const char *s) {
	ASTNode *n = arena_alloc(p->arena, sizeof(ASTNode));
	n->type = NODE_STRING_LIT;
	n->line = line;
	n->data.str_lit.s_val = arena_strdup(p->arena, s);
	n->data.str_lit.len = strlen(s);
	Type *ch = arena_alloc(p->arena, sizeof(Type));
	ch->kind = TYPE_U8;
	Type *st = arena_alloc(p->arena, sizeof(Type));
	st->kind = TYPE_SLICE;
	st->inner = ch;
	n->data_type = st;
	n->next = NULL;
	return n;
}

static ASTNode *make_i64_lit_node(Parser *p, int line, int64_t val) {
	ASTNode *n = arena_alloc(p->arena, sizeof(ASTNode));
	n->type = NODE_LITERAL;
	n->line = line;
	n->data.literal.i_val = (int)val;
	n->data.literal.i64_val = val;
	Type *t = arena_alloc(p->arena, sizeof(Type));
	t->kind = TYPE_I64;
	t->is_signed = 1;
	n->data_type = t;
	n->next = NULL;
	return n;
}

static ASTNode *make_int_lit_node(Parser *p, int line, int val) {
	ASTNode *n = arena_alloc(p->arena, sizeof(ASTNode));
	n->type = NODE_LITERAL;
	n->line = line;
	n->data.literal.i_val = val;
	n->data.literal.i64_val = (int64_t)val;
	Type *t = arena_alloc(p->arena, sizeof(Type));
	t->kind = TYPE_I32;
	t->is_signed = 1;
	n->data_type = t;
	n->next = NULL;
	return n;
}

static ASTNode *make_char_lit_node(Parser *p, int line, char c) {
	ASTNode *n = arena_alloc(p->arena, sizeof(ASTNode));
	n->type = NODE_LITERAL;
	n->line = line;
	n->data.literal.i_val = (int)c;
	n->data.literal.i64_val = (int64_t)c;
	Type *t = arena_alloc(p->arena, sizeof(Type));
	t->kind = TYPE_CHAR;
	n->data_type = t;
	n->next = NULL;
	return n;
}

static void append_print_stmt(ASTNode **head, ASTNode ***tail, ASTNode *node) {
	if (!node) return;
	node->next = NULL;
	**tail = node;
	*tail = &node->next;
}

static void append_print_str_lit(Parser *p, int line, ASTNode **head, ASTNode ***tail, const char *str, size_t len) {
	if (len == 0) return;
	char *s = arena_alloc(p->arena, len + 1);
	memcpy(s, str, len);
	s[len] = '\0';
	ASTNode *s_node = make_str_lit_node(p, line, "");
	s_node->data.str_lit.s_val = s;
	s_node->data.str_lit.len = len;
	ASTNode *len_node = make_i64_lit_node(p, line, (int64_t)len);
	s_node->next = len_node;
	ASTNode *call = make_call_node(p, line, "__kawa_print_str", s_node);
	append_print_stmt(head, tail, call);
}

static void append_print_expr(Parser *p, int line, ASTNode **head, ASTNode ***tail, ASTNode *arg, const char *spec) {
	if (!arg) return;
	arg->next = NULL;
	Type *t = deduce_node_type(p, arg);
	while (t && t->kind == TYPE_ALIAS && t->inner) t = t->inner;

	if (spec && *spec) {
		if (*spec == '%') spec++;
		if (strcmp(spec, "x") == 0 || strcmp(spec, "X") == 0) {
			int is_upper = (strcmp(spec, "X") == 0);
			ASTNode *up_node = make_int_lit_node(p, line, is_upper);
			arg->next = up_node;
			ASTNode *call = make_call_node(p, line, "__kawa_print_hex", arg);
			append_print_stmt(head, tail, call);
			return;
		}
		const char *dot = strchr(spec, '.');
		if (dot || spec[strlen(spec)-1] == 'f') {
			int prec = 6;
			if (dot) prec = atoi(dot + 1);
			else prec = atoi(spec);
			ASTNode *p_node = make_int_lit_node(p, line, prec);
			arg->next = p_node;
			ASTNode *call = make_call_node(p, line, "__kawa_print_f64_prec", arg);
			append_print_stmt(head, tail, call);
			return;
		}
		int slen = (int)strlen(spec);
		if (slen > 1 && (spec[slen-1] == 'd' || spec[slen-1] == 'u')) {
			char pad = ' ';
			int width = 0;
			if (spec[0] == '0') {
				pad = '0';
				width = atoi(spec + 1);
			} else {
				width = atoi(spec);
			}
			ASTNode *w_node = make_int_lit_node(p, line, width);
			ASTNode *pad_node = make_char_lit_node(p, line, pad);
			arg->next = w_node;
			w_node->next = pad_node;
			ASTNode *call = make_call_node(p, line, "__kawa_print_pad_i64", arg);
			append_print_stmt(head, tail, call);
			return;
		}
	}

	if (t) {
		switch (t->kind) {
		case TYPE_BOOL: {
			ASTNode *call = make_call_node(p, line, "__kawa_print_bool", arg);
			append_print_stmt(head, tail, call);
			return;
		}
		case TYPE_CHAR: {
			ASTNode *call = make_call_node(p, line, "__kawa_print_char", arg);
			append_print_stmt(head, tail, call);
			return;
		}
		case TYPE_U8:
		case TYPE_U16:
		case TYPE_U32:
		case TYPE_U64: {
			ASTNode *call = make_call_node(p, line, "__kawa_print_u64", arg);
			append_print_stmt(head, tail, call);
			return;
		}
		case TYPE_I8:
		case TYPE_I16:
		case TYPE_I32:
		case TYPE_I64: {
			ASTNode *call = make_call_node(p, line, "__kawa_print_i64", arg);
			append_print_stmt(head, tail, call);
			return;
		}
		case TYPE_F16:
		case TYPE_BF16:
		case TYPE_F32:
		case TYPE_F64: {
			ASTNode *call = make_call_node(p, line, "__kawa_print_f64", arg);
			append_print_stmt(head, tail, call);
			return;
		}
		case TYPE_SLICE:
		case TYPE_ARRAY:
		case TYPE_PTR:
		case TYPE_AMP: {
			if (t->inner && (t->inner->kind == TYPE_U8 || t->inner->kind == TYPE_CHAR)) {
				ASTNode *call = make_call_node(p, line, "__kawa_print_cstr", arg);
				append_print_stmt(head, tail, call);
				return;
			}
			break;
		}
		default:
			break;
		}
	}

	if (arg->type == NODE_STRING_LIT) {
		ASTNode *call = make_call_node(p, line, "__kawa_print_cstr", arg);
		append_print_stmt(head, tail, call);
		return;
	}

	ASTNode *call = make_call_node(p, line, "__kawa_print_i64", arg);
	append_print_stmt(head, tail, call);
}

static void append_print_nl(Parser *p, int line, ASTNode **head, ASTNode ***tail) {
	ASTNode *call = make_call_node(p, line, "__kawa_print_nl", NULL);
	append_print_stmt(head, tail, call);
}

static void desugar_print_call(Parser *p, ASTNode *call, ASTNode *head, int is_println) {
	p->uses_print = 1;
	ASTNode *stmts_head = NULL;
	ASTNode **stmts_tail = &stmts_head;

	// Case 1: no arguments: println() or print()
	if (!head) {
		if (is_println) append_print_nl(p, call->line, &stmts_head, &stmts_tail);
		call->type = NODE_BLOCK;
		call->data.block.stmts = stmts_head;
		Type *ret_t = arena_alloc(p->arena, sizeof(Type));
		ret_t->kind = TYPE_I32;
		call->data_type = ret_t;
		return;
	}

	// Case 2: first argument is NOT a string literal
	if (head->type != NODE_STRING_LIT) {
		append_print_expr(p, call->line, &stmts_head, &stmts_tail, head, NULL);
		for (ASTNode *extra = head->next; extra; extra = extra->next) {
			append_print_expr(p, call->line, &stmts_head, &stmts_tail, extra, NULL);
		}
		if (is_println) append_print_nl(p, call->line, &stmts_head, &stmts_tail);
		call->type = NODE_BLOCK;
		call->data.block.stmts = stmts_head;
		Type *ret_t = arena_alloc(p->arena, sizeof(Type));
		ret_t->kind = TYPE_I32;
		call->data_type = ret_t;
		return;
	}

	// Case 3: first argument is a string literal.
	const char *s = head->data.str_lit.s_val;
	size_t slen = head->data.str_lit.len;

	int has_interp = 0;
	for (size_t ci = 0; ci < slen; ci++) {
		if (s[ci] == '{' && (ci + 1 >= slen || s[ci+1] != '{')) {
			has_interp = 1;
			break;
		}
	}

	if (has_interp) {
		char *lit_buf = arena_alloc(p->arena, slen + 1);
		size_t lit_len = 0;
		size_t i = 0;
		while (i < slen) {
			if (s[i] == '{') {
				if (i + 1 < slen && s[i+1] == '{') {
					lit_buf[lit_len++] = '{';
					i += 2;
				} else {
					if (lit_len > 0) {
						append_print_str_lit(p, call->line, &stmts_head, &stmts_tail, lit_buf, lit_len);
						lit_len = 0;
					}
					i++; // skip '{'
					size_t start = i;
					int depth = 1;
					char in_quote = 0;
					while (i < slen && depth > 0) {
						if (in_quote) {
							if (s[i] == '\\' && i + 1 < slen) {
								i += 2;
								continue;
							}
							if (s[i] == in_quote) in_quote = 0;
						} else {
							if (s[i] == '"' || s[i] == '\'') in_quote = s[i];
							else if (s[i] == '{') depth++;
							else if (s[i] == '}') {
								depth--;
								if (depth == 0) break;
							}
						}
						i++;
					}
					if (depth > 0) {
						report_error(p, "Unclosed '{' in string interpolation");
						break;
					}
					size_t end = i;
					if (i < slen && s[i] == '}') i++;

					size_t content_len = end - start;
					char *content = arena_alloc(p->arena, content_len + 1);
					memcpy(content, s + start, content_len);
					content[content_len] = '\0';

					char *spec = NULL;
					int sub_depth = 0;
					char sub_quote = 0;
					int colon_idx = -1;
					for (size_t ci = 0; ci < content_len; ci++) {
						if (sub_quote) {
							if (content[ci] == '\\' && ci + 1 < content_len) { ci++; continue; }
							if (content[ci] == sub_quote) sub_quote = 0;
						} else {
							if (content[ci] == '"' || content[ci] == '\'') sub_quote = content[ci];
							else if (content[ci] == '(' || content[ci] == '[' || content[ci] == '{') sub_depth++;
							else if (content[ci] == ')' || content[ci] == ']' || content[ci] == '}') sub_depth--;
							else if (content[ci] == ':' && sub_depth == 0) {
								colon_idx = (int)ci;
								break;
							}
						}
					}

					char *expr_str = NULL;
					if (colon_idx >= 0) {
						expr_str = arena_alloc(p->arena, colon_idx + 1);
						memcpy(expr_str, content, colon_idx);
						expr_str[colon_idx] = '\0';
						spec = content + colon_idx + 1;
					} else {
						expr_str = content;
					}

					while (*expr_str == ' ' || *expr_str == '\t' || *expr_str == '\r' || *expr_str == '\n') expr_str++;
					size_t eslen = strlen(expr_str);
					while (eslen > 0 && (expr_str[eslen-1] == ' ' || expr_str[eslen-1] == '\t' || expr_str[eslen-1] == '\r' || expr_str[eslen-1] == '\n')) {
						expr_str[--eslen] = '\0';
					}

					if (eslen == 0) {
						report_error(p, "Empty expression in string interpolation");
					} else {
						Parser sub_p = *p;
						Lexer sub_lex;
						lexer_init(&sub_lex, expr_str, p->arena, p->lexer->filename);
						sub_p.lexer = &sub_lex;
						sub_p.cur = lexer_next(&sub_lex);
						ASTNode *arg_node = parse_expr(&sub_p);
						if (sub_p.had_error) p->had_error = 1;

						append_print_expr(p, call->line, &stmts_head, &stmts_tail, arg_node, spec);
					}
				}
			} else if (s[i] == '}') {
				if (i + 1 < slen && s[i+1] == '}') {
					lit_buf[lit_len++] = '}';
					i += 2;
				} else {
					lit_buf[lit_len++] = s[i++];
				}
			} else {
				lit_buf[lit_len++] = s[i++];
			}
		}
		if (lit_len > 0) {
			append_print_str_lit(p, call->line, &stmts_head, &stmts_tail, lit_buf, lit_len);
			lit_len = 0;
		}

		for (ASTNode *extra = head->next; extra; extra = extra->next) {
			append_print_expr(p, call->line, &stmts_head, &stmts_tail, extra, NULL);
		}
		if (is_println) {
			append_print_nl(p, call->line, &stmts_head, &stmts_tail);
		}
		call->type = NODE_BLOCK;
		call->data.block.stmts = stmts_head;
		Type *ret_t = arena_alloc(p->arena, sizeof(Type));
		ret_t->kind = TYPE_I32;
		call->data_type = ret_t;
		return;
	}

	// No interpolation syntax in string literal
	if (head->next != NULL) {
		int has_pct = 0;
		for (size_t ci = 0; ci < slen; ci++) {
			if (s[ci] == '%') { has_pct = 1; break; }
		}
		if (has_pct) {
			// Keep printf fallback for legacy %-style format strings
			ASTNode *new_callee = arena_alloc(p->arena, sizeof(ASTNode));
			new_callee->type = NODE_VAR_REF;
			new_callee->data.var_ref.name = arena_strdup(p->arena, "printf");
			call->data.call.callee = new_callee;
			Type *ret_t = arena_alloc(p->arena, sizeof(Type));
			ret_t->kind = TYPE_I32;
			call->data_type = ret_t;

			size_t cap = slen + 16;
			char *fmt_buf = arena_alloc(p->arena, cap);
			memcpy(fmt_buf, s, slen);
			size_t fmt_len = slen;
			if (is_println) fmt_buf[fmt_len++] = '\n';
			fmt_buf[fmt_len] = '\0';
			ASTNode *fmt_node = make_str_lit_node(p, call->line, fmt_buf);
			fmt_node->next = head->next;
			call->data.call.args = fmt_node;
			return;
		} else {
			append_print_str_lit(p, call->line, &stmts_head, &stmts_tail, s, slen);
			for (ASTNode *extra = head->next; extra; extra = extra->next) {
				append_print_expr(p, call->line, &stmts_head, &stmts_tail, extra, NULL);
			}
			if (is_println) append_print_nl(p, call->line, &stmts_head, &stmts_tail);
			call->type = NODE_BLOCK;
			call->data.block.stmts = stmts_head;
			Type *ret_t = arena_alloc(p->arena, sizeof(Type));
			ret_t->kind = TYPE_I32;
			call->data_type = ret_t;
			return;
		}
	} else {
		// Single plain string literal, e.g. println("hello world")
		append_print_str_lit(p, call->line, &stmts_head, &stmts_tail, s, slen);
		if (is_println) append_print_nl(p, call->line, &stmts_head, &stmts_tail);
		call->type = NODE_BLOCK;
		call->data.block.stmts = stmts_head;
		Type *ret_t = arena_alloc(p->arena, sizeof(Type));
		ret_t->kind = TYPE_I32;
		call->data_type = ret_t;
		return;
	}
}

static ASTNode *parse_postfix(Parser *p) {
	ASTNode *expr = parse_primary(p);
	while (1) {
		// TypeName { ... } in expression position: a struct literal. The
		// identifier must name a declared struct, so `if (x) { ... }`-style
		// blocks after bare identifiers never reach here.
		if (p->cur.type == TOK_LBRACE && expr->type == NODE_VAR_REF) {
			int names_struct = 0;
			ASTNode *st_node = NULL;
			for (int si = 0; si < p->struct_name_count; si++) {
				if (strcmp(p->struct_names[si],
						   expr->data.var_ref.name) == 0) {
					names_struct = 1;
					st_node = p->struct_nodes[si];
					break;
				}
			}
			if (!names_struct) {
				for (int gi = 0; gi < p->generic_struct_count; gi++) {
					if (strcmp(p->generic_structs[gi].name,
							   expr->data.var_ref.name) == 0) {
						names_struct = 1;
						st_node = p->generic_structs[gi].node;
						break;
					}
				}
			}
			if (names_struct) {
				if (st_node && st_node->module_name && p->cur_module &&
					strcmp(st_node->module_name, p->cur_module) != 0 && !st_node->is_pub) {
					report_error(p, "E0006: struct '%s' is private to its module",
								 st_node->data.struct_decl.name);
				}
				Type *st_t = arena_alloc(p->arena, sizeof(Type));
				st_t->kind = TYPE_STRUCT;
				st_t->name = expr->data.var_ref.name;
				ASTNode *lit = parse_struct_literal(p);
				lit->data_type = st_t;
				if (st_node && st_node->module_name && p->cur_module &&
					strcmp(st_node->module_name, p->cur_module) != 0) {
					for (StructInitItem *it = lit->data.struct_lit.items; it; it = it->next) {
						if (it->field_name) {
							for (ASTNode *f = st_node->data.struct_decl.fields; f; f = f->next) {
								if (strcmp(f->data.var_decl.name, it->field_name) == 0) {
									if (!f->is_pub) {
										report_error(p, "E0006: field '%s' on struct '%s' is private to its module",
													 it->field_name, st_node->data.struct_decl.name);
									}
									break;
								}
							}
						}
					}
				}
				return lit;
			}
		}
		if (p->cur.type == TOK_DOT && expr->type == NODE_VAR_REF) {
			const char *sname = expr->data.var_ref.name;
			Token next_tok = lexer_peek(p->lexer);
			if (is_enum_name(p, sname) && next_tok.type == TOK_IDENTIFIER) {
				EnumVariant *ev = find_enum_variant_in_parser(p, sname, next_tok.text);
				if (ev) {
					advance(p); // eat '.'
					advance(p); // eat variant name
					char mangled[256];
					snprintf(mangled, sizeof(mangled), "%s_%s", sname, ev->name);
					Type *en_t = arena_alloc(p->arena, sizeof(Type));
					en_t->kind = TYPE_ENUM;
					en_t->name = (char *)sname;

					if (ev->payload_count == 0 && p->cur.type != TOK_LPAREN) {
						ASTNode *call = arena_alloc(p->arena, sizeof(ASTNode));
						call->type = NODE_CALL;
						call->line = p->prev.line;
						ASTNode *callee = arena_alloc(p->arena, sizeof(ASTNode));
						callee->type = NODE_VAR_REF;
						callee->data.var_ref.name = arena_strdup(p->arena, mangled);
						call->data.call.callee = callee;
						call->data.call.args = NULL;
						call->data_type = en_t;
						expr = call;
						continue;
					} else {
						ASTNode *vref = arena_alloc(p->arena, sizeof(ASTNode));
						vref->type = NODE_VAR_REF;
						vref->data.var_ref.name = arena_strdup(p->arena, mangled);
						vref->data_type = en_t;
						vref->line = p->prev.line;
						expr = vref;
						continue;
					}
				}
			}
			if (next_tok.type == TOK_IDENTIFIER) {
				char mangled[256];
				snprintf(mangled, sizeof(mangled), "%s__%s", sname, next_tok.text);
				ASTNode *cdecl = find_decl(p, mangled);
				if (cdecl && cdecl->type == NODE_VAR_DECL && cdecl->data.var_decl.is_const) {
					advance(p); // eat '.'
					advance(p); // eat identifier
					if (cdecl->module_name && p->cur_module &&
						strcmp(cdecl->module_name, p->cur_module) != 0 && !cdecl->is_pub) {
						report_error(p, "E0006: associated constant '%s' on struct '%s' is private to its module",
									 next_tok.text, sname);
					}
					ASTNode *cref = arena_alloc(p->arena, sizeof(ASTNode));
					cref->type = NODE_VAR_REF;
					cref->data.var_ref.name = arena_strdup(p->arena, mangled);
					cref->data_type = cdecl->data_type;
					cref->line = p->prev.line;
					expr = cref;
					continue;
				}
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
				if (is_ident_like(p->cur.type))
					advance(p);
				else
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
			char *mname = NULL;
			if (p->cur.type == TOK_INT_LIT) {
				char buf[32];
				snprintf(buf, sizeof(buf), "_%s", p->cur.text);
				mname = arena_strdup(p->arena, buf);
				advance(p);
			} else if (is_ident_like(p->cur.type)) {
				mname = p->cur.text;
				advance(p);
			} else {
				mname = p->cur.text;
				consume(p, TOK_IDENTIFIER, "Expected member name");
			}
			member->data.member_access.member = mname;
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
				if (!obj_t)
					obj_t = deduce_node_type(p, expr);
				if (obj_t && obj_t->kind == TYPE_PTR && obj_t->inner)
					obj_t = obj_t->inner;
				if (obj_t && obj_t->kind == TYPE_STRUCT && obj_t->name) {
					for (int si = 0; si < p->struct_name_count; si++) {
						if (strcmp(p->struct_names[si], obj_t->name) == 0) {
							for (ASTNode *f = p->struct_nodes[si]->data.struct_decl.fields; f; f = f->next) {
								if (strcmp(f->data.var_decl.name, member->data.member_access.member) == 0) {
									member->data_type = f->data_type;
									if (p->cur.type != TOK_LPAREN && f->module_name && p->cur_module &&
										strcmp(f->module_name, p->cur_module) != 0 && !f->is_pub) {
										report_error(p, "E0006: field '%s' on struct '%s' is private to its module",
													 f->data.var_decl.name, obj_t->name);
									}
									break;
								}
							}
							break;
						}
					}
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
			char *arr_mname = NULL;
			if (p->cur.type == TOK_INT_LIT) {
				char buf[32];
				snprintf(buf, sizeof(buf), "_%s", p->cur.text);
				arr_mname = arena_strdup(p->arena, buf);
				advance(p);
			} else if (is_ident_like(p->cur.type)) {
				arr_mname = p->cur.text;
				advance(p);
			} else {
				arr_mname = p->cur.text;
				consume(p, TOK_IDENTIFIER, "Expected member name after ->");
			}
			member->data.member_access.member = arr_mname;
			if (deref->data_type && deref->data_type->kind == TYPE_STRUCT && deref->data_type->name) {
				for (int si = 0; si < p->struct_name_count; si++) {
					if (strcmp(p->struct_names[si], deref->data_type->name) == 0) {
						for (ASTNode *f = p->struct_nodes[si]->data.struct_decl.fields; f; f = f->next) {
							if (strcmp(f->data.var_decl.name, member->data.member_access.member) == 0) {
								member->data_type = f->data_type;
								if (p->cur.type != TOK_LPAREN && f->module_name && p->cur_module &&
									strcmp(f->module_name, p->cur_module) != 0 && !f->is_pub) {
									report_error(p, "E0006: field '%s' on struct '%s' is private to its module",
												 f->data.var_decl.name, deref->data_type->name);
								}
								break;
							}
						}
						break;
					}
				}
			}
			expr = member;
		} else if (p->cur.type == TOK_LBRACKET) {
			advance(p);
			ASTNode *idx = NULL;
			int is_slice = 0;
			int is_inclusive = 0;
			ASTNode *start_node = NULL;
			ASTNode *end_node = NULL;

			if (p->cur.type == TOK_DOTDOT || p->cur.type == TOK_DOTDOTEQ) {
				is_slice = 1;
				is_inclusive = (p->cur.type == TOK_DOTDOTEQ);
				advance(p);
				if (p->cur.type != TOK_RBRACKET) {
					end_node = parse_expr(p);
				}
			} else {
				idx = parse_expr(p);
				if (p->cur.type == TOK_DOTDOT || p->cur.type == TOK_DOTDOTEQ) {
					is_slice = 1;
					is_inclusive = (p->cur.type == TOK_DOTDOTEQ);
					start_node = idx;
					advance(p);
					if (p->cur.type != TOK_RBRACKET) {
						end_node = parse_expr(p);
					}
				} else if (idx && idx->type == NODE_RANGE) {
					is_slice = 1;
					is_inclusive = idx->data.range.is_inclusive;
					start_node = idx->data.range.left;
					end_node = idx->data.range.right;
				}
			}
			consume(p, TOK_RBRACKET, "Expected ']' after index");

			if (is_slice) {
				ASTNode *sindex = arena_alloc(p->arena, sizeof(ASTNode));
				sindex->type = NODE_SLICE_INDEX;
				sindex->line = p->cur.line;
				sindex->data.slice_index.object = expr;
				sindex->data.slice_index.start = start_node;
				sindex->data.slice_index.end = end_node;
				sindex->data.slice_index.is_inclusive = is_inclusive;

				Type *elem_t = NULL;
				if (expr->data_type && (expr->data_type->kind == TYPE_ARRAY ||
										expr->data_type->kind == TYPE_PTR ||
										expr->data_type->kind == TYPE_SLICE ||
										expr->data_type->kind == TYPE_AMP)) {
					elem_t = expr->data_type->inner;
				} else if (expr->type == NODE_VAR_REF) {
					ASTNode *decl = find_decl(p, expr->data.var_ref.name);
					if (decl && decl->data_type &&
						(decl->data_type->kind == TYPE_ARRAY ||
						 decl->data_type->kind == TYPE_PTR ||
						 decl->data_type->kind == TYPE_SLICE ||
						 decl->data_type->kind == TYPE_AMP)) {
						elem_t = decl->data_type->inner;
					}
				}
				if (!elem_t) {
					elem_t = arena_alloc(p->arena, sizeof(Type));
					elem_t->kind = TYPE_I32;
				}
				Type *slice_t = arena_alloc(p->arena, sizeof(Type));
				slice_t->kind = TYPE_SLICE;
				slice_t->inner = elem_t;
				sindex->data_type = slice_t;
				expr = sindex;
				continue;
			}

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
			// Operator-provided indexing (IDEAS 1.2): `g[i]` on a struct
			// with impl `self_index` becomes that call -- the expression's
			// type is the METHOD'S return type, stamped here so downstream
			// consumers (varargs str decay, coercions) see it.
			if (!index->data_type) {
				Type *ibase = expr->data_type;
				if (!ibase && expr->type == NODE_VAR_REF) {
					ASTNode *idecl =
						find_decl(p, expr->data.var_ref.name);
					ibase = idecl ? idecl->data_type : NULL;
				}
				if (ibase && ibase->kind == TYPE_STRUCT && ibase->name &&
					strlen(ibase->name) > 1) {
					char imethod[512];
					snprintf(imethod, sizeof(imethod), "%s__self_index",
							 ibase->name);
					for (int ik = p->fn_sig_count - 1; ik >= 0; ik--) {
						if (strcmp(p->fn_sigs[ik].name, imethod) == 0) {
							index->data_type = p->fn_sigs[ik].ret;
							break;
						}
					}
				}
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
				if (!recv_t) {
					recv_t = deduce_node_type(p, self_obj);
					if (recv_t && recv_t->kind == TYPE_PTR && recv_t->inner)
						recv_t = recv_t->inner;
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
			while (!p->had_error && p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF) {
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

				ASTNode *mdecl = find_decl(p, mangled);
				if (mdecl && mdecl->type == NODE_FUNC_DECL) {
					if (mdecl->module_name && p->cur_module &&
						strcmp(mdecl->module_name, p->cur_module) != 0 && !mdecl->is_pub) {
						report_error(p, "E0006: method '%s' on struct '%s' is private to its module",
									 method_name, struct_name);
					}
				}

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
				int is_print = 0;
				int is_println = 0;
				if (expr->type == NODE_VAR_REF) {
					ASTNode *decl = find_decl(p, expr->data.var_ref.name);
					if (!decl || decl->type != NODE_FUNC_DECL) {
						if (strcmp(expr->data.var_ref.name, "print") == 0) is_print = 1;
						else if (strcmp(expr->data.var_ref.name, "println") == 0) is_println = 1;
					}
				} else if (expr->type == NODE_MEMBER_ACCESS &&
						   expr->data.member_access.object->type == NODE_VAR_REF &&
						   strcmp(expr->data.member_access.object->data.var_ref.name, "stdc") == 0) {
					if (strcmp(expr->data.member_access.member, "print") == 0) is_print = 1;
					else if (strcmp(expr->data.member_access.member, "println") == 0) is_println = 1;
				}

				if (is_print || is_println) {
					desugar_print_call(p, call, head, is_println);
				} else {
					// Standard function call behavior
					call->data.call.callee = expr;
					call->data.call.args = head;
					if (expr->type == NODE_VAR_REF) {
						const char *fn_name = expr->data.var_ref.name;
						ASTNode *decl = find_decl(p, fn_name);
						if (decl && decl->type == NODE_FUNC_DECL) {
							if (decl->module_name && p->cur_module &&
								strcmp(decl->module_name, p->cur_module) != 0 && !decl->is_pub) {
								report_error(p, "E0006: function '%s' is private to its module", fn_name);
							}
						}
					}
				}
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
			// The process API returns a sized byte view, including for inferred locals.
			ASTNode *std_call = call->type == NODE_CALL ? call->data.call.callee : NULL;
			if (std_call && std_call->type == NODE_MEMBER_ACCESS &&
				strcmp(std_call->data.member_access.member, "arg_at") == 0) {
				ASTNode *mid = std_call->data.member_access.object;
				if (mid && mid->type == NODE_MEMBER_ACCESS && strcmp(mid->data.member_access.member, "process") == 0 &&
					mid->data.member_access.object->type == NODE_VAR_REF &&
					strcmp(mid->data.member_access.object->data.var_ref.name, "std") == 0) {
					Type *byte = arena_alloc(p->arena, sizeof(Type)); byte->kind = TYPE_U8;
					Type *view = arena_alloc(p->arena, sizeof(Type)); view->kind = TYPE_SLICE; view->inner = byte;
					call->data_type = view;
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
		if (p->cur.type == TOK_DOTDOT || p->cur.type == TOK_DOTDOTEQ)
			tok_prec = 3;
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
		ASTNode *rhs = NULL;
		if (op == TOK_DOTDOT && (p->cur.type == TOK_RBRACKET || p->cur.type == TOK_RPAREN ||
								 p->cur.type == TOK_SEMICOLON || p->cur.type == TOK_COMMA)) {
			rhs = NULL;
		} else {
			rhs = parse_binop_rhs(p, tok_prec + 1, parse_unary(p));
		}
		if (op == TOK_DOTDOT || op == TOK_DOTDOTEQ) {
			ASTNode *rnode = arena_alloc(p->arena, sizeof(ASTNode));
			rnode->type = NODE_RANGE;
			rnode->data.range.left = lhs;
			rnode->data.range.right = rhs;
			rnode->data.range.is_inclusive = (op == TOK_DOTDOTEQ);
			rnode->data_type = lhs ? lhs->data_type : (rhs ? rhs->data_type : NULL);
			lhs = rnode;
		} else if (op == TOK_TILDE_EQ) {
			ASTNode *pour = arena_alloc(p->arena, sizeof(ASTNode));
			pour->type = NODE_SET_POUR;
			pour->data.set_pour.target = lhs;
			pour->data.set_pour.value = rhs;
			lhs = pour;
		} else {
			// Whitelisted operator overloading (IDEAS 1.1): a struct with
			// the matching self_* method turns the binop into that call.
			ASTNode *ov = try_op_overload(p, op, lhs, rhs);
			if (ov) {
				lhs = ov; // becomes the LHS of any following operator
				continue;
			}
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
	while (!p->had_error && p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
		*tail = parse_statement(p);
		while (*tail)
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

static ASTNode *parse_destructuring_let(Parser *p) {
	consume(p, TOK_LET, "Expected 'let'");
	char *named_struct = NULL;
	int is_tuple = 0;

	if (p->cur.type == TOK_LPAREN) {
		is_tuple = 1;
		advance(p);
	} else if (p->cur.type == TOK_IDENTIFIER) {
		named_struct = p->cur.text;
		advance(p);
		consume(p, TOK_LBRACE, "Expected '{' after struct name in destructuring let");
	} else if (p->cur.type == TOK_LBRACE) {
		advance(p);
	} else {
		report_error(p, "Expected '{' or '(' after 'let'");
		return NULL;
	}

	struct {
		char *field_name;
		char *var_name;
		Type *var_type;
	} items[32];
	int item_count = 0;

	if (is_tuple) {
		while (!p->had_error && p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF) {
			char *vname = p->cur.text;
			consume(p, TOK_IDENTIFIER, "Expected variable name in tuple destructuring");
			Type *vtype = NULL;
			if (p->cur.type == TOK_COLON) {
				advance(p);
				vtype = parse_type(p);
			}
			if (item_count < 32) {
				items[item_count].field_name = NULL;
				items[item_count].var_name = vname;
				items[item_count].var_type = vtype;
				item_count++;
			}
			if (p->cur.type == TOK_COMMA)
				advance(p);
			else
				break;
		}
		consume(p, TOK_RPAREN, "Expected ')' in tuple destructuring");
	} else {
		while (!p->had_error && p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
			char *fname = p->cur.text;
			consume(p, TOK_IDENTIFIER, "Expected field name in struct destructuring");
			char *vname = fname;
			Type *vtype = NULL;
			if (p->cur.type == TOK_COLON) {
				advance(p);
				if (is_type_token(p->cur.type) && p->cur.type != TOK_IDENTIFIER) {
					vtype = parse_type(p);
				} else if (p->cur.type == TOK_IDENTIFIER) {
					char *ident = p->cur.text;
					advance(p);
					if (p->cur.type == TOK_COLON) {
						vname = ident;
						advance(p);
						vtype = parse_type(p);
					} else {
						int is_type = 0;
						for (int si = 0; si < p->struct_name_count; si++) {
							if (strcmp(p->struct_names[si], ident) == 0) {
								is_type = 1;
								break;
							}
						}
						if (is_type) {
							Type *t = arena_alloc(p->arena, sizeof(Type));
							t->kind = TYPE_STRUCT;
							t->name = ident;
							vtype = t;
						} else {
							vname = ident;
						}
					}
				}
			}
			if (item_count < 32) {
				items[item_count].field_name = fname;
				items[item_count].var_name = vname;
				items[item_count].var_type = vtype;
				item_count++;
			}
			if (p->cur.type == TOK_COMMA)
				advance(p);
			else
				break;
		}
		consume(p, TOK_RBRACE, "Expected '}' in struct destructuring");
	}

	consume(p, TOK_ASSIGN, "Expected '=' in destructuring let");
	ASTNode *rhs = parse_expr(p);
	consume(p, TOK_SEMICOLON, "Expected ';' after destructuring let");

	static int s_destruct_id = 0;
	char tmp_name[64];
	snprintf(tmp_name, sizeof(tmp_name), "__destruct_%d", ++s_destruct_id);
	char *tmp_var = arena_strdup(p->arena, tmp_name);

	Type *st_type = NULL;
	if (named_struct) {
		st_type = find_struct_type_by_name(p, named_struct);
	}
	if (!st_type && rhs->data_type) {
		st_type = rhs->data_type;
	}
	if (!st_type && rhs->type == NODE_VAR_REF) {
		ASTNode *rdecl = find_decl(p, rhs->data.var_ref.name);
		if (rdecl && rdecl->data_type)
			st_type = rdecl->data_type;
	}
	if (!st_type) {
		st_type = deduce_node_type(p, rhs);
	}
	if (st_type && !rhs->data_type) {
		rhs->data_type = st_type;
	}

	ASTNode *tmp_decl = arena_alloc(p->arena, sizeof(ASTNode));
	tmp_decl->type = NODE_VAR_DECL;
	tmp_decl->data.var_decl.name = tmp_var;
	tmp_decl->data.var_decl.init = rhs;
	tmp_decl->data.var_decl.is_const = 0;
	tmp_decl->data.var_decl.is_orbit = 0;
	tmp_decl->data_type = rhs->data_type ? rhs->data_type : st_type;

	if (p->decl_count < 1024) {
		p->decls[p->decl_count].name = tmp_var;
		p->decls[p->decl_count].node = tmp_decl;
		p->decl_count++;
	}

	ASTNode *head = tmp_decl;
	ASTNode *cur = head;

	for (int i = 0; i < item_count; i++) {
		ASTNode *tmp_ref = arena_alloc(p->arena, sizeof(ASTNode));
		tmp_ref->type = NODE_VAR_REF;
		tmp_ref->data.var_ref.name = tmp_var;
		tmp_ref->data_type = tmp_decl->data_type;

		ASTNode *val_expr = NULL;
		const char *fld = items[i].field_name;

		if (is_tuple && !fld) {
			if (st_type && st_type->kind == TYPE_STRUCT && st_type->name) {
				for (int si = 0; si < p->struct_name_count; si++) {
					if (strcmp(p->struct_names[si], st_type->name) == 0) {
						ASTNode *f = p->struct_nodes[si]->data.struct_decl.fields;
						for (int fi = 0; fi < i && f; fi++)
							f = f->next;
						if (f)
							fld = f->data.var_decl.name;
						break;
					}
				}
				if (!fld) {
					char num_buf[16];
					snprintf(num_buf, sizeof(num_buf), "_%d", i);
					fld = arena_strdup(p->arena, num_buf);
				}
			}
		}

		if (fld) {
			ASTNode *mem = arena_alloc(p->arena, sizeof(ASTNode));
			mem->type = NODE_MEMBER_ACCESS;
			mem->data.member_access.object = tmp_ref;
			mem->data.member_access.member = (char *)fld;
			if (st_type)
				mem->data_type = find_field_type(p, st_type, fld);
			val_expr = mem;
		} else {
			ASTNode *idx = arena_alloc(p->arena, sizeof(ASTNode));
			idx->type = NODE_LITERAL;
			idx->data_type = arena_alloc(p->arena, sizeof(Type));
			idx->data_type->kind = TYPE_I32;
			idx->data.literal.i_val = i;
			idx->data.literal.i64_val = i;

			ASTNode *arr_idx = arena_alloc(p->arena, sizeof(ASTNode));
			arr_idx->type = NODE_INDEX;
			arr_idx->data.index.object = tmp_ref;
			arr_idx->data.index.index = idx;
			if (st_type && (st_type->kind == TYPE_ARRAY || st_type->kind == TYPE_SLICE))
				arr_idx->data_type = st_type->inner;
			val_expr = arr_idx;
		}

		ASTNode *var_node = arena_alloc(p->arena, sizeof(ASTNode));
		var_node->type = NODE_VAR_DECL;
		var_node->data.var_decl.name = items[i].var_name;
		var_node->data.var_decl.init = val_expr;
		var_node->data.var_decl.is_const = 0;
		var_node->data.var_decl.is_orbit = 0;
		var_node->data_type = items[i].var_type ? items[i].var_type :
			(val_expr->data_type ? val_expr->data_type : NULL);

		if (p->decl_count < 1024) {
			p->decls[p->decl_count].name = items[i].var_name;
			p->decls[p->decl_count].node = var_node;
			p->decl_count++;
		}

		cur->next = var_node;
		cur = var_node;
	}

	return head;
}

static void parse_const_decl(Parser *p, ASTNode ***tail, const char *prefix,
							 int is_pub) {
	consume(p, TOK_CONST, "Expected 'const'");
	Type *type = NULL;
	if (is_type_token(p->cur.type)) {
		Token after = lexer_peek(p->lexer);
		if (p->cur.type != TOK_IDENTIFIER || after.type == TOK_IDENTIFIER || after.type == TOK_STAR)
			type = parse_type(p);
	}
	char *cname = p->cur.text;
	consume(p, TOK_IDENTIFIER, "Expected const identifier");
	if (p->cur.type == TOK_COLON) {
		advance(p);
		type = parse_type(p);
	}
	consume(p, TOK_ASSIGN, "Expected '='");
	ASTNode *init = parse_expr(p);
	consume(p, TOK_SEMICOLON, "Expected ';'");

	char *mangled = cname;
	if (prefix) {
		size_t len = strlen(prefix) + strlen(cname) + 4;
		char *buf = arena_alloc(p->arena, len);
		snprintf(buf, len, "%s__%s", prefix, cname);
		mangled = buf;
	}

	if (!type && init && init->data_type)
		type = init->data_type;
	if (!type) {
		type = arena_alloc(p->arena, sizeof(Type));
		type->kind = TYPE_I32;
	}

	ASTNode *node = arena_alloc(p->arena, sizeof(ASTNode));
	node->type = NODE_VAR_DECL;
	node->data.var_decl.name = mangled;
	node->data.var_decl.init = init;
	node->data.var_decl.is_const = 1;
	node->data.var_decl.is_orbit = 0;
	node->data_type = type;
	node->is_pub = is_pub;
	node->module_name = p->cur_module;

	if (p->decl_count < 1024) {
		p->decls[p->decl_count].name = mangled;
		p->decls[p->decl_count].node = node;
		p->decl_count++;
	}

	**tail = node;
	*tail = &node->next;
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

static ASTNode *parse_match(Parser *p) {
	advance(p); // eat `match`
	ASTNode *target = parse_expr(p);
	consume(p, TOK_LBRACE, "Expected '{' after match target");

	ASTNode *arms_head = NULL;
	ASTNode **arms_tail = &arms_head;
	Type *inferred_arm_type = NULL;

	while (!p->had_error && p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
		ASTNode *arm = arena_alloc(p->arena, sizeof(ASTNode));
		arm->type = NODE_MATCH_ARM;
		arm->line = p->cur.line;

		if (p->cur.type == TOK_ELSE ||
			(p->cur.type == TOK_IDENTIFIER && strcmp(p->cur.text, "_") == 0)) {
			arm->data.match_arm.is_else = 1;
			advance(p);
		} else {
			char *vname = p->cur.text;
			consume(p, TOK_IDENTIFIER, "Expected variant name in pattern");
			char *ename = NULL;
			if (p->cur.type == TOK_DOT) {
				ename = vname;
				advance(p); // eat '.'
				vname = p->cur.text;
				consume(p, TOK_IDENTIFIER, "Expected variant name after '.'");
			} else {
				ename = (char *)find_enum_name_by_variant(p, vname);
			}
			arm->data.match_arm.enum_name = ename;
			arm->data.match_arm.variant_name = vname;

			EnumVariant *ev = find_enum_variant_in_parser(p, ename, vname);

			if (p->cur.type == TOK_LPAREN) {
				advance(p); // eat '('
				ASTNode *bhead = NULL;
				ASTNode **btail = &bhead;
				int bi = 0;
				while (!p->had_error && p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF) {
					char *bname = p->cur.text;
					consume(p, TOK_IDENTIFIER, "Expected identifier in pattern");
					ASTNode *bdecl = arena_alloc(p->arena, sizeof(ASTNode));
					bdecl->type = NODE_VAR_DECL;
					bdecl->data.var_decl.name = bname;
					if (ev && bi < ev->payload_count) {
						bdecl->data_type = ev->payload_types[bi];
					} else {
						Type *it = arena_alloc(p->arena, sizeof(Type));
						it->kind = TYPE_I64;
						bdecl->data_type = it;
					}
					*btail = bdecl;
					btail = &bdecl->next;
					bi++;
					if (p->cur.type == TOK_COMMA)
						advance(p);
					else
						break;
				}
				consume(p, TOK_RPAREN, "Expected ')' after pattern bindings");
				arm->data.match_arm.bindings = bhead;
			}
		}

		consume(p, TOK_FAT_ARROW, "Expected '=>' after pattern");

		// Register pattern bindings into parser decls for scope resolution in arm body
		int saved_decls = p->decl_count;
		for (ASTNode *b = arm->data.match_arm.bindings; b; b = b->next) {
			if (p->decl_count < 1024) {
				p->decls[p->decl_count].name = b->data.var_decl.name;
				p->decls[p->decl_count].node = b;
				p->decl_count++;
			}
		}

		ASTNode *body = NULL;
		if (p->cur.type == TOK_LBRACE) {
			body = parse_block(p);
		} else {
			body = parse_expr(p);
		}
		p->decl_count = saved_decls; // unbind arm pattern variables

		arm->data.match_arm.body = body;
		if (body && body->data_type && !inferred_arm_type) {
			inferred_arm_type = body->data_type;
		}

		*arms_tail = arm;
		arms_tail = &arm->next;

		if (p->cur.type == TOK_COMMA)
			advance(p);
	}
	consume(p, TOK_RBRACE, "Expected '}' after match arms");

	ASTNode *mnode = arena_alloc(p->arena, sizeof(ASTNode));
	mnode->type = NODE_MATCH;
	mnode->data.match_stmt.target = target;
	mnode->data.match_stmt.arms = arms_head;
	mnode->data_type = inferred_arm_type;
	return mnode;
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
	} else if (p->cur.type == TOK_LPAREN) {
		Lexer temp = *p->lexer;
		Token t = lexer_next(&temp);
		int depth = 1;
		int looks_like_tuple = 0;
		while (t.type != TOK_EOF) {
			if (t.type == TOK_LPAREN) depth++;
			else if (t.type == TOK_RPAREN) {
				depth--;
				if (depth == 0) {
					Token after = lexer_next(&temp);
					if ((after.type == TOK_IDENTIFIER || after.type == TOK_STAR) && looks_like_tuple) {
						is_c_style_decl = 1;
					}
					break;
				}
			} else if (t.type == TOK_COMMA) {
				looks_like_tuple = 1;
			}
			t = lexer_next(&temp);
		}
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
			while (!p->had_error && p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
				*tail = parse_statement(p);
				while (*tail)
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
	if (p->cur.type == TOK_LET) {
		Lexer temp = *p->lexer;
		Token t1 = lexer_next(&temp);
		if (t1.type == TOK_LBRACE) {
			return parse_destructuring_let(p);
		}
		if (t1.type == TOK_LPAREN) {
			int depth = 1;
			Token t = lexer_next(&temp);
			while (t.type != TOK_EOF) {
				if (t.type == TOK_LPAREN) depth++;
				else if (t.type == TOK_RPAREN) {
					depth--;
					if (depth == 0) {
						Token after = lexer_next(&temp);
						if (after.type == TOK_ASSIGN) {
							return parse_destructuring_let(p);
						}
						break;
					}
				}
				t = lexer_next(&temp);
			}
		}
		if (t1.type == TOK_IDENTIFIER) {
			Token t2 = lexer_next(&temp);
			if (t2.type == TOK_LBRACE) {
				return parse_destructuring_let(p);
			}
		}
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
			// Optional explicit type: `const u32 X = ...` or `let (i32, str) t = ...`.
			if (is_type_token(p->cur.type) || p->cur.type == TOK_LPAREN) {
				Token after = lexer_peek(p->lexer);
				if (p->cur.type == TOK_LPAREN ||
					p->cur.type != TOK_IDENTIFIER ||
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
	if (p->cur.type == TOK_MATCH) {
		return parse_match(p);
	}
	if (p->cur.type == TOK_FOR) {
		advance(p);
		int has_paren = 0;
		int is_for_in = 0;

		if (p->cur.type == TOK_LPAREN) {
			Lexer temp = *p->lexer;
			Token t1 = lexer_next(&temp);
			if (t1.type == TOK_IN) {
				has_paren = 1;
				is_for_in = 1;
			} else if (t1.type == TOK_IDENTIFIER || t1.type == TOK_LET) {
				Token t2 = lexer_next(&temp);
				if (t2.type == TOK_IN) {
					has_paren = 1;
					is_for_in = 1;
				} else if (t2.type == TOK_IDENTIFIER) {
					Token t3 = lexer_next(&temp);
					if (t3.type == TOK_IN) {
						has_paren = 1;
						is_for_in = 1;
					}
				}
			}
		} else {
			is_for_in = 1;
		}

		if (is_for_in) {
			if (has_paren)
				advance(p); // eat '('
			if (p->cur.type == TOK_LET)
				advance(p); // optional 'let'
			char *iter = p->cur.text;
			consume(p, TOK_IDENTIFIER, "Expected loop variable name");
			consume(p, TOK_IN, "Expected 'in'");
			ASTNode *iter_expr = parse_expr(p);
			if (has_paren)
				consume(p, TOK_RPAREN, "Expected ')'");
			ASTNode *body = parse_block(p);

			if (iter_expr && iter_expr->type == NODE_RANGE) {
				ASTNode *start = iter_expr->data.range.left;
				ASTNode *end = iter_expr->data.range.right;
				int is_inc = iter_expr->data.range.is_inclusive;

				if (!start) {
					start = arena_alloc(p->arena, sizeof(ASTNode));
					start->type = NODE_LITERAL;
					start->data.literal.i_val = 0;
					start->data.literal.i64_val = 0;
					Type *i64_t = arena_alloc(p->arena, sizeof(Type));
					i64_t->kind = TYPE_I64;
					start->data_type = i64_t;
				}

				Type *idx_type = start->data_type ? start->data_type
								: (end && end->data_type ? end->data_type : NULL);
				if (!idx_type) {
					idx_type = arena_alloc(p->arena, sizeof(Type));
					idx_type->kind = TYPE_I64;
				}

				ASTNode *init = arena_alloc(p->arena, sizeof(ASTNode));
				init->type = NODE_VAR_DECL;
				init->data.var_decl.name = iter;
				init->data.var_decl.init = start;
				init->data.var_decl.is_const = 0;
				init->data_type = idx_type;

				ASTNode *iter_ref = arena_alloc(p->arena, sizeof(ASTNode));
				iter_ref->type = NODE_VAR_REF;
				iter_ref->data.var_ref.name = iter;
				iter_ref->data_type = idx_type;

				ASTNode *cond = arena_alloc(p->arena, sizeof(ASTNode));
				cond->type = NODE_BINARY_OP;
				cond->data.bin_op.op = is_inc ? TOK_LEQ : TOK_LANGLE;
				cond->data.bin_op.left = iter_ref;
				cond->data.bin_op.right = end;
				cond->data_type = bool_result_type(p);

				ASTNode *step_ref = arena_alloc(p->arena, sizeof(ASTNode));
				step_ref->type = NODE_VAR_REF;
				step_ref->data.var_ref.name = iter;
				step_ref->data_type = idx_type;

				ASTNode *one_lit = arena_alloc(p->arena, sizeof(ASTNode));
				one_lit->type = NODE_LITERAL;
				one_lit->data.literal.i_val = 1;
				one_lit->data.literal.i64_val = 1;
				one_lit->data_type = idx_type;

				ASTNode *add_one = arena_alloc(p->arena, sizeof(ASTNode));
				add_one->type = NODE_BINARY_OP;
				add_one->data.bin_op.op = TOK_PLUS;
				add_one->data.bin_op.left = step_ref;
				add_one->data.bin_op.right = one_lit;
				add_one->data_type = idx_type;

				ASTNode *step = arena_alloc(p->arena, sizeof(ASTNode));
				step->type = NODE_ASSIGN;
				step->data.assign.target = step_ref;
				step->data.assign.value = add_one;

				ASTNode *loop = arena_alloc(p->arena, sizeof(ASTNode));
				loop->type = NODE_FOR;
				loop->data.for_stmt.init = init;
				loop->data.for_stmt.cond = cond;
				loop->data.for_stmt.step = step;
				loop->data.for_stmt.body = body;
				return loop;
			} else {
				ASTNode *batch = arena_alloc(p->arena, sizeof(ASTNode));
				batch->type = NODE_BATCH;
				batch->data.batch.iterator_var = iter;
				batch->data.batch.collection = iter_expr;
				batch->data.batch.body = body;
				return batch;
			}
		}

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
		while (!p->had_error && p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
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
				while (*body_tail)
					body_tail = &(*body_tail)->next;
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
		ASTNode *captures = NULL;
		ASTNode **cap_tail = &captures;
		if (p->cur.type == TOK_LPAREN) {
			advance(p); // eat '('
			while (!p->had_error && p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF) {
				char *cname = p->cur.text;
				consume(p, TOK_IDENTIFIER, "Expected variable name in defer capture list");
				ASTNode *cnode = arena_alloc(p->arena, sizeof(ASTNode));
				cnode->type = NODE_VAR_DECL;
				cnode->data.var_decl.name = cname;
				*cap_tail = cnode;
				cap_tail = &cnode->next;
				if (p->cur.type == TOK_COMMA)
					advance(p);
				else
					break;
			}
			consume(p, TOK_RPAREN, "Expected ')' after defer capture list");
		}
		ASTNode *defer = arena_alloc(p->arena, sizeof(ASTNode));
		defer->type = NODE_DEFER;
		defer->data.defer.captures = captures;
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
		while (!p->had_error && p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
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
		p->cur.type == TOK_SLASH_EQ || p->cur.type == TOK_PERCENT_EQ ||
		p->cur.type == TOK_AND_EQ || p->cur.type == TOK_OR_EQ ||
		p->cur.type == TOK_XOR_EQ || p->cur.type == TOK_SHL_EQ ||
		p->cur.type == TOK_SHR_EQ) {
		int op = p->cur.type;
		advance(p); // Eat '=' / '+=' / '-=' / '*=' / '/=' / '%='

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
			case TOK_SLASH_EQ:
				bin_op = TOK_SLASH;
				break;
			case TOK_PERCENT_EQ:
				bin_op = TOK_PERCENT;
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
			// Operator overloading applies to compound assigns too:
			// `v1 += v2` rewrites to Vec2__self_add(v1, v2) when defined.
			ASTNode *ov =
				try_op_overload(p, bin_op, expr, value);
			if (ov) {
				assign->data.assign.value = ov;
				if (need_semi)
					consume(p, TOK_SEMICOLON, "Expected ';'");
				return assign;
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
void parse_function(Parser *p, ASTNode ***tail, char *prefix, int is_pub) {
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
	if (p->cur.type == TOK_LPAREN) {
		Lexer temp = *p->lexer;
		Token t = lexer_next(&temp);
		int depth = 1;
		int is_tuple_ret = 0;
		while (t.type != TOK_EOF) {
			if (t.type == TOK_LPAREN) depth++;
			else if (t.type == TOK_RPAREN) {
				depth--;
				if (depth == 0) {
					Token after = lexer_next(&temp);
					if (after.type == TOK_IDENTIFIER || is_ident_like(after.type)) {
						is_tuple_ret = 1;
					}
					break;
				}
			}
			t = lexer_next(&temp);
		}
		if (is_tuple_ret) {
			ret_type = parse_type(p);
		}
	} else if (is_type_token(p->cur.type)) {
		Token next = lexer_peek(p->lexer);
		if (p->cur.type == TOK_LBRACKET || next.type == TOK_STAR || next.type == TOK_AMP ||
			next.type == TOK_IDENTIFIER || is_ident_like(next.type)) {
			ret_type = parse_type(p);
		} else if (next.type == TOK_LPAREN) {
			int is_gen_struct = 0;
			for (int gi = 0; gi < p->generic_struct_count; gi++) {
				if (strcmp(p->generic_structs[gi].name, p->cur.text) == 0) {
					is_gen_struct = 1;
					break;
				}
			}
			if (is_gen_struct) {
				Lexer temp = *p->lexer;
				lexer_next(&temp); // eat '('
				int depth = 1;
				int is_gen_ret = 0;
				Token t = lexer_next(&temp);
				while (t.type != TOK_EOF) {
					if (t.type == TOK_LPAREN) depth++;
					else if (t.type == TOK_RPAREN) {
						depth--;
						if (depth == 0) {
							Token after = lexer_next(&temp);
							if (after.type == TOK_IDENTIFIER || is_ident_like(after.type)) {
								Token after2 = lexer_next(&temp);
								if (after2.type == TOK_LPAREN) {
									is_gen_ret = 1;
								}
							}
							break;
						}
					}
					t = lexer_next(&temp);
				}
				if (is_gen_ret) {
					ret_type = parse_type(p);
				}
			}
		}
	}
	char *func_name = p->cur.text;
	if (prefix) {
		size_t len = strlen(prefix) + strlen(func_name) + 4;
		char *mangled = arena_alloc(p->arena, len);
		snprintf(mangled, len, "%s__%s", prefix, func_name);
		func_name = mangled;
	}
	if (is_ident_like(p->cur.type))
		advance(p);
	else
		consume(p, TOK_IDENTIFIER, "Expected func name");
	consume(p, TOK_LPAREN, "Expected '('");

	ASTNode *args_head = NULL;
	ASTNode **args_tail = &args_head;
	while (!p->had_error && p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF) {
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
		} else if (p->cur.type == TOK_IDENTIFIER &&
				   lexer_peek(p->lexer).type == TOK_COLON) {
			arg_name = p->cur.text;
			consume(p, TOK_IDENTIFIER, "Arg name");
			consume(p, TOK_COLON, ":");
			arg_type = parse_type(p);
		} else if (p->cur.type == TOK_LPAREN) {
			arg_type = parse_type(p);
			arg_name = p->cur.text;
			consume(p, TOK_IDENTIFIER, "Arg name");
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

	consume(p, TOK_RPAREN, "Expected ')'");

	if (!ret_type) {
		if (p->cur.type == TOK_ARROW) {
			advance(p);
			ret_type = parse_type(p);
		} else if (p->cur.type == TOK_LPAREN) {
			ret_type = parse_type(p);
		} else if (is_type_token(p->cur.type) && p->cur.type != TOK_LBRACE && p->cur.type != TOK_SEMICOLON) {
			ret_type = parse_type(p);
		}
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
	fn_node->is_pub = is_pub;
	fn_node->module_name = p->cur_module;
	// Record the signature BEFORE parsing the body so recursive
	// `let x = name(...)` calls inside infer the declared return type.
	// Generic templates (return type is a bare `T`) are excluded: their
	// concrete type only exists per instantiation.
	if (p->fn_sig_count < 512 && ret_type) {
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
		if (p->decl_count < 1024) {
			p->decls[p->decl_count].name = a->data.var_decl.name;
			p->decls[p->decl_count].node = a;
			p->decl_count++;
		}
	}
	if (p->decl_count < 1024) {
		p->decls[p->decl_count].name = func_name;
		p->decls[p->decl_count].node = fn_node;
		p->decl_count++;
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

	EnumVariant *variants_head = NULL;
	EnumVariant **variants_tail = &variants_head;
	int variant_count = 0;

	Type *en_t = arena_alloc(p->arena, sizeof(Type));
	en_t->kind = TYPE_ENUM;
	en_t->name = name;

	while (!p->had_error && p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
		char *member = p->cur.text;
		consume(p, TOK_IDENTIFIER, "Expected enum member name");

		Type *payload_types[16];
		char *payload_names[16];
		int payload_count = 0;

		if (p->cur.type == TOK_LPAREN) {
			advance(p); // eat '('
			while (!p->had_error && p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF) {
				Type *pt = parse_type(p);
				char *pname = NULL;
				if (p->cur.type == TOK_IDENTIFIER) {
					pname = p->cur.text;
					advance(p);
				}
				if (payload_count < 16) {
					payload_types[payload_count] = pt;
					payload_names[payload_count] = pname;
					payload_count++;
				}
				if (p->cur.type == TOK_COMMA)
					advance(p);
				else
					break;
			}
			consume(p, TOK_RPAREN, "Expected ')' after variant payload");
		}

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

		char full_name[256];
		snprintf(full_name, sizeof(full_name), "%s_%s", name, member);

		// Record EnumVariant
		EnumVariant *ev = arena_alloc(p->arena, sizeof(EnumVariant));
		ev->name = member;
		ev->tag = (int)value;
		ev->payload_count = payload_count;
		for (int i = 0; i < payload_count; i++) {
			ev->payload_types[i] = payload_types[i];
			ev->payload_names[i] = payload_names[i];
		}
		*variants_tail = ev;
		variants_tail = &ev->next;
		variant_count++;

		// Register constructor signatures
		if (p->fn_sig_count < 512) {
			p->fn_sigs[p->fn_sig_count].name = arena_strdup(p->arena, full_name);
			p->fn_sigs[p->fn_sig_count].ret = en_t;
			p->fn_sigs[p->fn_sig_count].nparams = payload_count;
			p->fn_sig_count++;
		}
		if (p->fn_sig_count < 512) {
			p->fn_sigs[p->fn_sig_count].name = arena_strdup(p->arena, member);
			p->fn_sigs[p->fn_sig_count].ret = en_t;
			p->fn_sigs[p->fn_sig_count].nparams = payload_count;
			p->fn_sig_count++;
		}

		if (payload_count == 0) {
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

			if (p->decl_count < 256) {
				p->decls[p->decl_count].name = member_decl->data.var_decl.name;
				p->decls[p->decl_count].node = member_decl;
				p->decl_count++;
			}

			*fields_tail = member_decl;
			fields_tail = &member_decl->next;

			**outer_tail = member_decl;
			*outer_tail = &member_decl->next;
		}

		if (p->cur.type == TOK_COMMA)
			advance(p);
		else
			break;
	}
	consume(p, TOK_RBRACE, "Expected '}' after enum members");
	if (p->cur.type == TOK_SEMICOLON)
		advance(p);

	ASTNode *en = arena_alloc(p->arena, sizeof(ASTNode));
	en->type = NODE_ENUM_DECL;
	en->data.enum_decl.name = name;
	en->data.enum_decl.fields = fields_head;
	en->data.enum_decl.variants = variants_head;
	en->data.enum_decl.variant_count = variant_count;

	if (p->enum_count < 64) {
		p->enums[p->enum_count].name = name;
		p->enums[p->enum_count].variants = variants_head;
		p->enums[p->enum_count].variant_count = variant_count;
		p->enums[p->enum_count].node = en;
		p->enum_count++;
	}

	**outer_tail = en;
	*outer_tail = &en->next;
}

ASTNode *parse_program(Parser *p) {
	ASTNode *prog = arena_alloc(p->arena, sizeof(ASTNode));
	prog->type = NODE_PROGRAM;
	ASTNode **tail = &prog->next;
	p->prog_tail = &tail;

	while (!p->had_error && p->cur.type != TOK_EOF) {
		if (p->cur.type == TOK_ERROR) {
			synchronize(p);
			continue;
		}

		int is_pub = 0;
		if (p->cur.type == TOK_PUB) {
			is_pub = 1;
			advance(p);
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
				parse_function(p, &tail, NULL, is_pub);
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
			while (!p->had_error && p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF) {
				if (p->cur.type == TOK_DOT || p->cur.type == TOK_DOTDOT) {
					// `...` lexes as DOTDOT+DOT (since `..` became the
					// spread token) or, before that change, three DOTs.
					is_variadic = 1;
					while (p->cur.type == TOK_DOT ||
						   p->cur.type == TOK_DOTDOT)
						advance(p);
					break;
				}
				Type *arg_type = NULL;
				char *arg_name = NULL;
				if (p->cur.type == TOK_IDENTIFIER &&
					lexer_peek(p->lexer).type == TOK_COLON) {
					// Kawa-style: `x: f64`.
					arg_name = p->cur.text;
					consume(p, TOK_IDENTIFIER, "Arg name");
					consume(p, TOK_COLON, ":");
					arg_type = parse_type(p);
				} else if (is_type_token(p->cur.type)) {
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
			if (!ret_type) {
				if (p->cur.type == TOK_ARROW) {
					advance(p);
					ret_type = parse_type(p);
				} else if (is_type_token(p->cur.type) && p->cur.type != TOK_SEMICOLON) {
					ret_type = parse_type(p);
				}
			}
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
			if (p->fn_sig_count < 512 && ret_type) {
				int np2 = 0;
				for (ASTNode *a2 = args_head; a2; a2 = a2->next)
					np2++;
				p->fn_sigs[p->fn_sig_count].name = fn_name;
				p->fn_sigs[p->fn_sig_count].ret = ret_type;
				p->fn_sigs[p->fn_sig_count].nparams = np2;
				p->fn_sig_count++;
			}
		} else if (p->cur.type == TOK_STRUCT) {
parse_soa_struct:;
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

			char *type_param = NULL;
			char *type_params[8];
			int type_param_count = 0;
			if (p->cur.type == TOK_LPAREN) {
				advance(p);
				while (!p->had_error && p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF) {
					char *tp = p->cur.text;
					consume(p, TOK_IDENTIFIER, "Expected type parameter");
					if (type_param_count < 8) {
						type_params[type_param_count++] = tp;
					}
					if (p->cur.type == TOK_COMMA)
						advance(p);
					else
						break;
				}
				consume(p, TOK_RPAREN, "Expected ')' after type parameter(s)");
				if (type_param_count > 0)
					type_param = type_params[0];
			}
			st->data.struct_decl.type_param = type_param;
			st->data.struct_decl.type_param_count = type_param_count;
			for (int tpi = 0; tpi < type_param_count; tpi++)
				st->data.struct_decl.type_params[tpi] = type_params[tpi];
			st->is_pub = is_pub;
			st->module_name = p->cur_module;

			if (type_param_count == 0 && p->struct_name_count < 128) {
				p->struct_names[p->struct_name_count] =
					st->data.struct_decl.name;
				p->struct_nodes[p->struct_name_count] = st;
				p->struct_name_count++;
			}
			consume(p, TOK_LBRACE, "{");

			ASTNode *fields_head = NULL;
			ASTNode **fields_tail = &fields_head;

			while (!p->had_error && p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
				int f_pub = 0;
				if (p->cur.type == TOK_PUB) {
					f_pub = 1;
					advance(p);
				}
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
					emb->is_pub = f_pub;
					emb->module_name = p->cur_module;
					*fields_tail = emb;
					fields_tail = &emb->next;
					continue;
				}
				char *f_name = p->cur.text;
				if (is_ident_like(p->cur.type))
					advance(p);
				else
					consume(p, TOK_IDENTIFIER, "Field name");
				// Field default value (IDEAS 1.3): `f32 zoom = 1.0;`.
				// The expression must fold at compile time; struct
				// literals that omit the field use it as the seed.
				ASTNode *f_default = NULL;
				if (p->cur.type == TOK_ASSIGN) {
					advance(p);
					f_default = parse_expr(p);
				}
				consume(p, TOK_SEMICOLON, ";");

				ASTNode *field = arena_alloc(p->arena, sizeof(ASTNode));
				field->type = NODE_VAR_DECL;
				field->data.var_decl.name = f_name;
				field->data_type = f_type;
				field->data.var_decl.field_default = f_default;
				field->is_pub = f_pub;
				field->module_name = p->cur_module;
				*fields_tail = field;
				fields_tail = &field->next;
			}
			consume(p, TOK_RBRACE, "}");
			st->data.struct_decl.fields = fields_head;
			st->data.struct_decl.is_soa = soa_attr;
			if (soa_attr && p->soa_count < 64)
				p->soa_structs[p->soa_count++] = st->data.struct_decl.name;

			if (type_param_count > 0) {
				if (p->generic_struct_count < 32) {
					p->generic_structs[p->generic_struct_count].name =
						st->data.struct_decl.name;
					p->generic_structs[p->generic_struct_count].type_param =
						type_param;
					p->generic_structs[p->generic_struct_count].type_param_count =
						type_param_count;
					for (int tpi = 0; tpi < type_param_count; tpi++)
						p->generic_structs[p->generic_struct_count].type_params[tpi] =
							type_params[tpi];
					p->generic_structs[p->generic_struct_count].node = st;
					p->generic_struct_count++;
				}
				continue;
			}

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
			if (s) {
				s->is_pub = is_pub;
				s->module_name = p->cur_module;
			}
			*tail = s;
			while (*tail)
				tail = &(*tail)->next;
		} else if (p->cur.type == TOK_IMPL) {
			advance(p);
			char *type_params[8];
			int type_param_count = 0;
			if (p->cur.type == TOK_LPAREN) {
				advance(p);
				while (!p->had_error && p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF) {
					char *tp = p->cur.text;
					consume(p, TOK_IDENTIFIER, "Expected type parameter");
					if (type_param_count < 8) {
						type_params[type_param_count++] = tp;
					}
					if (p->cur.type == TOK_COMMA)
						advance(p);
					else
						break;
				}
				consume(p, TOK_RPAREN, "Expected ')' after type parameter(s)");
			}
			char *sname = p->cur.text;
			consume(p, TOK_IDENTIFIER, "Impl struct name");
			if (p->cur.type == TOK_LPAREN) {
				advance(p);
				int count2 = 0;
				char *params2[8];
				while (!p->had_error && p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF) {
					char *tp = p->cur.text;
					consume(p, TOK_IDENTIFIER, "Expected type parameter");
					if (count2 < 8)
						params2[count2++] = tp;
					if (p->cur.type == TOK_COMMA)
						advance(p);
					else
						break;
				}
				consume(p, TOK_RPAREN, "Expected ')' after type parameter(s)");
				if (type_param_count == 0) {
					type_param_count = count2;
					for (int i = 0; i < count2; i++)
						type_params[i] = params2[i];
				}
			}
			char *type_param = (type_param_count > 0) ? type_params[0] : NULL;
			consume(p, TOK_LBRACE, "{");

			ASTNode *methods_head = NULL;
			ASTNode **methods_tail = &methods_head;

			while (!p->had_error && p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
				int item_pub = 0;
				if (p->cur.type == TOK_PUB) {
					item_pub = 1;
					advance(p);
				}
				if (p->cur.type == TOK_CONST) {
					parse_const_decl(p, &methods_tail, sname, item_pub);
				} else {
					parse_function(p, &methods_tail, sname, item_pub);
				}
			}
			consume(p, TOK_RBRACE, "}");

			ASTNode *st = arena_alloc(p->arena, sizeof(ASTNode));
			st->type = NODE_IMPL_BLOCK;
			st->data.impl.struct_name = sname;
			st->data.impl.type_param = type_param;
			st->data.impl.type_param_count = type_param_count;
			for (int i = 0; i < type_param_count; i++)
				st->data.impl.type_params[i] = type_params[i];
			st->data.impl.methods = methods_head;
			st->is_pub = is_pub;
			st->module_name = p->cur_module;

			if (type_param_count > 0) {
				if (p->generic_impl_count < 32) {
					p->generic_impls[p->generic_impl_count].struct_name = sname;
					p->generic_impls[p->generic_impl_count].type_param = type_param;
					p->generic_impls[p->generic_impl_count].type_param_count = type_param_count;
					for (int i = 0; i < type_param_count; i++)
						p->generic_impls[p->generic_impl_count].type_params[i] = type_params[i];
					p->generic_impls[p->generic_impl_count].node = st;
					p->generic_impl_count++;
				}
				for (int gi = 0; gi < p->generic_struct_count; gi++) {
					if (strcmp(p->generic_structs[gi].name, sname) == 0) {
						int g_num_params = p->generic_structs[gi].type_param_count > 0 ? p->generic_structs[gi].type_param_count : 1;
						for (int inst_i = 0; inst_i < p->generic_structs[gi].inst_count; inst_i++) {
							Type *conc_multi[8];
							for (int pi = 0; pi < g_num_params; pi++) {
								conc_multi[pi] = p->generic_structs[gi].instantiations_multi[inst_i][pi];
								if (!conc_multi[pi] && pi == 0)
									conc_multi[pi] = p->generic_structs[gi].instantiations[inst_i];
							}
							char inst_sname[256];
							if (g_num_params > 1) {
								char sfx_buf[256];
								sfx_buf[0] = '\0';
								for (int pi = 0; pi < g_num_params; pi++) {
									const char *s = type_to_suffix(p->arena, conc_multi[pi]);
									if (pi > 0) strcat(sfx_buf, "_");
									strcat(sfx_buf, s);
								}
								snprintf(inst_sname, sizeof(inst_sname), "%s__%s", sname, sfx_buf);
							} else {
								const char *sfx = type_to_suffix(p->arena, conc_multi[0]);
								snprintf(inst_sname, sizeof(inst_sname), "%s__%s", sname, sfx);
							}

							ASTNode *inst_methods = NULL;
							ASTNode **m_tail = &inst_methods;
							for (ASTNode *m = methods_head; m; m = m->next) {
								ASTNode *cm = clone_and_subst_node(p, m, type_param_count, type_params, conc_multi, sname, inst_sname);
								*m_tail = cm;
								m_tail = &cm->next;
								if (cm->type == NODE_FUNC_DECL) {
									if (p->fn_sig_count < 512 && cm->data.func.ret_type) {
										int np = 0;
										for (ASTNode *a = cm->data.func.args; a; a = a->next)
											np++;
										p->fn_sigs[p->fn_sig_count].name = cm->data.func.name;
										p->fn_sigs[p->fn_sig_count].ret = cm->data.func.ret_type;
										p->fn_sigs[p->fn_sig_count].nparams = np;
										p->fn_sig_count++;
									}
									if (p->decl_count < 1024) {
										p->decls[p->decl_count].name = cm->data.func.name;
										p->decls[p->decl_count].node = cm;
										p->decl_count++;
									}
								}
							}
							ASTNode *ib = arena_alloc(p->arena, sizeof(ASTNode));
							ib->type = NODE_IMPL_BLOCK;
							ib->data.impl.struct_name = arena_strdup(p->arena, inst_sname);
							ib->data.impl.methods = inst_methods;
							ib->is_pub = is_pub;
							ib->module_name = p->cur_module;
							if (p->prog_tail && *p->prog_tail) {
								**p->prog_tail = ib;
								*p->prog_tail = &ib->next;
							}
						}
						break;
					}
				}
				continue;
			}

			*tail = st;
			tail = &st->next;
		} else {
			int is_global_decl = 0;
			if (p->cur.type == TOK_CONST || p->cur.type == TOK_ORBIT ||
				p->cur.type == TOK_LET) {
				// let/const/orbit are always declarations at file scope.
				is_global_decl = 1;
			} else if (p->cur.type == TOK_LPAREN) {
				Lexer temp = *p->lexer;
				Token t = lexer_next(&temp);
				int depth = 1;
				int looks_like_tuple = 0;
				while (t.type != TOK_EOF) {
					if (t.type == TOK_LPAREN) depth++;
					else if (t.type == TOK_RPAREN) {
						depth--;
						if (depth == 0) {
							Token after = lexer_next(&temp);
							if ((after.type == TOK_IDENTIFIER || after.type == TOK_STAR) && looks_like_tuple) {
								is_global_decl = 1;
							}
							break;
						}
					} else if (t.type == TOK_COMMA) {
						looks_like_tuple = 1;
					}
					t = lexer_next(&temp);
				}
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
				if (s) {
					s->is_pub = is_pub;
					s->module_name = p->cur_module;
				}
				*tail = s;
				while (*tail)
					tail = &(*tail)->next;
			} else {
				report_error(p, "Unexpected top-level token: %s", p->cur.text);
				advance(p);
			}
		}
	}
	return prog;
}
