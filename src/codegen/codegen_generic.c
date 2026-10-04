#include "codegen_internal.h"

/* Resolve under the caller's types, then clone a concrete instance. Nested
 * instantiation never replaces a caller's T map or rewrites a shared template.
 * A structural key distinguishes owner/ref/pointer and fixed-array extents. */
static size_t key_size(Type *t) {
    return t ? 64+(t->name ? strlen(t->name) : 0)+key_size(t->inner) : 4;
}
static char *key_write(char *out,Type *t) {
    if (!t) { memcpy(out,"end",4); return out+3; }
    size_t length=t->name ? strlen(t->name) : 0;
    out+=sprintf(out,"k%d_a%ld_n%zu_",t->kind,t->array_len,length);
    if (length) { memcpy(out,t->name,length); out+=length; }
    *out++='_';
    return key_write(out,t->inner);
}
static void return_type(KawaCompiler *c,ASTNode *call,LLVMValueRef fn) {
    for (FunctionSignature *s=c->function_signatures; s; s=s->next)
        if (s->function==fn) { call->data_type=s->return_type; return; }
}
static Type *argument_type(KawaCompiler *c,ASTNode *arg) {
    Type *type=kawa_expr_type(c,arg);
    if ((!type || (type->kind==TYPE_STRUCT && type->name && !strcmp(type->name,"T"))) &&
        arg->type==NODE_CALL && arg->data.call.callee->type==NODE_VAR_REF) {
        LLVMValueRef fn=kawa_generic_function(c,arg,arg->data.call.callee->data.var_ref.name);
        if (fn) type=kawa_expr_type(c,arg);
    }
    return kawa_concrete_type(c,type);
}
LLVMValueRef kawa_generic_function(KawaCompiler *c,ASTNode *call,const char *name) {
    ASTNode *template=NULL;
    for (int i=0; i<c->generic_fn_count; ++i)
        if (!strcmp(c->generic_fns[i]->data.func.name,name)) { template=c->generic_fns[i]; break; }
    if (!template) return NULL;
    ASTNode *parameters[32], *arguments[32]={0};
    unsigned count=0,position=0,provided=0;
    for (ASTNode *p=template->data.func.args; p; p=p->next) {
        if (count==32) { kerr(KAWA_E_ARITY,call,"generic functions support up to 32 parameters"); exit(1); }
        parameters[count++]=p;
    }
    for (ASTNode *a=call->data.call.args; a; a=a->next) {
        unsigned index=count;
        if (a->has_arg_label) {
            for (unsigned i=0; i<count; ++i)
                if (!strcmp(a->arg_label,parameters[i]->data.var_decl.name)) { index=i; break; }
        } else {
            while (position<count && arguments[position]) ++position;
            index=position++;
        }
        if (index>=count || arguments[index]) {
            kerr(KAWA_E_ARGS,call,"unknown, duplicate, or excess generic argument"); exit(1);
        }
        arguments[index]=a; ++provided;
    }
    if (provided!=count) { kerr(KAWA_E_ARITY,call,"generic call requires %u arguments",count); exit(1); }
    char *names[8]; Type *types[8]; unsigned bindings=0;
    for (unsigned i=0; i<count; ++i) {
        Type *parameter=parameters[i]->data_type, *actual=argument_type(c,arguments[i]);
        while (parameter && parameter->inner) {
            if (!actual || (actual->kind!=parameter->kind &&
                !(parameter->kind==TYPE_SLICE && actual->kind==TYPE_ARRAY)) ||
                (parameter->kind==TYPE_ARRAY && parameter->array_len!=actual->array_len)) {
                kerr(KAWA_E_TYPE,arguments[i],"generic argument has incompatible type structure"); exit(1);
            }
            parameter=parameter->inner; actual=actual->inner;
        }
        if (!parameter || parameter->kind!=TYPE_STRUCT || !parameter->name || strcmp(parameter->name,"T")) continue;
        if (!actual || (actual->kind==TYPE_STRUCT && actual->name && !strcmp(actual->name,"T"))) {
            kerr(KAWA_E_TYPE,arguments[i],"cannot infer a concrete generic type"); exit(1);
        }
        /* First declaration of T sets its type; ordinary checked argument
         * conversion handles later T operands, just like concrete functions. */
        if (!bindings) { names[bindings]=parameter->name; types[bindings++]=actual; }
    }
    if (!bindings) { kerr(KAWA_E_TYPE,call,"generic type must be inferred from an argument"); exit(1); }
    size_t length=strlen(name)+64;
    for (unsigned i=0; i<bindings; ++i) length+=key_size(types[i]);
    char *mangled=arena_alloc(c->arena,length), *end=mangled;
    end+=sprintf(end,"__kawa_generic_%s__",name);
    for (unsigned i=0; i<bindings; ++i) end=key_write(end,types[i]);
    *end=0;
    LLVMValueRef fn=LLVMGetNamedFunction(c->module,mangled);
    if (!fn) {
        ASTNode *instance=kawa_clone_ast(c->arena,template,bindings,names,types,NULL,NULL);
        instance->data.func.name=mangled;
        LLVMBasicBlockRef saved_block=LLVMGetInsertBlock(c->builder);
        LLVMValueRef saved_function=c->current_func;
        LLVMTypeRef saved_return=c->current_ret_type;
        Type *saved_return_ast=c->current_ret_node_type;
        Scope *saved_scope=c->scope_stack;
        int saved_coroutine=c->in_coroutine, saved_unreachable=c->warned_unreachable;
        c->in_coroutine=0;
        LLVMSetCurrentDebugLocation2(c->builder,NULL);
        codegen_func_decl(c,instance,NULL);
        LLVMPositionBuilderAtEnd(c->builder,saved_block);
        c->current_func=saved_function;
        c->current_ret_type=saved_return;
        c->current_ret_node_type=saved_return_ast;
        c->scope_stack=saved_scope;
        c->in_coroutine=saved_coroutine;
        c->warned_unreachable=saved_unreachable;
        kawa_di_set_location(c,call->line);
        fn=LLVMGetNamedFunction(c->module,mangled);
        if (!fn) { kerr(KAWA_E_SEMANTIC,call,"generic specialization failed"); exit(1); }
    }
    return_type(c,call,fn);
    return fn;
}
