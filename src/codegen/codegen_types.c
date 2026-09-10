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
		sd->fields[idx].default_expr =
			f->data.var_decl.field_default;
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

// Resolve the struct type of an indexing base (`g[i]`): the parser stamps
// data_type on computed bases but leaves plain VAR_REFs to scope lookup.
Type *index_base_struct_type(KawaCompiler *c, ASTNode *obj) {
	Type *bt = obj->data_type;
	if (!bt && obj->type == NODE_VAR_REF) {
		Scope *bs = scope_find(c, obj->data.var_ref.name);
		if (bs && bs->node)
			bt = bs->node->data_type;
	}
	if (bt && bt->kind == TYPE_STRUCT && bt->name && strlen(bt->name) > 1)
		return bt;
	return NULL;
}

int get_field_index(KawaCompiler *c, LLVMTypeRef struct_type,
					const char *field_name) {
	StructDef *sd = find_struct_def(c, struct_type);
	if (!sd) {
		char *name = LLVMPrintTypeToString(struct_type);
		kdiag_error_at(KAWA_E_SEMANTIC, "<kawa>", NULL, 0,
					   "unknown struct type in get_field_index "
					   "(type=%s, field=%s)", // internal
					   name, field_name);
		LLVMDisposeMessage(name);
		exit(1);
	}
	for (int i = 0; i < sd->field_count; i++) {
		if (strcmp(sd->fields[i].name, field_name) == 0)
			return i;
	}
	kdiag_error_at(KAWA_E_SEMANTIC, "<kawa>", NULL, 0,
				   "no field `%s` on struct", // internal
				   field_name);
	exit(1);
}

LLVMTypeRef get_field_type(KawaCompiler *c, LLVMTypeRef struct_type,
						   const char *field_name) {
	StructDef *sd = find_struct_def(c, struct_type);
	if (!sd) {
		kdiag_error_at(KAWA_E_SEMANTIC, "<kawa>", NULL, 0,
					   "unknown struct type in get_field_type"); // internal
		exit(1);
	}
	for (int i = 0; i < sd->field_count; i++) {
		if (strcmp(sd->fields[i].name, field_name) == 0)
			return sd->fields[i].type;
	}
	kdiag_error_at(KAWA_E_SEMANTIC, "<kawa>", NULL, 0,
				   "no field `%s` on struct", // internal
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

ASTNode *find_enum_decl(KawaCompiler *c, const char *name) {
	if (!name || !c || !c->program_root)
		return NULL;
	for (ASTNode *s = c->program_root->next; s; s = s->next) {
		if (s->type == NODE_ENUM_DECL && s->data.enum_decl.name &&
			strcmp(s->data.enum_decl.name, name) == 0)
			return s;
	}
	return NULL;
}

EnumVariant *find_enum_variant(ASTNode *enum_decl, const char *variant_name) {
	if (!enum_decl || !variant_name)
		return NULL;
	for (EnumVariant *v = enum_decl->data.enum_decl.variants; v; v = v->next) {
		if (strcmp(v->name, variant_name) == 0)
			return v;
	}
	return NULL;
}

int get_enum_max_payload_words(KawaCompiler *c, const char *name) {
	ASTNode *en = find_enum_decl(c, name);
	if (!en)
		return 1;
	int max_words = 0;
	for (EnumVariant *v = en->data.enum_decl.variants; v; v = v->next) {
		int words = 0;
		for (int i = 0; i < v->payload_count; i++) {
			Type *pt = v->payload_types[i];
			if (!pt) continue;
			if (pt->kind == TYPE_SLICE)
				words += 2;
			else if (pt->kind == TYPE_ARRAY) {
				int elem_words = (pt->inner && pt->inner->kind == TYPE_SLICE) ? 2 : 1;
				words += (int)(pt->array_len * elem_words);
			} else {
				words += 1;
			}
		}
		if (words > max_words)
			max_words = words;
	}
	return max_words > 0 ? max_words : 1;
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

	case TYPE_CHAN:
		// chan<T> is a buffered ring: { T* buf, i64 cap, i64 head,
		// i64 count, i64 mask }. `cap` is the user-visible capacity (the
		// blocking threshold), `head` is a monotonic counter, and `mask`
		// is alloc-1 where alloc is the power-of-two slot count actually
		// allocated (>= cap) -- indexing is head & mask, never urem.
		// Single-threaded cooperative semantics -- blocking ops yield via
		// coro suspends, so no atomics or locks exist in the generated
		// code.
		t->is_signed = 0;
		{
			LLVMTypeRef elem = t->inner ? get_llvm_type(c, t->inner)
										: LLVMInt8TypeInContext(c->context);
			LLVMTypeRef i64t = LLVMInt64TypeInContext(c->context);
			LLVMTypeRef fields[] = {LLVMPointerType(elem, 0), i64t, i64t,
									i64t, i64t};
			return LLVMStructTypeInContext(c->context, fields, 5, 0);
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
			LLVMTypeRef st = LLVMStructTypeInContext(c->context, fields, 2, 0);
			return st;
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

	case TYPE_ENUM: {
		if (!t->name)
			return LLVMInt64TypeInContext(c->context);
		char enum_struct_name[256];
		snprintf(enum_struct_name, sizeof(enum_struct_name), "enum.%s", t->name);
		LLVMTypeRef enum_t = LLVMGetTypeByName(c->module, enum_struct_name);
		if (enum_t)
			return enum_t;
		int max_words = get_enum_max_payload_words(c, t->name);
		LLVMTypeRef fields[2];
		fields[0] = LLVMInt64TypeInContext(c->context); // tag
		fields[1] = LLVMArrayType(LLVMInt64TypeInContext(c->context), max_words > 0 ? max_words : 1);
		enum_t = LLVMStructCreateNamed(c->context, enum_struct_name);
		LLVMStructSetBody(enum_t, fields, 2, 0);
		return enum_t;
	}

	default:
		// An unknown TypeKind is a compiler bug -- refuse to silently
		// emit i32 (which previously caused miscompiles).
		kdiag_error_at(KAWA_E_SEMANTIC, "<kawa>", NULL, 0,
					   "unknown TypeKind %d in get_llvm_type", // internal
				  t->kind);
		exit(1);
	}
}

// --- Function overloading --------------------------------------------

// Canonical name of a type for symbol mangling. Structs use their own
// names; slices/arrays/pointers wrap the element name.
static const char *mangle_type_name(KawaCompiler *c, Type *t, char *buf,
									size_t bufsz) {
	(void)c;
	switch (t->kind) {
	case TYPE_I8: return "i8";
	case TYPE_U8: return "u8";
	case TYPE_I16: return "i16";
	case TYPE_U16: return "u16";
	case TYPE_I32: return "i32";
	case TYPE_U32: return "u32";
	case TYPE_I64: return "i64";
	case TYPE_U64: return "u64";
	case TYPE_F16: return "f16";
	case TYPE_BF16: return "bf16";
	case TYPE_F32: return "f32";
	case TYPE_F64: return "f64";
	case TYPE_BOOL: return "bool";
	case TYPE_VOID: return "void";
	case TYPE_SLICE:
	case TYPE_ARRAY:
	case TYPE_PTR: {
		if (!t->inner)
			return "?ptr";
		char inner[128];
		const char *in =
			mangle_type_name(c, t->inner, inner, sizeof(inner));
		snprintf(buf, bufsz, t->kind == TYPE_SLICE ? "slice_%s"
						   : t->kind == TYPE_ARRAY ? "arr%s_%s"
												 : "ptr_%s",
				 t->kind == TYPE_ARRAY ? "" : "", in);
		return buf;
	}
	default:
		if (t->name) {
			snprintf(buf, bufsz, "%s", t->name);
			return buf;
		}
		return "?";
	}
}

// Structural type equality for overload resolution: kinds must match,
// element/inner types recursively.
int kawa_types_same(Type *a, Type *b) {
	while (a && b) {
		if (a->kind != b->kind)
			return 0;
		if (a->kind == TYPE_STRUCT || a->kind == TYPE_ALIAS ||
			a->kind == TYPE_CHAN) {
			const char *an = a->name, *bn = b->name;
			if (an != bn && (!an || !bn || strcmp(an, bn) != 0))
				return 0;
			// Structs/chans have no further structure to compare here;
			// the name is the identity.
			return a->kind == TYPE_STRUCT || a->kind == TYPE_ALIAS
					   ? 1
					   : kawa_types_same(a->inner, b->inner);
		}
		if (a->kind == TYPE_ARRAY && a->array_len != b->array_len)
			return 0;
		a = a->inner;
		b = b->inner;
	}
	return a == b; // both NULL ends the walk
}

// Build `bare__t1_t2` from declared param types. Result is arena-owned.
static char *overload_mangled_name(KawaCompiler *c, const char *bare,
								   ASTNode *args) {
	size_t need = strlen(bare) + 4;
	for (ASTNode *a = args; a; a = a->next)
		need += 24;
	char *out = arena_alloc(c->arena, need + 1);
	int n = snprintf(out, need, "%s", bare);
	char buf[160];
	int first = 1;
	for (ASTNode *a = args; a; a = a->next) {
		if (a->data_type) {
			n += snprintf(out + n, need - (size_t)n, "%s%s", first ? "__" : "_",
						  mangle_type_name(c, a->data_type, buf, sizeof(buf)));
			first = 0;
		}
	}
	if (first)
		snprintf(out + n, need - (size_t)n, "__");
	return out;
}

// Pass over all decls: register every NODE_FUNC_DECL whose bare name is
// declared more than once (with distinct param lists). Called before the
// emission walk so codegen_func_decl can rename overloads on the way by.
void collect_overloads(KawaCompiler *c, ASTNode *root) {
	// Count decls per bare name.
	struct { const char *name; int count; } names[64];
	int nn = 0;
	for (ASTNode *g = root; g; g = g->next) {
		if (g->type != NODE_FUNC_DECL || !g->data.func.name)
			continue;
		int found = 0;
		for (int i = 0; i < nn; i++) {
			if (strcmp(names[i].name, g->data.func.name) == 0) {
				names[i].count++;
				found = 1;
				break;
			}
		}
		if (!found && nn < 64) {
			names[nn].name = g->data.func.name;
			names[nn].count = 1;
			nn++;
		}
	}
	// Register every fn sharing an overloaded name.
	for (ASTNode *g = root; g; g = g->next) {
		if (g->type != NODE_FUNC_DECL || !g->data.func.name)
			continue;
		int dup = 0;
		for (int i = 0; i < nn; i++)
			if (strcmp(names[i].name, g->data.func.name) == 0 &&
				names[i].count > 1)
				dup = 1;
		if (!dup || c->overload_fn_count >= 64)
			continue;
		int np = 0;
		for (ASTNode *a = g->data.func.args; a; a = a->next)
			np++;
		struct OverloadFn *of = &c->overload_fns[c->overload_fn_count];
		of->bare = g->data.func.name;
		of->decl = g;
		of->mangled =
			overload_mangled_name(c, g->data.func.name, g->data.func.args);
		of->nparams = np;
		of->params = arena_alloc(c->arena,
								 sizeof(Type *) * (np > 0 ? np : 1));
		int pi2 = 0;
		for (ASTNode *a = g->data.func.args; a; a = a->next)
			of->params[pi2++] = a->data_type;
		c->overload_fn_count++;
	}
	// Record which bare names are overloaded for cheap call-site checks.
	for (int i = 0; i < nn && c->overload_name_count < 32; i++) {
		if (names[i].count > 1)
			c->overload_names[c->overload_name_count++] =
				(char *)names[i].name;
	}
}

// Pre-register every impl method as "Struct__method" so bodies can check
// method existence regardless of emission order (operator overloading,
// self_index). Also fills the overload registry's needs.
void collect_impl_methods(KawaCompiler *c, ASTNode *root) {
	for (ASTNode *g = root; g; g = g->next) {
		if (g->type != NODE_IMPL_BLOCK ||
			c->impl_method_count >= 256)
			continue;
		for (ASTNode *m = g->data.impl.methods; m; m = m->next) {
			if (m->type != NODE_FUNC_DECL || !m->data.func.name)
				continue;
			if (c->impl_method_count >= 256)
				break;
			// Impl methods are stored under their MANGLED name
			// (`Grid__self_index_set`) -- the parser prefixes the struct
			// when it builds the decl, matching how they're emitted.
			c->impl_methods[c->impl_method_count++] =
				arena_strdup(c->arena, m->data.func.name);
		}
	}
}

int impl_has_method(KawaCompiler *c, const char *struct_name,
					const char *method) {
	if (!struct_name)
		return 0;
	char buf[256];
	snprintf(buf, sizeof(buf), "%s__%s", struct_name, method);
	for (int i = 0; i < c->impl_method_count; i++)
		if (strcmp(c->impl_methods[i], buf) == 0)
			return 1;
	return 0;
}

// Is this bare name part of an overload set?
int is_overloaded_name(KawaCompiler *c, const char *bare) {
	for (int i = 0; i < c->overload_name_count; i++)
		if (strcmp(c->overload_names[i], bare) == 0)
			return 1;
	return 0;
}

// Find the overload whose param list matches the given argument types
// exactly. Returns the mangled symbol or NULL. Ambiguity reports and exits.
const char *resolve_overload(KawaCompiler *c, ASTNode *call,
							 const char *bare, ASTNode *args) {
	Type *argt[16];
	int na = 0;
	for (ASTNode *a = args; a; a = a->next, na++) {
		Type *at = a->data_type;
		if (!at && a->type == NODE_VAR_REF) {
			Scope *sv = scope_find(c, a->data.var_ref.name);
			if (sv && sv->node && sv->node->data_type)
				at = sv->node->data_type;
		}
		argt[na] = at;
	}
	struct OverloadFn *match = NULL;
	int matches = 0;
	for (int i = 0; i < c->overload_fn_count; i++) {
		struct OverloadFn *of = &c->overload_fns[i];
		if (strcmp(of->bare, bare) != 0)
			continue;
		if (of->nparams != na)
			continue;
		int ok = 1;
		for (int pi = 0; pi < na && ok; pi++) {
			if (!of->params[pi] || !argt[pi] ||
				!kawa_types_same(of->params[pi], argt[pi]))
				ok = 0;
		}
		if (!ok)
			continue;
		match = of;
		matches++;
	}
	if (matches == 1)
		return match->mangled;
	if (matches > 1) {
		kerr(KAWA_E_TYPE, call, "ambiguous call to `%s`: %d overloads "
			  "match these argument types", bare, matches);
		exit(1);
	}
	return NULL;
}

// Count arguments at a call node (helper for overload diagnostics).
int resolve_overload_arg_count(ASTNode *call) {
	int na = 0;
	for (ASTNode *a = call->data.call.args; a; a = a->next)
		na++;
	return na;
}
