#include "codegen_internal.h"

/* Resolve under the caller's types, then clone a concrete instance. Nested
 * instantiation never replaces a caller's T map or rewrites a shared template.
 * A structural key distinguishes owner/ref/pointer and fixed-array extents. */
static void return_type(StillCompiler *c,ASTNode *call,LLVMValueRef fn) {
    for (FunctionSignature *s=c->function_signatures; s; s=s->next)
        if (s->function==fn) { call->data_type=s->return_type; return; }
}
static Type *argument_type(StillCompiler *c,ASTNode *arg) {
    Type *type=wky_expr_type(c,arg);
    if ((!type || (type->kind==TYPE_STRUCT && type->name && !strcmp(type->name,"T"))) &&
        arg->type==NODE_CALL && arg->data.call.callee->type==NODE_VAR_REF) {
        LLVMValueRef fn=wky_generic_function(c,arg,arg->data.call.callee->data.var_ref.name);
        if (fn) type=wky_expr_type(c,arg);
    }
    return wky_concrete_type(c,type);
}
static int has_parameter(Type *type) {
    return type && ((type->kind==TYPE_STRUCT && type->name && !strcmp(type->name,"T")) ||
        has_parameter(type->inner) || has_parameter(type->error));
}
static Type *infer_parameter(StillCompiler *c, ASTNode *argument, Type *formal, Type *actual) {
    if (!has_parameter(formal)) return NULL;
    if (formal->kind==TYPE_STRUCT && formal->name && !strcmp(formal->name,"T")) {
        if (!actual || has_parameter(actual)) { still_error(STILL_E_TYPE,argument,"cannot infer a concrete generic type"); exit(1); }
        return actual;
    }
    if (!actual || (formal->kind!=actual->kind && !(formal->kind==TYPE_SLICE && actual->kind==TYPE_ARRAY)) ||
        (formal->kind==TYPE_ARRAY && formal->array_len!=actual->array_len)) {
        still_error(STILL_E_TYPE,argument,"generic argument has incompatible type structure"); exit(1);
    }
    Type *found=infer_parameter(c,argument,formal->inner,actual->inner);
    return found ? found : infer_parameter(c,argument,formal->error,actual->error);
}
LLVMValueRef wky_generic_function(StillCompiler *c,ASTNode *call,const char *name) {
    ASTNode *template=NULL;
    for (int i=0; i<c->generic_fn_count; ++i)
        if (!strcmp(c->generic_fns[i]->data.func.name,name)) { template=c->generic_fns[i]; break; }
    if (!template) return NULL;
    ASTNode *parameters[32], *arguments[32]={0};
    unsigned count=0,position=0,provided=0;
    for (ASTNode *p=template->data.func.args; p; p=p->next) {
        if (count==32) { still_error(STILL_E_ARITY,call,"generic functions support up to 32 parameters"); exit(1); }
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
            still_error(STILL_E_ARGS,call,"unknown, duplicate, or excess generic argument"); exit(1);
        }
        arguments[index]=a; ++provided;
    }
    if (provided!=count) { still_error(STILL_E_ARITY,call,"generic call requires %u arguments",count); exit(1); }
    char *names[8]; Type *types[8]; unsigned bindings=0;
    for (unsigned i=0; i<count; ++i) {
        Type *inferred=infer_parameter(c,arguments[i],parameters[i]->data_type,argument_type(c,arguments[i]));
        /* First occurrence selects T; checked argument conversion validates
         * subsequent operands against that concrete signature. */
        if (inferred && !bindings) { names[bindings]="T"; types[bindings++]=inferred; }
    }
    if (!bindings) { still_error(STILL_E_TYPE,call,"generic type must be inferred from an argument"); exit(1); }
    char *keys[8]; size_t length=strlen(name)+32;
    for (unsigned i=0; i<bindings; ++i) { keys[i]=wky_type_key(c,types[i]); length+=strlen(keys[i])+1; }
    char *mangled=arena_alloc(c->arena,length), *end=mangled;
    end+=sprintf(end,"__wky_generic_%s__",name);
    for (unsigned i=0; i<bindings; ++i) end+=sprintf(end,"%s_",keys[i]);
    LLVMValueRef fn=LLVMGetNamedFunction(c->module,mangled);
    if (!fn) {
        ASTNode *instance=wky_clone_ast(c->arena,template,bindings,names,types,NULL,NULL);
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
        still_di_set_location(c,call->line);
        fn=LLVMGetNamedFunction(c->module,mangled);
        if (!fn) { still_error(STILL_E_SEMANTIC,call,"generic specialization failed"); exit(1); }
    }
    return_type(c,call,fn);
    return fn;
}
