#include "codegen_internal.h"

typedef struct WkyValueLayoutCache {
    LLVMTypeRef type;
    Type *ast_type;
    LLVMValueRef layout;
    struct WkyValueLayoutCache *next;
} WkyValueLayoutCache;
static int has_callback(StillCompiler *c, Type *type, Type **path, unsigned depth, int include_arenas) {
    type=wky_resolve_type(c,type);
    if (!type) return 0;
    if (type->kind==TYPE_HANDLE || (include_arenas && type->kind==TYPE_ARENA)) return 1;
    if (type->kind==TYPE_REF || type->kind==TYPE_PTR || type->kind==TYPE_AMP) return 0;
    if (depth==128) return 0;
    for (unsigned i=0; i<depth; ++i)
        if (path[i]==type || (type->kind==TYPE_STRUCT && path[i]->kind==TYPE_STRUCT &&
            type->name && path[i]->name && !strcmp(type->name,path[i]->name))) return 0;
    path[depth]=type;
    if (type->kind==TYPE_STRUCT) {
        StructDef *sd=find_struct_def_pub(c,get_llvm_type(c,type));
        for (int i=0; sd && i<sd->field_count; ++i)
            if (has_callback(c,sd->fields[i].ast_type,path,depth+1,include_arenas)) return 1;
    }
    if (wky_is_tagged(type)) {
        ASTNode *en=wky_tagged_decl(c,type);
        for (EnumVariant *v=en ? en->data.enum_decl.variants : NULL; v; v=v->next)
            for (int i=0; i<v->payload_count; ++i)
                if (has_callback(c,v->payload_types[i],path,depth+1,include_arenas)) return 1;
    }
    return has_callback(c,type->inner,path,depth+1,include_arenas) || has_callback(c,type->error,path,depth+1,include_arenas);
}
int wky_value_contains_handle(StillCompiler *c, Type *type) {
    Type *path[128]; return has_callback(c,type,path,0,0);
}
static int contains_callback(StillCompiler *c, Type *type) {
    Type *path[128]; return has_callback(c,type,path,0,1);
}
int wky_value_contains_arena(StillCompiler *c, Type *type) {
    type=wky_resolve_type(c,type);
    if (!type) return 0;
    if (type->kind==TYPE_ARENA) return 1;
    if (type->kind==TYPE_ARRAY) return wky_value_contains_arena(c,type->inner);
    if (wky_is_tagged(type)) {
        ASTNode *en=wky_tagged_decl(c,type);
        for (EnumVariant *v=en ? en->data.enum_decl.variants : NULL; v; v=v->next)
            for (int i=0; i<v->payload_count; ++i)
                if (wky_value_contains_arena(c,v->payload_types[i])) return 1;
    }
    if (type->kind==TYPE_STRUCT) {
        StructDef *sd=find_struct_def_pub(c,get_llvm_type(c,type));
        for (int i=0; sd && i<sd->field_count; ++i)
            if (wky_value_contains_arena(c,sd->fields[i].ast_type)) return 1;
    }
    return 0;
}

/* A nested array contributes one repeated field, not one metadata entry or
 * instruction per element. Small constant layouts can specialize in LLVM;
 * large arrays retain bounded compiler memory and compact iteration code. */
LLVMValueRef wky_memory_layout(StillCompiler *c, Type *type) {
    type=wky_resolve_type(c,type);
    LLVMTypeRef value_type=get_llvm_type(c,type);
    for (WkyValueLayoutCache *it=c->value_layouts; it; it=it->next)
        if (it->type==value_type && wky_types_same(it->ast_type,type)) return it->layout;
    LLVMTypeRef i64=LLVMInt64TypeInContext(c->context), pointer=LLVMPointerTypeInContext(c->context,0);
    LLVMTypeRef field_types[]={i64,i64,i64,pointer,i64,i64}, layout_types[]={i64,i64,pointer};
    LLVMTypeRef field_type=LLVMStructTypeInContext(c->context,field_types,6,0);
    LLVMTypeRef layout_type=LLVMStructTypeInContext(c->context,layout_types,3,0);
    StructDef *structure=type->kind==TYPE_STRUCT ? find_struct_def_pub(c,value_type) : NULL;
    ASTNode *tagged=wky_is_tagged(type) ? wky_tagged_decl(c,type) : NULL;
    unsigned capacity=structure ? structure->field_count : 1, count=0;
    if (tagged) {
        capacity=0;
        for (EnumVariant *v=tagged->data.enum_decl.variants; v; v=v->next) capacity+=v->payload_count;
    }
    LLVMValueRef *fields=arena_alloc(c->arena,(capacity ? capacity : 1)*sizeof(*fields));
    EnumVariant *variant=tagged ? tagged->data.enum_decl.variants : NULL;
    unsigned payload_index=0;
    for (unsigned i=0; i<capacity; ++i) {
        while (variant && payload_index>=(unsigned)variant->payload_count) { variant=variant->next; payload_index=0; }
        Type channel_buffer={.kind=TYPE_OWNER,.inner=type->inner};
        Type *child=variant ? variant->payload_types[payload_index] : structure ? structure->fields[i].ast_type :
            type->kind==TYPE_CHAN ? &channel_buffer : type->inner;
        uint64_t offset=structure ? LLVMOffsetOfElement(c->target_data,value_type,i) : 0;
        if (variant) {
            LLVMTypeRef payload_types[16];
            for (int j=0; j<variant->payload_count; ++j) payload_types[j]=get_llvm_type(c,variant->payload_types[j]);
            LLVMTypeRef payload=LLVMStructTypeInContext(c->context,payload_types,variant->payload_count,0);
            offset=LLVMOffsetOfElement(c->target_data,value_type,1)+LLVMOffsetOfElement(c->target_data,payload,payload_index++);
        }
        child=wky_resolve_type(c,child);
        if (!wky_contains_managed(c,child,1)) continue;
        uint64_t repeat=structure || tagged || type->kind==TYPE_CHAN ? 1 : (uint64_t)type->array_len;
        LLVMValueRef parts[]={
            LLVMConstInt(i64,offset,0), LLVMConstInt(i64,repeat,0),
            LLVMSizeOf(get_llvm_type(c,child)),
            wky_is_owner(child) ? LLVMConstNull(pointer) : wky_memory_layout(c,child),
            LLVMConstInt(i64,variant ? (uint64_t)variant->tag : 0,0), LLVMConstInt(i64,variant!=NULL,0)
        };
        fields[count++]=LLVMConstNamedStruct(field_type,parts,6);
    }
    LLVMTypeRef array_type=LLVMArrayType(field_type,count);
    LLVMValueRef entries=LLVMAddGlobal(c->module,array_type,"__wky_value_fields");
    LLVMSetInitializer(entries,LLVMConstArray(field_type,fields,count));
    LLVMSetLinkage(entries,LLVMPrivateLinkage);
    LLVMSetGlobalConstant(entries,1);
    LLVMSetUnnamedAddress(entries,LLVMGlobalUnnamedAddr);
    LLVMValueRef layout=LLVMAddGlobal(c->module,layout_type,"__wky_value_layout");
    LLVMValueRef parts[]={LLVMSizeOf(value_type),LLVMConstInt(i64,count,0),entries};
    LLVMSetInitializer(layout,LLVMConstNamedStruct(layout_type,parts,3));
    LLVMSetLinkage(layout,LLVMPrivateLinkage);
    LLVMSetGlobalConstant(layout,1);
    LLVMSetUnnamedAddress(layout,LLVMGlobalUnnamedAddr);
    WkyValueLayoutCache *cached=arena_alloc(c->arena,sizeof(*cached));
    *cached=(WkyValueLayoutCache){value_type,type,layout,c->value_layouts};
    c->value_layouts=cached;
    return layout;
}
static LLVMValueRef invoke(StillCompiler *c,const char *name,LLVMValueRef *args,unsigned count) {
    c->uses_memory=1;
    LLVMValueRef fn=LLVMGetNamedFunction(c->module,name);
    if (!fn) {
        LLVMTypeRef types[4];
        for (unsigned i=0; i<count; ++i) types[i]=LLVMTypeOf(args[i]);
        fn=LLVMAddFunction(c->module,name,LLVMFunctionType(LLVMVoidTypeInContext(c->context),types,count,0));
    }
    LLVMValueRef call=LLVMBuildCall2(c->builder,LLVMGlobalGetValueType(fn),fn,args,count,"");
    still_report_attach(c,call,still_report_site(c,NULL,"memory_operation",name,NULL));
    return call;
}
void wky_memory_cleanup_value(StillCompiler *c, LLVMValueRef slot, Type *type) {
    if (wky_resolve_type(c,type)->kind==TYPE_HANDLE) { wky_coro_drop(c,slot); return; }
    if (wky_is_owner(wky_resolve_type(c,type))) { wky_mark_cleanup_effect(c,wky_memory_cleanup(c,slot,0),type); return; }
    LLVMValueRef args[]={slot,wky_memory_layout(c,type)};
    wky_mark_cleanup_effect(c,invoke(c,"__wky_mem_value_drop",args,2),type);
}
void wky_memory_defer_value(StillCompiler *c, LLVMValueRef slot, Type *type) {
    wky_memory_defer(c,slot,0);
    if (!wky_is_owner(wky_resolve_type(c,type)) || contains_callback(c,type)) c->defer_stack->memory_type=type;
}
void wky_memory_store_value(StillCompiler *c, LLVMValueRef slot, LLVMValueRef value,
                             Type *type, LLVMValueRef container) {
    LLVMValueRef incoming=create_entry_block_alloca(c,LLVMTypeOf(value),"incoming_value");
    LLVMBuildStore(c->builder,value,incoming);
    LLVMValueRef guard=LLVMConstNull(LLVMPointerTypeInContext(c->context,0));
    if (container) {
        guard=create_entry_block_alloca(c,LLVMTypeOf(container),"value_container");
        LLVMBuildStore(c->builder,container,guard);
    }
    LLVMValueRef args[]={slot,incoming,wky_memory_layout(c,type),guard};
    wky_mark_cleanup_effect(c,invoke(c,"__wky_mem_value_store",args,4),type);
}

void wky_mark_cleanup_effect(StillCompiler *c, LLVMValueRef call, Type *type) {
    if (!contains_callback(c,type)) return;
    const char *name="wky.cleanup.callback";
    LLVMMetadataRef payload=LLVMMDStringInContext2(c->context,"resource cleanup may allocate",sizeof("resource cleanup may allocate")-1);
    LLVMSetMetadata(call,LLVMGetMDKindIDInContext(c->context,name,(unsigned)strlen(name)),
        LLVMMetadataAsValue(c->context,LLVMMDNodeInContext2(c->context,&payload,1)));
}
