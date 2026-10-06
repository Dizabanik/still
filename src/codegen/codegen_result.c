#include "codegen_internal.h"

int wky_is_tagged(Type *type) {
    return type && (type->kind==TYPE_ENUM || type->kind==TYPE_OPTION || type->kind==TYPE_RESULT);
}

char *wky_type_key(StillCompiler *c, Type *type) {
    type=wky_concrete_type(c,type);
    if (!type) return "void";
    char *inner=type->inner ? wky_type_key(c,type->inner) : "";
    char *error=type->error ? wky_type_key(c,type->error) : "";
    const char *name=(type->kind==TYPE_STRUCT || type->kind==TYPE_ALIAS || type->kind==TYPE_ENUM) && type->name ? type->name : "";
    size_t size=strlen(inner)+strlen(error)+strlen(name)+96;
    char *key=arena_alloc(c->arena,size);
    snprintf(key,size,"k%d_n%zu_%s_a%ld_i%zu_%s_e%zu_%s",type->kind,
        strlen(name),name,type->array_len,
        strlen(inner),inner,strlen(error),error);
    return key;
}

ASTNode *wky_tagged_decl(StillCompiler *c, Type *type) {
    type=wky_concrete_type(c,type);
    if (!type || !wky_is_tagged(type)) return NULL;
    if (type->kind==TYPE_ENUM) return find_enum_decl(c,type->name);
    for (ASTNode *node=c->builtin_tagged_types; node; node=node->next)
        if (wky_types_same(node->data_type,type)) { type->name=node->data.enum_decl.name; return node; }
    char *inner=wky_type_key(c,type->inner), *error=type->error ? wky_type_key(c,type->error) : "";
    size_t size=strlen(inner)+strlen(error)+40;
    char *name=arena_alloc(c->arena,size);
    snprintf(name,size,"%s.%s.%s",type->kind==TYPE_OPTION ? "option" : "result",inner,error);
    type->name=name;
    ASTNode *node=arena_alloc(c->arena,sizeof(*node));
    node->type=NODE_ENUM_DECL; node->data_type=type; node->data.enum_decl.name=name;
    node->data.enum_decl.variant_count=2;
    EnumVariant *success=arena_alloc(c->arena,sizeof(*success));
    EnumVariant *failure=arena_alloc(c->arena,sizeof(*failure));
    success->name=type->kind==TYPE_OPTION ? "Some" : "Ok"; success->tag=0;
    failure->name=type->kind==TYPE_OPTION ? "None" : "Err"; failure->tag=1;
    success->payload_count=type->inner && type->inner->kind!=TYPE_VOID;
    success->payload_types[0]=type->inner;
    failure->payload_count=type->error && type->error->kind!=TYPE_VOID;
    failure->payload_types[0]=type->error;
    success->next=failure; node->data.enum_decl.variants=success;
    node->next=c->builtin_tagged_types; c->builtin_tagged_types=node;
    return node;
}

static LLVMValueRef payload_slot(StillCompiler *c, LLVMValueRef slot, Type *type, Type *payload) {
    LLVMTypeRef tagged=get_llvm_type(c,type);
    LLVMValueRef raw=LLVMBuildStructGEP2(c->builder,tagged,slot,1,"result_payload");
    LLVMTypeRef fields[]={get_llvm_type(c,payload)};
    LLVMTypeRef storage=LLVMStructTypeInContext(c->context,fields,1,0);
    return LLVMBuildStructGEP2(c->builder,storage,raw,0,"result_value");
}

LLVMValueRef wky_result_construct(StillCompiler *c, ASTNode *node, Type *type,
                                  int failure, LLVMValueRef value, Type *source) {
    type=wky_concrete_type(c,type);
    if (!type || (type->kind!=TYPE_OPTION && type->kind!=TYPE_RESULT)) {
        still_error(STILL_E_TYPE,node,"constructor requires an option<T> or result<T, E> context"); exit(1);
    }
    Type *payload=failure ? type->error : type->inner;
    LLVMTypeRef tagged=get_llvm_type(c,type);
    LLVMValueRef slot=create_entry_block_alloca(c,tagged,"result_new");
    LLVMBuildStore(c->builder,LLVMConstNull(tagged),slot);
    LLVMValueRef tag=LLVMBuildStructGEP2(c->builder,tagged,slot,0,"result_tag");
    LLVMBuildStore(c->builder,LLVMConstInt(LLVMInt64TypeInContext(c->context),failure,0),tag);
    if (payload && payload->kind!=TYPE_VOID) {
        if (!value) { still_error(STILL_E_ARITY,node,"constructor requires a payload"); exit(1); }
        value=coerce_value(c,value,source,get_llvm_type(c,payload),payload);
        LLVMBuildStore(c->builder,value,payload_slot(c,slot,type,payload));
    } else if (value) { still_error(STILL_E_ARITY,node,"constructor does not take a payload"); exit(1); }
    return LLVMBuildLoad2(c->builder,tagged,slot,"result");
}

void wky_propagate_failure(StillCompiler *c, ASTNode *node, LLVMValueRef value, Type *payload) {
    if (c->filter_stack) {
        FilterFrame *handler=c->filter_stack;
        Type default_error={.kind=TYPE_I32};
        Type *expected=handler->err_type ? wky_concrete_type(c,handler->err_type) : &default_error;
        if (!value || !payload || payload->kind==TYPE_VOID) {
            still_error(STILL_E_TYPE,node,"a dregs handler requires an error payload; propagate a void error to a result return instead"); exit(1);
        }
        if (!wky_types_same(wky_concrete_type(c,payload),expected)) {
            still_error(STILL_E_TYPE,node,"propagated error type does not match the active dregs handler"); exit(1);
        }
        LLVMBuildStore(c->builder,value,handler->err_slot);
        for (DeferFrame *d=c->defer_stack; d && d!=handler->defers_at_entry; d=d->next) run_defer_frame(c,d);
        LLVMBuildBr(c->builder,handler->catch_bb);
        return;
    }
    Type *ret=wky_concrete_type(c,c->current_ret_node_type);
    if (c->in_coroutine || !ret || (ret->kind!=TYPE_RESULT && ret->kind!=TYPE_OPTION) ||
        (ret->kind==TYPE_RESULT && !wky_types_same(wky_concrete_type(c,payload),ret->error)) ||
        (ret->kind==TYPE_OPTION && payload)) {
        still_error(STILL_E_TYPE,node,"propagation requires a matching fallible return type or typed dregs handler"); exit(1);
    }
    LLVMValueRef result=wky_result_construct(c,node,ret,1,value,payload);
    for (DeferFrame *d=c->defer_stack; d; d=d->next) run_defer_frame(c,d);
    LLVMBuildRet(c->builder,result);
}

LLVMValueRef wky_result_builtin(StillCompiler *c, ASTNode *node, const char *name) {
    int propagate=!strcmp(name,"try");
    int some=!strcmp(name,"some"), none=!strcmp(name,"none");
    int ok=!strcmp(name,"ok"), err=!strcmp(name,"err");
    if (!propagate && !some && !none && !ok && !err) return NULL;
    ASTNode *arg=node->data.call.args;
    unsigned count=0; for (ASTNode *it=arg; it; it=it->next) ++count;
    if (count>1 || (propagate && count!=1) || (none && count)) {
        still_error(STILL_E_ARITY,node,"%s received the wrong number of arguments",name); exit(1);
    }
    if (!propagate) {
        Type *type=wky_concrete_type(c,node->data_type);
        if (!type && some && arg) {
            type=arena_alloc(c->arena,sizeof(*type)); type->kind=TYPE_OPTION; type->inner=wky_expr_type(c,arg);
        }
        if (!type || ((some || none) ? type->kind!=TYPE_OPTION : type->kind!=TYPE_RESULT)) {
            still_error(STILL_E_TYPE,node,"%s requires a matching option<T> or result<T, E> context",name); exit(1);
        }
        Type *payload=err ? type->error : type->inner;
        if (arg) wky_literal_context(c,arg,payload);
        LLVMValueRef value=arg ? codegen_expr(c,arg) : NULL;
        node->data_type=type;
        return wky_result_construct(c,node,type,none || err,value,wky_expr_type(c,arg));
    }
    Type *type=wky_concrete_type(c,wky_expr_type(c,arg));
    if (!type || (type->kind!=TYPE_OPTION && type->kind!=TYPE_RESULT)) {
        still_error(STILL_E_TYPE,node,"try expects an option<T> or result<T, E>"); exit(1);
    }
    LLVMValueRef result=codegen_expr(c,arg);
    LLVMTypeRef tagged=get_llvm_type(c,type);
    LLVMValueRef slot=create_entry_block_alloca(c,tagged,"try_result");
    LLVMBuildStore(c->builder,result,slot);
    if (wky_contains_managed(c,type,1)) wky_memory_defer_value(c,slot,type);
    LLVMValueRef tag=LLVMBuildExtractValue(c->builder,result,0,"try_tag");
    emit_check_or_trap(c,node,LLVMBuildICmp(c->builder,LLVMIntULE,tag,
        LLVMConstInt(LLVMTypeOf(tag),1,0),"valid_result"),"invalid fallible value tag");
    LLVMBasicBlockRef success=wky_append_block(c->current_func,"try_ok");
    LLVMBasicBlockRef failure=wky_append_block(c->current_func,"try_error");
    LLVMValueRef branch=LLVMBuildCondBr(c->builder,LLVMBuildICmp(c->builder,LLVMIntEQ,tag,
        LLVMConstNull(LLVMTypeOf(tag)),"try_success"),success,failure);
    set_branch_weights(c,branch,2000,1);
    LLVMPositionBuilderAtEnd(c->builder,failure);
    Type *error=type->error;
    LLVMValueRef error_value=error && error->kind!=TYPE_VOID ?
        LLVMBuildLoad2(c->builder,get_llvm_type(c,error),payload_slot(c,slot,type,error),"try_error_value") : NULL;
    if (wky_contains_managed(c,type,1)) LLVMBuildStore(c->builder,LLVMConstNull(tagged),slot);
    wky_propagate_failure(c,node,error_value,error);
    LLVMPositionBuilderAtEnd(c->builder,success);
    LLVMValueRef value=type->inner->kind!=TYPE_VOID ?
        LLVMBuildLoad2(c->builder,get_llvm_type(c,type->inner),payload_slot(c,slot,type,type->inner),"try_value") :
        LLVMConstInt(LLVMInt32TypeInContext(c->context),0,0);
    if (wky_contains_managed(c,type,1)) LLVMBuildStore(c->builder,LLVMConstNull(tagged),slot);
    node->data_type=type->inner;
    return value;
}
