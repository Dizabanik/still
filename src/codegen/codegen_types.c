#include "codegen_internal.h"

StructDef *struct_defs = NULL;
AliasDef *alias_defs = NULL;

void register_alias(const char *name, Type *target) {
	AliasDef *ad = malloc(sizeof(AliasDef));
	ad->name = strdup(name);
	ad->target = target;
	ad->next = alias_defs;
	alias_defs = ad;
}

Type *resolve_alias_type(const char *name) {
	AliasDef *cur = alias_defs;
	while (cur) {
		if (strcmp(cur->name, name) == 0)
			return cur->target;
		cur = cur->next;
	}
	return NULL;
}

void register_struct(const char *name, LLVMTypeRef type) {
	StructDef *sd = malloc(sizeof(StructDef));
	sd->name = strdup(name);
	sd->type = type;
	sd->field_count = 0;
	sd->next = struct_defs;
	struct_defs = sd;
}

int get_field_index(KawaCompiler *c, LLVMTypeRef struct_type,
					const char *field_name) {
	StructDef *sd = struct_defs;
	while (sd) {
		if (sd->type == struct_type) {
			for (int i = 0; i < sd->field_count; i++) {
				if (strcmp(sd->fields[i].name, field_name) == 0)
					return i;
			}
		}
		sd = sd->next;
	}
	return 0;
}

LLVMTypeRef get_field_type(KawaCompiler *c, LLVMTypeRef struct_type,
						   const char *field_name) {
	StructDef *sd = struct_defs;
	while (sd) {
		if (sd->type == struct_type) {
			for (int i = 0; i < sd->field_count; i++) {
				if (strcmp(sd->fields[i].name, field_name) == 0)
					return sd->fields[i].type;
			}
		}
		sd = sd->next;
	}
	return LLVMInt32TypeInContext(c->context);
}

LLVMTypeRef get_llvm_type(KawaCompiler *c, Type *t) {
	if (!t)
		return LLVMInt32TypeInContext(c->context);

	if (t->kind == TYPE_PTR) {
		// Recursively resolve inner type so Car* becomes %Car* (struct ptr),
		// not i8*
		return LLVMPointerType(get_llvm_type(c, t->inner), 0);
	}
	if (t->kind == TYPE_HANDLE) {
		return LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
	}
	switch (t->kind) {
	case TYPE_VOID:
		return LLVMVoidTypeInContext(c->context);
	case TYPE_BOOL:
		return LLVMInt1TypeInContext(c->context);
	case TYPE_CHAR:
		return LLVMInt8TypeInContext(c->context);
	case TYPE_I8:
	case TYPE_U8:
		return LLVMInt8TypeInContext(c->context);
	case TYPE_I16:
	case TYPE_U16:
		return LLVMInt16TypeInContext(c->context);
	case TYPE_I32:
	case TYPE_U32:
		return LLVMInt32TypeInContext(c->context);
	case TYPE_I64:
	case TYPE_U64:
		return LLVMInt64TypeInContext(c->context);
	case TYPE_F32:
		return LLVMFloatTypeInContext(c->context);
	case TYPE_F64:
		return LLVMDoubleTypeInContext(c->context);
	case TYPE_SET: {
		LLVMTypeRef elems[] = {
			LLVMPointerType(LLVMInt32TypeInContext(c->context), 0),
			LLVMInt64TypeInContext(c->context),
			LLVMInt64TypeInContext(c->context)};
		return LLVMStructTypeInContext(c->context, elems, 3, 0);
	}
	case TYPE_STRUCT: {
		Type *alias_target = resolve_alias_type(t->name);
		if (alias_target) {
			return get_llvm_type(c, alias_target);
		}
		LLVMTypeRef struct_t = LLVMGetTypeByName(c->module, t->name);
		if (!struct_t) {
			struct_t = LLVMStructCreateNamed(c->context, t->name);
		}
		return struct_t;
	}
	default:
		return LLVMInt32TypeInContext(c->context);
	}
}
