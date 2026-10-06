#include "codegen_internal.h"

static LLVMTypeRef integer(StillCompiler *c) { return LLVMInt64TypeInContext(c->context); }
static LLVMValueRef number(StillCompiler *c, uint64_t n) { return LLVMConstInt(integer(c),n,0); }
static LLVMValueRef field(StillCompiler *c, Type *type, LLVMValueRef address, unsigned index) {
    return LLVMBuildStructGEP2(c->builder,get_llvm_type(c,type),address,index,"channel_field");
}
static LLVMValueRef load(StillCompiler *c, Type *type, LLVMValueRef address, unsigned index) {
    LLVMTypeRef record=get_llvm_type(c,type);
    return LLVMBuildLoad2(c->builder,LLVMStructGetTypeAtIndex(record,index),
        field(c,type,address,index),"channel_state");
}

LLVMValueRef wky_channel_builtin(StillCompiler *c, ASTNode *n, const char *name) {
    int make=!strcmp(name,"make_chan"), close=!strcmp(name,"close_chan");
    if (!make && !close) return NULL;
    ASTNode *arg=n->data.call.args;
    if (!arg || arg->next) { still_error(STILL_E_ARITY,n,"%s expects one argument",name); exit(1); }
    Type *type=wky_concrete_type(c,make ? n->data_type : wky_expr_type(c,arg));
    if (!type || type->kind!=TYPE_CHAN) { still_error(STILL_E_TYPE,n,"%s requires a chan<T> context",name); exit(1); }
    if (close) {
        LLVMValueRef address=get_address(c,arg,NULL);
        LLVMBuildStore(c->builder,LLVMConstInt(LLVMInt1TypeInContext(c->context),1,0),field(c,type,address,5));
        return LLVMConstInt(LLVMInt32TypeInContext(c->context),0,0);
    }
    LLVMValueRef capacity=codegen_expr(c,arg);
    if (LLVMGetTypeKind(LLVMTypeOf(capacity))!=LLVMIntegerTypeKind) { still_error(STILL_E_TYPE,n,"channel capacity must be an integer"); exit(1); }
    capacity=coerce_value(c,capacity,wky_expr_type(c,arg),integer(c),NULL);
    emit_check_or_trap(c,n,LLVMBuildICmp(c->builder,LLVMIntUGT,capacity,number(c,0),"positive_capacity"),"channel capacity must be positive");
    emit_check_or_trap(c,n,LLVMBuildICmp(c->builder,LLVMIntULE,capacity,number(c,UINT64_C(1)<<62),"channel_size"),"channel capacity is too large");
    LLVMValueRef mask=LLVMBuildSub(c->builder,capacity,number(c,1),"ring_mask");
    for (unsigned shift=1; shift<64; shift*=2)
        mask=LLVMBuildOr(c->builder,mask,LLVMBuildLShr(c->builder,mask,number(c,shift),"ring_round"),"ring_round_up");
    LLVMValueRef count=LLVMBuildAdd(c->builder,mask,number(c,1),"ring_slots");
    Type buffer={.kind=TYPE_OWNER,.inner=type->inner};
    ASTNode literal={.type=NODE_VAR_REF,.data_type=&(Type){.kind=TYPE_U64}};
    ASTNode fake_binding={.type=NODE_VAR_DECL,.data_type=literal.data_type};
    fake_binding.data.var_decl.name="__channel_slots"; literal.data.var_ref.name=fake_binding.data.var_decl.name;
    LLVMValueRef count_slot=create_entry_block_alloca(c,integer(c),"ring_capacity");
    LLVMBuildStore(c->builder,count,count_slot);
    Scope *saved=c->scope_stack; scope_push(c,fake_binding.data.var_decl.name,count_slot,integer(c),&fake_binding);
    ASTNode allocate={.type=NODE_CALL,.data_type=&buffer,.line=n->line}; allocate.data.call.args=&literal;
    LLVMValueRef storage=wky_memory_builtin(c,&allocate,"own"); c->scope_stack=saved;
    // scope_push retains AST pointers for warnings; this compiler-only binding
    // opts out and has no lexical use after allocation.
    c->function_locals=c->function_locals->all_next;
    LLVMValueRef value=LLVMConstNull(get_llvm_type(c,type));
    value=LLVMBuildInsertValue(c->builder,value,storage,0,"channel_buffer");
    value=LLVMBuildInsertValue(c->builder,value,capacity,1,"channel_capacity");
    return LLVMBuildInsertValue(c->builder,value,mask,4,"channel_mask");
}

static void yield(StillCompiler *c, ASTNode *n) {
    if (!c->in_coroutine) {
        emit_check_or_trap(c,n,LLVMConstNull(LLVMInt1TypeInContext(c->context)),"channel operation would block outside a coroutine");
        return;
    }
    LLVMValueRef save=LLVMBuildCall2(c->builder,c->coro_save_type,c->coro_save,&c->current_coro_hdl,1,"channel_save");
    LLVMValueRef suspend=LLVMBuildCall2(c->builder,c->coro_suspend_type,c->coro_suspend,
        (LLVMValueRef[]){save,LLVMConstNull(LLVMInt1TypeInContext(c->context))},2,"channel_yield");
    LLVMBasicBlockRef resume=wky_append_block(c->current_func,"channel_resume");
    LLVMValueRef dispatch=LLVMBuildSwitch(c->builder,suspend,c->coro_suspend_block,2);
    LLVMAddCase(dispatch,LLVMConstInt(LLVMInt8TypeInContext(c->context),0,0),resume);
    LLVMAddCase(dispatch,LLVMConstInt(LLVMInt8TypeInContext(c->context),1,0),wky_coro_cancel_block(c));
    LLVMPositionBuilderAtEnd(c->builder,resume);
}

LLVMValueRef wky_channel_operation(StillCompiler *c, ASTNode *n) {
    ASTNode *channel=n->type==NODE_SEND ? n->data.send.chan : n->data.recv.chan;
    LLVMValueRef container=NULL, address=wky_memory_lvalue(c,channel,NULL,&container);
    if (!address) address=get_address(c,channel,NULL);
    return wky_channel_operation_at(c,n,address,container);
}

LLVMValueRef wky_channel_validate(StillCompiler *c, ASTNode *n, Type *type,
                                   LLVMValueRef address, LLVMValueRef container) {
    address=wky_memory_write_address(c,address,get_llvm_type(c,type),container);
    LLVMValueRef buffer=load(c,type,address,0);
    emit_check_or_trap(c,n,LLVMBuildIsNotNull(c->builder,LLVMBuildExtractValue(c->builder,buffer,0,"channel_identity"),"allocated_channel"),
                       "channel is not allocated");
    return address;
}

LLVMValueRef wky_channel_operation_at(StillCompiler *c, ASTNode *n,
                                       LLVMValueRef address, LLVMValueRef parent) {
    int send=n->type==NODE_SEND;
    ASTNode *channel=send ? n->data.send.chan : n->data.recv.chan;
    Type *type=wky_concrete_type(c,wky_expr_type(c,channel));
    if (!type || type->kind!=TYPE_CHAN) { still_error(STILL_E_TYPE,n,"channel operation requires chan<T>"); exit(1); }
    if (!address) { still_error(STILL_E_TYPE,n,"bind the channel before sending or receiving"); exit(1); }
    LLVMTypeRef element=get_llvm_type(c,type->inner);
    Type buffer={.kind=TYPE_OWNER,.inner=type->inner};
    int owning=wky_contains_managed(c,type->inner,1);
    DeferFrame *saved=c->defer_stack;
    LLVMValueRef pending=NULL, value=NULL;
    if (send) {
        ASTNode *input=n->data.send.value;
        wky_literal_context(c,input,type->inner);
        value=coerce_value(c,codegen_expr(c,input),wky_expr_type(c,input),element,type->inner);
        if (owning) {
            pending=create_entry_block_alloca(c,element,"pending_message");
            LLVMBuildStore(c->builder,value,pending); wky_memory_defer_value(c,pending,type->inner);
        }
    }
    LLVMBasicBlockRef check=wky_append_block(c->current_func,"channel_check");
    LLVMBasicBlockRef blocked=wky_append_block(c->current_func,"channel_blocked");
    LLVMBasicBlockRef ready=wky_append_block(c->current_func,"channel_ready");
    LLVMBuildBr(c->builder,check); LLVMPositionBuilderAtEnd(c->builder,check);
    address=wky_channel_validate(c,n,type,address,parent);
    LLVMValueRef count=load(c,type,address,3), closed=load(c,type,address,5);
    if (send) emit_check_or_trap(c,n,LLVMBuildNot(c->builder,closed,"open_channel"),"send on a closed channel");
    LLVMValueRef unavailable=LLVMBuildICmp(c->builder,send ? LLVMIntUGE : LLVMIntEQ,count,
        send ? load(c,type,address,1) : number(c,0),"channel_unavailable");
    LLVMValueRef branch=LLVMBuildCondBr(c->builder,unavailable,blocked,ready); set_branch_weights(c,branch,1,99);
    LLVMPositionBuilderAtEnd(c->builder,blocked);
    if (!send) emit_check_or_trap(c,n,LLVMBuildNot(c->builder,closed,"open_channel"),"receive on an empty closed channel");
    yield(c,n); LLVMBuildBr(c->builder,check);
    LLVMPositionBuilderAtEnd(c->builder,ready);
    LLVMValueRef head=load(c,type,address,2), mask=load(c,type,address,4);
    count=load(c,type,address,3);
    LLVMValueRef index=LLVMBuildAnd(c->builder,send ? LLVMBuildAdd(c->builder,head,count,"ring_end") : head,mask,"ring_index");
    LLVMValueRef buf_slot=field(c,type,address,0);
    LLVMValueRef container=LLVMBuildLoad2(c->builder,get_llvm_type(c,&buffer),buf_slot,"ring_owner");
    ASTNode buf_binding={.type=NODE_VAR_DECL,.data_type=&buffer}; buf_binding.data.var_decl.name="__channel_ring";
    ASTNode buf_expr={.type=NODE_VAR_REF,.data_type=&buffer}; buf_expr.data.var_ref.name=buf_binding.data.var_decl.name;
    Type index_type={.kind=TYPE_U64}; ASTNode index_binding={.type=NODE_VAR_DECL,.data_type=&index_type}; index_binding.data.var_decl.name="__channel_index";
    ASTNode index_expr={.type=NODE_VAR_REF,.data_type=&index_type}; index_expr.data.var_ref.name=index_binding.data.var_decl.name;
    LLVMValueRef index_slot=create_entry_block_alloca(c,integer(c),"channel_index"); LLVMBuildStore(c->builder,index,index_slot);
    Scope *saved_scope=c->scope_stack, *saved_locals=c->function_locals;
    scope_push(c,buf_binding.data.var_decl.name,buf_slot,get_llvm_type(c,&buffer),&buf_binding);
    scope_push(c,index_binding.data.var_decl.name,index_slot,integer(c),&index_binding);
    LLVMValueRef slot=wky_memory_address(c,n,&buf_expr,&index_expr,NULL,NULL);
    c->scope_stack=saved_scope; c->function_locals=saved_locals;
    if (send) {
        if (owning) {
            value=LLVMBuildLoad2(c->builder,element,pending,"message");
            if (wky_is_owner(type->inner)) wky_memory_store_owner(c,slot,value,container,type->inner);
            else wky_memory_store_value(c,slot,value,type->inner,container);
            LLVMBuildStore(c->builder,LLVMConstNull(element),pending);
        } else LLVMBuildStore(c->builder,value,slot);
        LLVMBuildStore(c->builder,LLVMBuildAdd(c->builder,count,number(c,1),"channel_increment"),field(c,type,address,3));
        c->defer_stack=saved;
        n->data_type=arena_alloc(c->arena,sizeof(Type)); n->data_type->kind=TYPE_VOID;
        return LLVMConstInt(LLVMInt32TypeInContext(c->context),0,0);
    }
    if (owning) {
        value=wky_memory_take_value(c,n,slot,type->inner,container);
    } else value=LLVMBuildLoad2(c->builder,element,slot,"received_message");
    LLVMBuildStore(c->builder,LLVMBuildAdd(c->builder,head,number(c,1),"channel_advance"),field(c,type,address,2));
    LLVMBuildStore(c->builder,LLVMBuildSub(c->builder,count,number(c,1),"channel_decrement"),field(c,type,address,3));
    n->data_type=type->inner;
    return value;
}
