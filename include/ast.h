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
	TYPE_F16,
	TYPE_BF16,
	TYPE_F32,
	TYPE_F64,
	TYPE_SET,
	TYPE_ARRAY,
	TYPE_HANDLE,
	TYPE_STRUCT,
	TYPE_ALIAS,
	TYPE_PTR,
	TYPE_AMP,
	TYPE_SLICE,
	TYPE_CHAN,
	TYPE_ENUM,
	TYPE_OWNER,
	TYPE_REF,
	TYPE_ARENA
} TypeKind;

typedef struct Type {
	TypeKind kind;
	int is_signed; // 1 for signed integer/float (i8/16/32/64, f32/f64),
				   // 0 for unsigned integer (u8/16/32/64), bool, char, void,
				   // and any other kind. Conservatively defaults to 0.
	struct Type *inner; // For set<T>, T* or [N]T; the element type of []T
	char *name;			// For struct/alias/enum names
	long array_len;		// For [N]T fixed-size arrays
} Type;

typedef struct EnumVariant {
	char *name;
	int tag;
	Type *payload_types[16];
	char *payload_names[16];
	int payload_count;
	struct EnumVariant *next;
} EnumVariant;

typedef struct StructInitItem {
	char *field_name; // NULL if positional
	struct ASTNode *value;
	// `..base` spread: copy remaining fields from this expression. The
	// item carrying it is ignored as a value source.
	struct ASTNode *spread_from;
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
	NODE_EXTERN_FN,
	NODE_ASM,
	NODE_UNCHECKED_BLOCK,
	NODE_STABLE,
	NODE_SEND,
	NODE_RECV,
	NODE_SELECT,
	NODE_SIZEOF,
	NODE_CAST,
	NODE_IMPORT,
	NODE_MATCH,
	NODE_MATCH_ARM,
	NODE_RANGE,
	NODE_SLICE_INDEX,

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
	int column, span_length;
			  // (diagnostics: traps, runtime errors)
	// Call-site named-argument label (`f(x: 1)`). Lives OUTSIDE the data
	// union on purpose: var_decl.name aliases literal.i_val/str_lit.s_val,
	// so reusing it would corrupt the argument expression it labels.
	char *arg_label;
	int has_arg_label;
	int is_pub;
	const char *module_name;

	union {
		struct { ASTNode *reference, *body, *otherwise; int optional; } stable;
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
			int is_noalloc;
			int is_nocapture;
			unsigned fp_permissions; // 1 contraction, 2 reassociation, 4 finite-only
		} func;
		struct {
			char *name;
			ASTNode *fields;
			int is_soa; // #[soa]: fields stored as parallel arrays
			char *type_param; // e.g. "T" for generic struct Box(T) (points to type_params[0])
			char *type_params[8];
			int type_param_count;
		} struct_decl;
		struct {
			char *name;
			ASTNode *fields; // NODE_VAR_DECL chain: one i32 const per member
			EnumVariant *variants;
			int variant_count;
		} enum_decl;
		struct {
			struct ASTNode *val;
		} cast;
		struct {
			char *struct_name;
			ASTNode *methods;
			char *type_param; // e.g. "T" for generic impl Box(T) (points to type_params[0])
			char *type_params[8];
			int type_param_count;
		} impl;
		struct {
			char *name;
			ASTNode *init;
			int is_orbit;
			int is_const;
			// Struct FIELD default value (`f32 zoom = 1.0;` in a struct
			// body). NULL for ordinary locals; only fields carry one.
			ASTNode *field_default;
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
			int unary_negation;
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
			int i_val;      // legacy small value; i64_val is authoritative
			long long i64_val;
			double f_val;
		} literal;
		struct {
			char *s_val;
			size_t len;
			size_t *source_offsets;
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
			ASTNode *captures; // chain of NODE_VAR_DECL for captured variables
		} defer;
		struct {
			ASTNode *try_block;
			char *err_var;
			ASTNode *catch_block;
			Type *err_type; // payload type (`dregs (e: ParseErr)`); NULL = i32
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
			ASTNode *chan;
			ASTNode *value;
		} send;
		struct {
			ASTNode *chan;
		} recv;
		struct {
			struct SelectCase {
				ASTNode *var_decl; // NODE_VAR_DECL binding the received value (may be NULL)
				ASTNode *chan;     // channel to receive from
				ASTNode *body;
				struct SelectCase *next;
			} *cases;   // first ready wins; round-robin poll
			int has_default;
			ASTNode *default_body;
		} select_stmt;
		struct {
			struct ASTNode *expr;
		} deref;
		struct {
			char *lib_name;
		} import;
		struct {
			char *name;       // extern fn name (the C symbol)
			Type *ret_type;   // declared return type
			ASTNode *args;    // NODE_VAR_DECL chain: name + data_type
			int is_variadic;
		} extern_fn;
		struct {
			char *asm_template;   // instruction string(s)
			char *constraints;    // LLVM inline asm constraints, may be NULL
			ASTNode *outputs;     // unused v1 (r constraint form later)
		} asm_block;
		struct {
			ASTNode *expr;
		} ret_stmt;
		struct {
			ASTNode *left;
			ASTNode *right;
			int is_inclusive; // 1 for ..=, 0 for ..
		} range;
		struct {
			ASTNode *target;
			ASTNode *arms; // NODE_MATCH_ARM chain
		} match_stmt;
		struct {
			char *enum_name;
			char *variant_name;
			ASTNode *bindings;
			int is_else;
			ASTNode *body;
		} match_arm;
		struct {
			ASTNode *object;
			ASTNode *start;
			ASTNode *end;
			int is_inclusive;
		} slice_index;
	} data;

	ASTNode *next;
};

/* Clone an AST specialization without mutating its template. Linked child
 * lists and dependency edges preserve shared node identity. Strings are
 * immutable arena bytes; types and mutable nodes belong to the new instance. */
ASTNode *kawa_clone_ast(Arena *, ASTNode *, int parameter_count, char **parameters,
                        Type **concretes, const char *generic_struct, const char *instance_struct);
#endif
