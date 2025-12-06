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
	TYPE_HANDLE,
	TYPE_STRUCT,
	TYPE_ALIAS,
	TYPE_PTR,
	TYPE_AMP
} TypeKind;

typedef struct Type {
	TypeKind kind;
	struct Type *inner; // For set<T> or T*
	char *name;			// For struct/alias names
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
	NODE_IMPL_BLOCK,
	NODE_BLOCK,
	NODE_VAR_DECL,
	NODE_CONST_DECL,
	NODE_ASSIGN,
	NODE_RETURN,
	NODE_BINARY_OP,
	NODE_LITERAL,
	NODE_STRING_LIT,
	NODE_VAR_REF,
	NODE_MEMBER_ACCESS,
	NODE_CALL,
	NODE_SET_LITERAL,
	NODE_SET_POUR,
	NODE_BREW,
	NODE_SIP,
	NODE_DRIP,
	NODE_DROP,
	NODE_IF,
	NODE_WHILE,
	NODE_BATCH,
	NODE_DEFER,
	NODE_FILTER,
	NODE_PRESS,
	NODE_ALIAS,
	NODE_CAST,
	NODE_IMPORT,
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
		} func;
		struct {
			char *name;
			ASTNode *fields;
		} struct_decl;
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
