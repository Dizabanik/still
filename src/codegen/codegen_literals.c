#include "codegen_internal.h"

void kawa_literal_context(KawaCompiler *c,ASTNode *n,Type *type) {
	if (!n || n->type!=NODE_STRUCT_LITERAL) return;
	if (!n->data_type) n->data_type=type;
    Type *actual=kawa_resolve_type(c,n->data_type);
    kawa_check_value_type(c,n,actual);
	if (!actual || (actual->kind!=TYPE_STRUCT && actual->kind!=TYPE_ARRAY)) {
		kerr(KAWA_E_TYPE,n,"aggregate literals require a struct or fixed array; use a typed constructor for other values");
		exit(1);
	}
}
LiteralPlan kawa_literal_plan(KawaCompiler *c,ASTNode *n,LLVMTypeRef type) {
	kawa_literal_context(c,n,n->data_type);
	unsigned count=0,fields=LLVMCountStructElementTypes(type),position=0;
	for (StructInitItem *item=n->data.struct_lit.items; item; item=item->next) ++count;
	LiteralPlan plan={.indices=arena_alloc(c->arena,(count ? count : 1)*sizeof(unsigned))};
	unsigned at=0;
	for (StructInitItem *item=n->data.struct_lit.items; item; item=item->next,++at) {
		if (item->spread_from) {
			if (plan.spread) { kerr(KAWA_E_ARGS,n,"a struct initializer permits one spread base"); exit(1); }
			plan.spread=item; continue;
		}
		unsigned index;
		if (item->field_name) index=(unsigned)get_field_index(c,type,item->field_name);
		else {
			while (position<fields && position<64 && (plan.provided & (UINT64_C(1)<<position))) ++position;
			index=position++;
		}
		if (index>=fields || index>=64) { kerr(KAWA_E_ARITY,n,"too many fields in struct initializer"); exit(1); }
		uint64_t mask=UINT64_C(1)<<index;
		if (plan.provided & mask) { kerr(KAWA_E_ARGS,n,"duplicate field in struct initializer"); exit(1); }
		plan.provided |= mask; plan.indices[at]=index;
	}
	return plan;
}
LLVMValueRef kawa_codegen_literal(KawaCompiler *c,ASTNode *n) {
	kawa_literal_context(c,n,n->data_type);
	LLVMTypeRef type=get_llvm_type(c,n->data_type);
	LLVMValueRef value=LLVMConstNull(type);
	Type *actual=kawa_resolve_type(c,n->data_type);
	if (LLVMGetTypeKind(type)==LLVMArrayTypeKind) {
		unsigned index=0,length=LLVMGetArrayLength(type);
		for (StructInitItem *item=n->data.struct_lit.items; item; item=item->next,++index) {
			if (item->field_name || item->spread_from) { kerr(KAWA_E_ARGS,n,"array initializers require positional elements"); exit(1); }
			if (index>=length) { kerr(KAWA_E_ARITY,n,"too many elements in array initializer"); exit(1); }
			kawa_literal_context(c,item->value,actual->inner);
			LLVMValueRef element=codegen_expr(c,item->value);
			element=coerce_value(c,element,kawa_expr_type(c,item->value),LLVMGetElementType(type),actual->inner);
			value=LLVMBuildInsertValue(c->builder,value,element,index,"array_element");
		}
		return value;
	}
	LiteralPlan plan=kawa_literal_plan(c,n,type);
	StructDef *definition=find_struct_def_pub(c,type);
	if (plan.spread) {
		kawa_literal_context(c,plan.spread->spread_from,actual);
		value=codegen_expr(c,plan.spread->spread_from);
		if (LLVMTypeOf(value)!=type) { kerr(KAWA_E_TYPE,n,"spread base must have the same struct type"); exit(1); }
	} else if (definition) {
		for (int i=0; i<definition->field_count; ++i) {
			ASTNode *default_value=definition->fields[i].default_expr;
			if (!default_value || (plan.provided & (UINT64_C(1)<<i))) continue;
			kawa_literal_context(c,default_value,definition->fields[i].ast_type);
			LLVMValueRef field=codegen_expr(c,default_value);
			field=coerce_value(c,field,kawa_expr_type(c,default_value),
				definition->fields[i].type,definition->fields[i].ast_type);
			value=LLVMBuildInsertValue(c->builder,value,field,(unsigned)i,"default_field");
		}
	}
	unsigned at=0;
	for (StructInitItem *item=n->data.struct_lit.items; item; item=item->next,++at) {
		if (item->spread_from) continue;
		unsigned index=plan.indices[at];
		Type *field_type=definition ? definition->fields[index].ast_type : NULL;
		kawa_literal_context(c,item->value,field_type);
		LLVMValueRef field=codegen_expr(c,item->value);
		field=coerce_value(c,field,kawa_expr_type(c,item->value),LLVMStructGetTypeAtIndex(type,index),field_type);
		value=LLVMBuildInsertValue(c->builder,value,field,index,"explicit_field");
	}
	return value;
}
