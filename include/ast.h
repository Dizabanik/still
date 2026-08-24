#ifndef KAWA_AST_H
#define KAWA_AST_H

#include "arena.h"

// --- TYPES ---
typedef enum {
	TYPE_VOID,
	TYPE_BOOL,
	TYPE_CHAR,
	TYPE_I8,
	TYPE_I16,
	TYPE_I32,
	TYPE_I64,
	TYPE_U8,
	TYPE_U16,
	TYPE_U32,
	TYPE_U64,
	TYPE_F32,
	TYPE_F64,
	TYPE_SET,
	TYPE_ARRAY,
	TYPE_HANDLE,
	TYPE_STRUCT,
	TYPE_ALIAS,
	TYPE_PTR,
	TYPE_AMP
} TypeKind;

typedef struct Type {
	TypeKind kind;
	int is_signed; // 1 for signed integer/float (i8/16/32/64, f32/f64),
				   // 0 for unsigned integer (u8/16/32/64), bool, char, void,
				   // and any other kind. Conservatively defaults to 0.
	struct Type *inner; // For set<T>, T* or [N]T
	char *name;			// For struct/alias names
	long array_len;		// For [N]T fixed-size arrays
} Type;

typedef struct StructInitItem {
	char *field_name; // NULL if positional
	struct ASTNode *value;
	struct StructInitItem *next;
} StructInitItem;

// --- NODES ---
typedef enum {
	NODE_PROGRAM,
	NODE_FUNC_DECL,
	NODE_STRUCT_DECL,
	NODE_ENUM_DECL,
	NODE_IMPL_BLOCK,
	NODE_BLOCK,
	NODE_VAR_DECL,
	NODE_CONST_DECL,
	NODE_ASSIGN,
	NODE_RETURN,
	NODE_BINARY_OP,
	NODE_TERNARY,
	NODE_LITERAL,
	NODE_STRING_LIT,
	NODE_VAR_REF,
	NODE_MEMBER_ACCESS,
	NODE_INDEX,
	NODE_CALL,
	NODE_SET_LITERAL,
	NODE_SET_POUR,
	NODE_BREW,
	NODE_SIP,
	NODE_DRIP,
	NODE_DROP,
	NODE_IF,
	NODE_WHILE,
	NODE_FOR,
	NODE_BREAK,
	NODE_CONTINUE,
	NODE_SWITCH,
	NODE_CASE,
	NODE_BATCH,
	NODE_DEFER,
	NODE_FILTER,
	NODE_PRESS,
	NODE_ALIAS,
	NODE_SIZEOF,
	NODE_CAST,
	NODE_IMPORT,

	// --- Internal / Lowering ---
	NODE_STRUCT_LITERAL,
	NODE_DEREF,
	NODE_AMP
} NodeType;

typedef struct ASTNode ASTNode;

typedef struct Dependency {
	struct ASTNode *dependent_node;
	struct ASTNode *logic_expr;
	struct Dependency *next;
} Dependency;

struct ASTNode {
	NodeType type;
	Type *data_type;
	Dependency *dependents;
	int line; // source line, stamped by the parser where it matters
			  // (diagnostics: traps, runtime errors)
	// Call-site named-argument label (`f(x: 1)`). Lives OUTSIDE the data
	// union on purpose: var_decl.name aliases literal.i_val/str_lit.s_val,
	// so reusing it would corrupt the argument expression it labels.
	char *arg_label;
	int has_arg_label;

	union {
		struct {
			ASTNode *stmts;
		} block;
		struct {
			char *name;
			ASTNode *args;
			ASTNode *body;
			Type *ret_type;
			int is_pure;
			int is_drip;
			int is_test;   // #[test] attribute
			int is_ignored; // #[ignore]
		} func;
		struct {
			char *name;
			ASTNode *fields;
		} struct_decl;
		struct {
			char *name;
			ASTNode *fields; // NODE_VAR_DECL chain: one i32 const per member
		} enum_decl;
		struct {
			struct ASTNode *val;
		} cast;
		struct {
			char *struct_name;
			ASTNode *methods;
		} impl;
		struct {
			char *name;
			ASTNode *init;
			int is_orbit;
			int is_const;
		} var_decl;
		struct {
			struct ASTNode *target;
			struct ASTNode *value;
		} assign;
		struct {
			StructInitItem *items;
		} struct_lit;
		struct {
			int op;
			ASTNode *left, *right;
		} bin_op;
		struct {
			ASTNode *cond, *then_expr, *else_expr;
		} ternary;
		struct {
			struct ASTNode *value; // Optional expr
			struct Type *type_val; // Optional type
		} size_of;
		struct {
			int i_val;
			double f_val;
		} literal;
		struct {
			char *s_val;
		} str_lit;
		struct {
			char *name;
		} var_ref;
		struct {
			ASTNode *object;
			char *member;
		} member_access;
		struct {
			ASTNode *object;
			ASTNode *index;
		} index;
		struct {
			ASTNode *callee;
			ASTNode *args;
		} call;
		struct {
			ASTNode *items;
		} set_lit;
		struct {
			ASTNode *target;
			ASTNode *value;
		} set_pour;
		struct {
			ASTNode *body;
		} brew;
		struct {
			ASTNode *handle;
		} sip;
		struct {
			ASTNode *val;
		} drop;
		struct {
			ASTNode *cond, *then_block, *else_block;
		} if_stmt;
		struct {
			ASTNode *cond, *body;
		} while_stmt;
		struct {
			ASTNode *init;   // may be NULL
			ASTNode *cond;   // may be NULL = true
			ASTNode *step;   // may be NULL; expression statement (e.g. `i += 1`)
			ASTNode *body;
		} for_stmt;
		struct {
			ASTNode *value;      // switched-on expression
			ASTNode *cases;      // NODE_CASE chain, in source order
		} switch_stmt;
		struct {
			ASTNode *expr;       // case value (const); NULL for default
			ASTNode *body;       // stmts run when matched (fallthrough = C)
		} case_stmt;
		struct {
			char *iterator_var;
			ASTNode *collection;
			ASTNode *body;
		} batch;
		struct {
			ASTNode *stmt;
		} defer;
		struct {
			ASTNode *try_block;
			char *err_var;
			ASTNode *catch_block;
		} filter;
		struct {
			char *name;
			ASTNode *target;
		} press;
		struct {
			char *name;
			Type *target_type;
		} alias;
		struct {
			struct ASTNode *expr;
		} deref;
		struct {
			char *lib_name;
		} import;
		struct {
			ASTNode *expr;
		} ret_stmt;
	} data;

	ASTNode *next;
};

#endif
