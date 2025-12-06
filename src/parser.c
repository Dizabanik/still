#include "ast.h"
#include "timbr.h"
#include <lexer.h>
#include <parser.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
	timbr_diagnostic(TIMBR_ERROR, "Parser Error", p->lexer->filename, lT,
					 p->cur.line, p->cur.posA, p->cur.len, buffer);
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
		case TOK_STRUCT:
		case TOK_IMPL:
		case TOK_IF:
		case TOK_WHILE:
		case TOK_RETURN:
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
	for (int i = 0; i < p->decl_count; i++) {
		if (strcmp(p->decls[i].name, name) == 0)
			return p->decls[i].node;
	}
	return NULL;
}

static int is_type_token(TokenType t) {
	return (t >= TOK_VOID && t <= TOK_F64) || t == TOK_IDENTIFIER;
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
		// If followed by ')' or '*', it's likely a type (Cast)
		// If followed by '+', '-', etc., it's a variable (Grouping)
		if (t2.type == TOK_RPAREN || t2.type == TOK_STAR) {
			return 1;
		}
	}
	return 0;
}

static Type *parse_type(Parser *p) {
	Type *t = arena_alloc(p->arena, sizeof(Type));
	TokenType tok = p->cur.type;

	if (tok == TOK_U32)
		t->kind = TYPE_U32;
	else if (tok == TOK_I32)
		t->kind = TYPE_I32;
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
	else if (tok == TOK_IDENTIFIER) {
		t->kind = TYPE_STRUCT;
		t->name = p->cur.text;
	} else {
		report_error(p, "Expected type");
		return t;
	}
	advance(p);
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
// 2. Implement parse_unary
static ASTNode *parse_unary(Parser *p) {
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
	}
	return parse_postfix(p); // Fall through to postfix/primary
}
static ASTNode *parse_expr(Parser *p);
static ASTNode *parse_statement(Parser *p);
static ASTNode *parse_block(Parser *p);
static ASTNode *parse_grind(Parser *p);

static ASTNode *parse_struct_literal(Parser *p);
static int peek_is_struct_literal(Parser *p);

// Returns 1 if the upcoming '{ ... }' looks like a struct literal.
// Returns 0 if it looks like a block code.
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

static ASTNode *parse_primary(Parser *p) {
	ASTNode *n = arena_alloc(p->arena, sizeof(ASTNode));
	if (p->cur.type == TOK_INT_LIT) {
		n->type = NODE_LITERAL;
		n->data_type = arena_alloc(p->arena, sizeof(Type));
		n->data_type->kind = TYPE_U32;
		n->data.literal.i_val = atoi(p->cur.text);
		advance(p);
	} else if (p->cur.type == TOK_FLOAT_LIT) {
		n->type = NODE_LITERAL;
		n->data_type = arena_alloc(p->arena, sizeof(Type));
		n->data_type->kind = TYPE_F64; // FIX: Default to F64
		n->data.literal.f_val = strtod(p->cur.text, NULL); // FIX: use strtod
		advance(p);
	} else if (p->cur.type == TOK_STRING_LIT) {
		n->type = NODE_STRING_LIT;
		n->data.str_lit.s_val = p->cur.text;
		advance(p);
	} else if (p->cur.type == TOK_TRUE) {
		n->type = NODE_LITERAL;
		n->data.literal.i_val = 1;
		advance(p);
	} else if (p->cur.type == TOK_FALSE) {
		n->type = NODE_LITERAL;
		n->data.literal.i_val = 0;
		advance(p);
	} else if (p->cur.type == TOK_SIZEOF) {
		advance(p);
		consume(p, TOK_LPAREN, "(");
		int depth = 1;
		while (depth > 0 && p->cur.type != TOK_EOF) {
			if (p->cur.type == TOK_LPAREN)
				depth++;
			if (p->cur.type == TOK_RPAREN)
				depth--;
			if (depth > 0)
				advance(p);
		}
		if (p->cur.type == TOK_RPAREN)
			advance(p);
		n->type = NODE_LITERAL;
		n->data.literal.i_val = 4;
	} else if (p->cur.type == TOK_GRIND) {
		advance(p);
		return parse_grind(p);
	} else if (p->cur.type == TOK_LBRACE) {
		if (peek_is_struct_literal(p)) {
			return parse_struct_literal(p);
		} else {
			return parse_block(p);
		}
	} else if (p->cur.type == TOK_IDENTIFIER) {
		n->type = NODE_VAR_REF;
		n->data.var_ref.name = p->cur.text;
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
		advance(p);
	}
	return n;
}

static ASTNode *parse_postfix(Parser *p) {
	ASTNode *expr = parse_primary(p);
	while (1) {
		if (p->cur.type == TOK_DOT) {
			advance(p);
			ASTNode *member = arena_alloc(p->arena, sizeof(ASTNode));
			member->type = NODE_MEMBER_ACCESS;
			member->data.member_access.object = expr;
			member->data.member_access.member = p->cur.text;
			consume(p, TOK_IDENTIFIER, "Expected member name");
			expr = member;
		} else if (p->cur.type == TOK_LPAREN) {
			advance(p);

			// --- NEW: Detect Method Call and Transform ---
			int is_method_call = 0;
			ASTNode *self_obj = NULL;
			char *struct_name = NULL;
			char *method_name = NULL;

			if (expr->type == NODE_MEMBER_ACCESS) {
				self_obj = expr->data.member_access.object;
				method_name = expr->data.member_access.member;

				// Lookup the type of the object (e.g., look up 'k' to find
				// 'User')
				if (self_obj->type == NODE_VAR_REF) {
					ASTNode *decl = find_decl(p, self_obj->data.var_ref.name);
					if (decl && decl->data_type &&
						decl->data_type->kind == TYPE_STRUCT) {
						struct_name = decl->data_type->name;
						is_method_call = 1;
					}
				}
			}
			// ---------------------------------------------

			ASTNode *call = arena_alloc(p->arena, sizeof(ASTNode));
			call->type = NODE_CALL;

			// Parse arguments normally
			ASTNode *head = NULL;
			ASTNode **tail = &head;
			while (p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF) {
				*tail = parse_expr(p);
				if (*tail) {
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
				// Note: In a real compiler, allocate this string in the arena
				int len = strlen(struct_name) + strlen(method_name) + 2;
				char *mangled = malloc(len);
				sprintf(mangled, "%s__%s", struct_name, method_name);

				// 2. Change Callee to a simple VAR_REF (Function Name)
				ASTNode *new_callee = arena_alloc(p->arena, sizeof(ASTNode));
				new_callee->type = NODE_VAR_REF;
				new_callee->data.var_ref.name = mangled;
				call->data.call.callee = new_callee;

				// 3. Inject 'self' (k) as the first argument
				// We must create a COPY of the object node or reuse it safely
				// to avoid double-free or AST cycle issues, though reusing ptr
				// is usually fine here.
				ASTNode *self_arg = self_obj;
				self_arg->next = head;			 // Link old args after self
				call->data.call.args = self_arg; // Self is now head
			} else {
				// Standard function call behavior
				call->data.call.callee = expr;
				call->data.call.args = head;
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
		if (p->cur.type == TOK_PLUS || p->cur.type == TOK_MINUS)
			tok_prec = 10;
		if (p->cur.type == TOK_STAR || p->cur.type == TOK_SLASH)
			tok_prec = 20;
		if (p->cur.type == TOK_LANGLE || p->cur.type == TOK_RANGLE ||
			p->cur.type == TOK_ISEQ || p->cur.type == TOK_LEQ ||
			p->cur.type == TOK_REQ)
			tok_prec = 5;
		if (p->cur.type == TOK_TILDE_EQ)
			tok_prec = 2;
		if (tok_prec < expr_prec)
			return lhs;
		int op = p->cur.type;
		advance(p);
		ASTNode *rhs = parse_postfix(p);
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
		int l = val->data.bin_op.left->data.literal.i_val;
		int r = val->data.bin_op.right->data.literal.i_val;
		int res = 0;
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
		folded->data.literal.i_val = res;
		folded->data_type = val->data_type;
		return folded;
	}
	return val;
}

static ASTNode *parse_statement(Parser *p) {
	if (p->cur.type == TOK_ERROR) {
		synchronize(p);
		return NULL;
	}

	int is_c_style_decl = 0;
	if (is_type_token(p->cur.type)) {
		Token next = lexer_peek(p->lexer);
		if (next.type == TOK_IDENTIFIER || next.type == TOK_STAR)
			is_c_style_decl = 1;
	}

	if (p->cur.type == TOK_LET || p->cur.type == TOK_CONST ||
		p->cur.type == TOK_ORBIT || is_c_style_decl) {
		int is_orbit = (p->cur.type == TOK_ORBIT);
		int is_const = (p->cur.type == TOK_CONST);

		Type *type = NULL;
		if (is_c_style_decl) {
			type = parse_type(p);
		} else {
			advance(p);
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
				init = parse_expr(p);
			}
		}

		// FIX: Type Inference
		if (!type) {
			if (init && init->data_type) {
				type = init->data_type; // Infer from expression
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
		ret->data.ret_stmt.expr = parse_expr(p);
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
	if (p->cur.type == TOK_FILTER) {
		advance(p);
		ASTNode *filt = arena_alloc(p->arena, sizeof(ASTNode));
		filt->type = NODE_FILTER;
		filt->data.filter.try_block = parse_block(p);
		consume(p, TOK_DREGS, "Expected 'dregs'");
		consume(p, TOK_LPAREN, "(");
		filt->data.filter.err_var = p->cur.text;
		consume(p, TOK_IDENTIFIER, "Err var");
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
	if (p->cur.type == TOK_ASSIGN) {
		advance(p); // Eat '='

		ASTNode *assign = arena_alloc(p->arena, sizeof(ASTNode));
		assign->type = NODE_ASSIGN;

		// Validate LHS is an L-Value
		if (expr->type == NODE_VAR_REF || expr->type == NODE_MEMBER_ACCESS) {
			assign->data.assign.target = expr;
		} else {
			report_error(
				p, "Invalid assignment target. Must be variable or field.");
		}

		// Special Case: Variable Decl with implicit struct literal
		// User a = { ... };
		// (Note: This specific path is for re-assignment.
		// Decl parsing happens in the LET block above, which you should also
		// update to use parse_expr() for the init value, which calls
		// parse_primary, which now handles struct literals automatically thanks
		// to peek_is_struct_literal).

		assign->data.assign.value = parse_expr(p);
		consume(p, TOK_SEMICOLON, "Expected ';'");
		return assign;
	}
	consume(p, TOK_SEMICOLON, "Expected ';'");
	return expr;
}
void parse_function(Parser *p, ASTNode ***tail, char *prefix) {
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
		int len = strlen(prefix) + strlen(func_name) + 3;
		char *mangled = malloc(len);
		sprintf(mangled, "%s__%s", prefix, func_name);
		func_name = mangled;
	}
	consume(p, TOK_IDENTIFIER, "Expected func name");
	consume(p, TOK_LPAREN, "Expected '('");

	ASTNode *args_head = NULL;
	ASTNode **args_tail = &args_head;
	while (p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF) {
		Type *arg_type = NULL;
		char *arg_name = NULL;

		if (is_type_token(p->cur.type)) {
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
	fn_node->data.func.is_pure = is_pure;
	fn_node->data.func.is_drip = is_drip;
	fn_node->data.func.name = func_name;
	fn_node->data.func.ret_type = ret_type;
	fn_node->data.func.args = args_head;

	consume(p, TOK_RPAREN, "Expected ')'");
	fn_node->data.func.body = parse_block(p);
	**tail = fn_node;
	*tail = &fn_node->next;
	if (prefix && func_name)
		free(func_name);
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

		if (p->cur.type == TOK_FN || p->cur.type == TOK_PURE) {
			parse_function(p, &tail, NULL);
		} else if (p->cur.type == TOK_STRUCT) {
			advance(p);
			ASTNode *st = arena_alloc(p->arena, sizeof(ASTNode));
			st->type = NODE_STRUCT_DECL;
			st->data.struct_decl.name = p->cur.text;
			consume(p, TOK_IDENTIFIER, "Struct name");
			consume(p, TOK_LBRACE, "{");

			ASTNode *fields_head = NULL;
			ASTNode **fields_tail = &fields_head;

			while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
				Type *f_type = parse_type(p);
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

			*tail = st;
			tail = &st->next;
		} else if (p->cur.type == TOK_IMPORT) {
			advance(p);
			ASTNode *imp = arena_alloc(p->arena, sizeof(ASTNode));
			imp->type = NODE_IMPORT;
			imp->data.import.lib_name = p->cur.text;
			consume(p, TOK_IDENTIFIER, "Lib name");
			consume(p, TOK_SEMICOLON, ";");
			*tail = imp;
			tail = &imp->next;
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
			if (is_type_token(p->cur.type)) {
				Token next = lexer_peek(p->lexer);
				if (next.type == TOK_IDENTIFIER || next.type == TOK_STAR)
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
