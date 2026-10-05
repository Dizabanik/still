#include "codegen_internal.h"

typedef struct KawaValueLayoutCache {
    LLVMTypeRef type;
    Type *ast_type;
    LLVMValueRef layout;
    struct KawaValueLayoutCache *next;
} KawaValueLayoutCache;
int kawa_value_contains_arena(KawaCompiler *c, Type *type) {
    type=kawa_resolve_type(c,type);
    if (!type) return 0;
    if (type->kind==TYPE_ARENA) return 1;
    if (type->kind==TYPE_ARRAY) return kawa_value_contains_arena(c,type->inner);
    if (type->kind==TYPE_STRUCT) {
        StructDef *sd=find_struct_def_pub(c,get_llvm_type(c,type));
        for (int i=0; sd && i<sd->field_count; ++i)
            if (kawa_value_contains_arena(c,sd->fields[i].ast_type)) return 1;
    }
    return 0;
}

/* A nested array contributes one repeated field, not one metadata entry or
 * instruction per element. Small constant layouts can specialize in LLVM;
 * large arrays retain bounded compiler memory and compact iteration code. */
LLVMValueRef kawa_memory_layout(KawaCompiler *c, Type *type) {
    type=kawa_resolve_type(c,type);
    LLVMTypeRef value_type=get_llvm_type(c,type);
    for (KawaValueLayoutCache *it=c->value_layouts; it; it=it->next)
        if (it->type==value_type && kawa_types_same(it->ast_type,type)) return it->layout;
    LLVMTypeRef i64=LLVMInt64TypeInContext(c->context), pointer=LLVMPointerTypeInContext(c->context,0);
    LLVMTypeRef field_types[]={i64,i64,i64,pointer}, layout_types[]={i64,i64,pointer};
    LLVMTypeRef field_type=LLVMStructTypeInContext(c->context,field_types,4,0);
    LLVMTypeRef layout_type=LLVMStructTypeInContext(c->context,layout_types,3,0);
    StructDef *structure=type->kind==TYPE_STRUCT ? find_struct_def_pub(c,value_type) : NULL;
    unsigned capacity=structure ? structure->field_count : 1, count=0;
    LLVMValueRef *fields=arena_alloc(c->arena,(capacity ? capacity : 1)*sizeof(*fields));
    for (unsigned i=0; i<capacity; ++i) {
        Type *child=structure ? structure->fields[i].ast_type : type->inner;
        child=kawa_resolve_type(c,child);
        if (!kawa_contains_managed(c,child,1)) continue;
        uint64_t offset=structure ? LLVMOffsetOfElement(c->target_data,value_type,i) : 0;
        uint64_t repeat=structure ? 1 : (uint64_t)type->array_len;
        LLVMValueRef parts[]={
            LLVMConstInt(i64,offset,0), LLVMConstInt(i64,repeat,0),
            LLVMSizeOf(get_llvm_type(c,child)),
            kawa_is_owner(child) ? LLVMConstNull(pointer) : kawa_memory_layout(c,child)
        };
        fields[count++]=LLVMConstNamedStruct(field_type,parts,4);
    }
    LLVMTypeRef array_type=LLVMArrayType(field_type,count);
    LLVMValueRef entries=LLVMAddGlobal(c->module,array_type,"__kawa_value_fields");
    LLVMSetInitializer(entries,LLVMConstArray(field_type,fields,count));
    LLVMSetLinkage(entries,LLVMPrivateLinkage);
    LLVMSetGlobalConstant(entries,1);
    LLVMSetUnnamedAddress(entries,LLVMGlobalUnnamedAddr);
    LLVMValueRef layout=LLVMAddGlobal(c->module,layout_type,"__kawa_value_layout");
    LLVMValueRef parts[]={LLVMSizeOf(value_type),LLVMConstInt(i64,count,0),entries};
    LLVMSetInitializer(layout,LLVMConstNamedStruct(layout_type,parts,3));
    LLVMSetLinkage(layout,LLVMPrivateLinkage);
    LLVMSetGlobalConstant(layout,1);
    LLVMSetUnnamedAddress(layout,LLVMGlobalUnnamedAddr);
    KawaValueLayoutCache *cached=arena_alloc(c->arena,sizeof(*cached));
    *cached=(KawaValueLayoutCache){value_type,type,layout,c->value_layouts};
    c->value_layouts=cached;
    return layout;
}
static LLVMValueRef invoke(KawaCompiler *c,const char *name,LLVMValueRef *args,unsigned count) {
    c->uses_memory=1;
    LLVMValueRef fn=LLVMGetNamedFunction(c->module,name);
    if (!fn) {
        LLVMTypeRef types[4];
        for (unsigned i=0; i<count; ++i) types[i]=LLVMTypeOf(args[i]);
        fn=LLVMAddFunction(c->module,name,LLVMFunctionType(LLVMVoidTypeInContext(c->context),types,count,0));
    }
    LLVMValueRef call=LLVMBuildCall2(c->builder,LLVMGlobalGetValueType(fn),fn,args,count,"");
    kawa_report_attach(c,call,kawa_report_site(c,NULL,"memory_operation",name,NULL));
    return call;
}
void kawa_memory_cleanup_value(KawaCompiler *c, LLVMValueRef slot, Type *type) {
    if (kawa_is_owner(kawa_resolve_type(c,type))) { kawa_memory_cleanup(c,slot,0); return; }
    LLVMValueRef args[]={slot,kawa_memory_layout(c,type)};
    invoke(c,"__kawa_mem_value_drop",args,2);
}
void kawa_memory_defer_value(KawaCompiler *c, LLVMValueRef slot, Type *type) {
    kawa_memory_defer(c,slot,0);
    if (!kawa_is_owner(kawa_resolve_type(c,type))) c->defer_stack->memory_type=type;
}
void kawa_memory_store_value(KawaCompiler *c, LLVMValueRef slot, LLVMValueRef value,
                             Type *type, LLVMValueRef container) {
    LLVMValueRef incoming=create_entry_block_alloca(c,LLVMTypeOf(value),"incoming_value");
    LLVMBuildStore(c->builder,value,incoming);
    LLVMValueRef guard=LLVMConstNull(LLVMPointerTypeInContext(c->context,0));
    if (container) {
        guard=create_entry_block_alloca(c,LLVMTypeOf(container),"value_container");
        LLVMBuildStore(c->builder,container,guard);
    }
    LLVMValueRef args[]={slot,incoming,kawa_memory_layout(c,type),guard};
    invoke(c,"__kawa_mem_value_store",args,4);
}
