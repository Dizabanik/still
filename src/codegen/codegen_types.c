#include "codegen_internal.h"

// --- Type Registries ---
// Heads of singly-linked lists; both live on the KawaCompiler struct so each
// compiler owns its own. Nodes are arena-allocated.

void register_alias(KawaCompiler *c, const char *name, Type *target) {
	AliasDef *ad = arena_alloc(c->arena, sizeof(AliasDef));
	ad->name = arena_strdup(c->arena, name);
	ad->target = target;
	ad->next = c->alias_defs;
	c->alias_defs = ad;
}

Type *resolve_alias_type(KawaCompiler *c, const char *name) {
	for (AliasDef *cur = c->alias_defs; cur; cur = cur->next) {
		if (strcmp(cur->name, name) == 0)
			return cur->target;
	}
	return NULL;
}

// Register a struct by name and LLVMTypeRef. Populates the field table from
// the AST field list (so callers don't need a separate "fill" pass that can
// disagree with what `register_struct` already saw).
void register_struct(KawaCompiler *c, const char *name, LLVMTypeRef type,
					 ASTNode *fields) {
	StructDef *sd = arena_alloc(c->arena, sizeof(StructDef));
	sd->name = arena_strdup(c->arena, name);
	sd->type = type;
	sd->field_count = 0;
	sd->next = c->struct_defs;
	c->struct_defs = sd;

	int idx = 0;
	for (ASTNode *f = fields; f && idx < 64; f = f->next) {
		sd->fields[idx].name = arena_strdup(c->arena, f->data.var_decl.name);
		sd->fields[idx].type = get_llvm_type(c, f->data_type);
		idx++;
	}
	sd->field_count = idx;
}

// Locate the StructDef whose LLVM type matches `struct_type` (pointer
// identity on LLVMTypeRef, since each named struct has a unique handle).
static StructDef *find_struct_def(KawaCompiler *c, LLVMTypeRef struct_type) {
	for (StructDef *sd = c->struct_defs; sd; sd = sd->next) {
		if (sd->type == struct_type)
			return sd;
	}
	return NULL;
}

StructDef *find_struct_def_pub(KawaCompiler *c, LLVMTypeRef struct_type) {
	return find_struct_def(c, struct_type);
}

// Struct-embedding promotion (IDEAS 3): ONE hop. When `field` names no
// direct field of `struct_type`, find the first direct embedded-struct
// field whose own surface declares it. *out_mid gets the embedded field's
// index, *out_field the member's index inside it. Direct fields always win
// (callers check first); shallowest embedded match wins (Go's depth rule).
int try_promoted_field(KawaCompiler *c, LLVMTypeRef struct_type,
					   const char *field, int *out_mid, int *out_field);

static int promoted_one_hop(KawaCompiler *c, LLVMTypeRef struct_type,
							const char *field, int *out_mid,
							int *out_field) {
	StructDef *sd = find_struct_def(c, struct_type);
	if (!sd)
		return 0;
	for (int i = 0; i < sd->field_count; i++) {
		if (LLVMGetTypeKind(sd->fields[i].type) != LLVMStructTypeKind)
			continue;
		StructDef *inner_sd = find_struct_def(c, sd->fields[i].type);
		if (!inner_sd)
			continue;
		for (int j = 0; j < inner_sd->field_count; j++) {
			if (strcmp(inner_sd->fields[j].name, field) == 0) {
				*out_mid = i;
				*out_field = j;
				return 1;
			}
		}
	}
	return 0;
}

static int promoted_deep(KawaCompiler *c, LLVMTypeRef struct_type,
						 const char *field, int *out_mid, int *out_field,
						 int depth);

// One hop at this level: direct surface of any direct embedded struct.
// Shallowest-first search over the whole embed tree. Returns the FIRST HOP
// (this level's embedded field index); the access site loops, re-resolving
// from the new container until the member is direct there.
static int promoted_deep(KawaCompiler *c, LLVMTypeRef struct_type,
						 const char *field, int *out_mid, int *out_field,
						 int depth) {
	if (depth > 16)
		return 0;
	if (promoted_one_hop(c, struct_type, field, out_mid, out_field))
		return 1;
	StructDef *sd = find_struct_def(c, struct_type);
	if (!sd)
		return 0;
	for (int i = 0; i < sd->field_count; i++) {
		if (LLVMGetTypeKind(sd->fields[i].type) != LLVMStructTypeKind)
			continue;
		int m2, f2;
		if (promoted_deep(c, sd->fields[i].type, field, &m2, &f2,
						  depth + 1)) {
			*out_mid = i;
			*out_field = -1; // signal: descend, not done
			return 1;
		}
	}
	return 0;
}

int try_promoted_field(KawaCompiler *c, LLVMTypeRef struct_type,
					   const char *field, int *out_mid, int *out_field) {
	return promoted_deep(c, struct_type, field, out_mid, out_field, 0);
}

// True when `struct_type` has a direct field named `field`. Promotion never
// shadows real members.
int has_direct_field(KawaCompiler *c, LLVMTypeRef struct_type,
					 const char *field) {
	StructDef *sd = find_struct_def(c, struct_type);
	if (!sd)
		return 0;
	for (int i = 0; i < sd->field_count; i++)
		if (strcmp(sd->fields[i].name, field) == 0)
			return 1;
	return 0;
}

// Name of the field at `index` in `struct_type`.
const char *sd_field_name(KawaCompiler *c, LLVMTypeRef struct_type,
						  int index) {
	StructDef *sd = find_struct_def(c, struct_type);
	if (!sd || index < 0 || index >= sd->field_count)
		return "";
	return sd->fields[index].name;
}

int get_field_index(KawaCompiler *c, LLVMTypeRef struct_type,
					const char *field_name) {
	StructDef *sd = find_struct_def(c, struct_type);
	if (!sd) {
		char *name = LLVMPrintTypeToString(struct_type);
		timbr_err("Internal error: unknown struct type in get_field_index "
				  "(type=%s, field=%s)\n",
				  name, field_name);
		LLVMDisposeMessage(name);
		exit(1);
	}
	for (int i = 0; i < sd->field_count; i++) {
		if (strcmp(sd->fields[i].name, field_name) == 0)
			return i;
	}
	timbr_err("Internal error: no field '%s' on struct (LLVM verifier should "
			  "have caught this earlier)\n",
			  field_name);
	exit(1);
}

LLVMTypeRef get_field_type(KawaCompiler *c, LLVMTypeRef struct_type,
						   const char *field_name) {
	StructDef *sd = find_struct_def(c, struct_type);
	if (!sd) {
		timbr_err("Internal error: unknown struct type in get_field_type\n");
		exit(1);
	}
	for (int i = 0; i < sd->field_count; i++) {
		if (strcmp(sd->fields[i].name, field_name) == 0)
			return sd->fields[i].type;
	}
	timbr_err("Internal error: no field '%s' on struct (LLVM verifier should "
			  "have caught this earlier)\n",
			  field_name);
	exit(1);
}

// Returns 1 if integer types are signed, 0 otherwise (including for floats,
// which carry their own sign via IEEE semantics and don't need a separate
// is_signed flag for div purposes).
static int kind_is_signed_int(TypeKind k) {
	switch (k) {
	case TYPE_I8:
	case TYPE_I16:
	case TYPE_I32:
	case TYPE_I64:
		return 1;
	default:
		return 0;
	}
}

int type_is_signed(KawaCompiler *c, Type *t) {
	if (!t)
		return 0;
	// Inside an instantiated generic, sign comes from the concrete type.
	if (c->generic_instantiating && t->kind == TYPE_STRUCT && t->name) {
		for (int gi = 0; gi < c->generic_param_count; gi++) {
			if (strcmp(c->generic_param_names[gi], t->name) == 0)
				return type_is_signed(c, c->generic_param_types[gi]);
		}
	}
	return kind_is_signed_int(t->kind);
}

// Map a Type* to an LLVMTypeRef. Sets t->is_signed as a side effect for
// integer kinds (so callers can read sign without re-checking the kind).
LLVMTypeRef get_llvm_type(KawaCompiler *c, Type *t) {
	if (!t)
		return LLVMInt32TypeInContext(c->context);

	switch (t->kind) {
	case TYPE_VOID:
		t->is_signed = 0;
		return LLVMVoidTypeInContext(c->context);
	case TYPE_BOOL:
		t->is_signed = 0;
		return LLVMInt1TypeInContext(c->context);
	case TYPE_CHAR:
		t->is_signed = 0;
		return LLVMInt8TypeInContext(c->context);
	case TYPE_I8:
	case TYPE_U8:
		t->is_signed = kind_is_signed_int(t->kind);
		return LLVMInt8TypeInContext(c->context);
	case TYPE_I16:
	case TYPE_U16:
		t->is_signed = kind_is_signed_int(t->kind);
		return LLVMInt16TypeInContext(c->context);
	case TYPE_I32:
	case TYPE_U32:
		t->is_signed = kind_is_signed_int(t->kind);
		return LLVMInt32TypeInContext(c->context);
	case TYPE_I64:
	case TYPE_U64:
		t->is_signed = kind_is_signed_int(t->kind);
		return LLVMInt64TypeInContext(c->context);
	case TYPE_F16:
		t->is_signed = 1; // IEEE float, signed-ness per op
		return LLVMHalfTypeInContext(c->context);
	case TYPE_BF16:
		t->is_signed = 1;
		return LLVMBFloatTypeInContext(c->context);
	case TYPE_F32:
		t->is_signed = 1; // IEEE float, signed-ness per op
		return LLVMFloatTypeInContext(c->context);
	case TYPE_F64:
		t->is_signed = 1;
		return LLVMDoubleTypeInContext(c->context);

	case TYPE_PTR: {
		// Resolve inner so `Car*` becomes `%Car*` (named struct ptr), not i8*.
		// The O3 pipeline keeps named pointer types so struct-aware
		// alias analysis still works.
		LLVMTypeRef inner = get_llvm_type(c, t->inner);
		t->is_signed = 0;
		return LLVMPointerType(inner, 0);
	}

	case TYPE_AMP: {
		// `&x` produces a pointer to the pointee's value type, same as TYPE_PTR
		// in Kawa's memory model.
		LLVMTypeRef inner = t->inner ? get_llvm_type(c, t->inner)
									 : LLVMInt8TypeInContext(c->context);
		t->is_signed = 0;
		return LLVMPointerType(inner, 0);
	}

	case TYPE_HANDLE:
		t->is_signed = 0;
		return LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);

	case TYPE_ARRAY: {
		LLVMTypeRef elem = get_llvm_type(c, t->inner);
		t->is_signed = 0;
		return LLVMArrayType(elem, (unsigned)t->array_len);
	}

	case TYPE_SET: {
		// Fixed representation: { i32* buf, i64 len, i64 cap }.
		// Note: only i32 elements are supported (matching the rest of the
		// codebase's set literal codegen). Generic set<T> would need a
		// different shape.
		LLVMTypeRef elems[] = {
			LLVMPointerType(LLVMInt32TypeInContext(c->context), 0),
			LLVMInt64TypeInContext(c->context),
			LLVMInt64TypeInContext(c->context)};
		t->is_signed = 0;
		return LLVMStructTypeInContext(c->context, elems, 3, 0);
	}

	case TYPE_SLICE:
		// []T is a fat pointer { T* data, i64 len }: a view with no
		// ownership and no capacity. Passed by value like a C struct;
		// bounds-checked on index in debug builds only.
		t->is_signed = 0;
		{
			LLVMTypeRef elem = t->inner ? get_llvm_type(c, t->inner)
										: LLVMInt8TypeInContext(c->context);
			LLVMTypeRef fields[] = {LLVMPointerType(elem, 0),
									LLVMInt64TypeInContext(c->context)};
			return LLVMStructTypeInContext(c->context, fields, 2, 0);
		}

	case TYPE_STRUCT: {
		// Generic instantiation (IDEAS 2.2): a bare type-param reference
		// resolves through the active instantiation map.
		if (c->generic_instantiating && t->name) {
			for (int gi = 0; gi < c->generic_param_count; gi++) {
				if (strcmp(c->generic_param_names[gi], t->name) == 0)
					return get_llvm_type(c, c->generic_param_types[gi]);
			}
		}
		// Follow alias chains first: `alias Bar = Foo` means a value of
		// declared type Bar is laid out exactly like Foo.
		Type *alias_target = resolve_alias_type(c, t->name);
		if (alias_target)
			return get_llvm_type(c, alias_target);

		// Already-declared struct in the module -> reuse.
		LLVMTypeRef struct_t = LLVMGetTypeByName(c->module, t->name);
		if (struct_t)
			return struct_t;

		// Forward reference or typo: create a named opaque placeholder so
		// the verifier can still produce a precise diagnostic. (Previously
		// this branch silently succeeded with a phantom struct, which made
		// typos turn into segfaults at runtime.)
		return LLVMStructCreateNamed(c->context, t->name);
	}

	case TYPE_ALIAS: {
		// Resolve via registry; fall back to opaque placeholder on miss.
		Type *alias_target = resolve_alias_type(c, t->name);
		if (alias_target)
			return get_llvm_type(c, alias_target);
		return LLVMInt32TypeInContext(c->context);
	}

	default:
		// An unknown TypeKind is a compiler bug -- refuse to silently
		// emit i32 (which previously caused miscompiles).
		timbr_err("Internal error: unknown TypeKind %d in get_llvm_type\n",
				  t->kind);
		exit(1);
	}
}
