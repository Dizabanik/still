#include "codegen_internal.h"

/* const qualifies storage, not a pointer's referent. Inline aggregate fields
 * belong to that storage; an owner/ref/pointer index crosses an indirection.
 * Address-taking of immutable storage is rejected because raw pointer types
 * cannot preserve its qualifier. */
void wky_require_mutable(StillCompiler *c, ASTNode *target) {
    if (!target) return;
    if (target->type==NODE_VAR_REF) {
        Scope *s=scope_find(c,target->data.var_ref.name);
        if (s && s->node && s->node->type==NODE_VAR_DECL &&
            (s->node->data.var_decl.is_const || s->node->data.var_decl.is_orbit)) {
            still_error(STILL_E_TYPE,target,"cannot modify immutable binding `%s`",s->name);
            exit(1);
        }
    } else if (target->type==NODE_MEMBER_ACCESS) {
        ASTNode *object=target->data.member_access.object;
        Type *type=wky_expr_type(c,object);
        if (type && (type->kind==TYPE_CHAN || type->kind==TYPE_SLICE || type->kind==TYPE_ARRAY) &&
            (!strcmp(target->data.member_access.member,"len") || type->kind==TYPE_CHAN)) {
            still_error(STILL_E_TYPE,target,"container metadata is read-only"); exit(1);
        }
        if (!type || (type->kind!=TYPE_PTR && type->kind!=TYPE_AMP && !wky_is_managed(type)))
            wky_require_mutable(c,object);
    } else if (target->type==NODE_INDEX) {
        Type *type=wky_expr_type(c,target->data.index.object);
        if (!type || type->kind==TYPE_ARRAY || type->kind==TYPE_STRUCT)
            wky_require_mutable(c,target->data.index.object);
    }
}

void wky_require_unsafe(StillCompiler *c, ASTNode *node, const char *operation) {
    if (c->unsafe_depth) return;
    ASTNode location={.line=c->source_line}; if (!node) node=&location;
    still_diag_help("put the raw operation inside an `unsafe { ... }` block; use owner<T>/ref<T> for checked storage");
    still_error(STILL_E_TYPE,node,"%s requires an unsafe block",operation);
    exit(1);
}
void wky_check_call_safety(StillCompiler *c, ASTNode *node, LLVMValueRef function) {
    if (LLVMGetStringAttributeAtIndex(function,LLVMAttributeFunctionIndex,"wky.unsafe",10))
        wky_require_unsafe(c,node,"calling an unsafe function");
}

static int orbit_pure_call(StillCompiler *c, const char *name) {
    int found=0;
    for (ASTNode *node=c->program_root->next; node; node=node->next) {
        ASTNode *method=node->type==NODE_IMPL_BLOCK ? node->data.impl.methods : node;
        for (; method; method=node->type==NODE_IMPL_BLOCK ? method->next : NULL) {
            if (method->type!=NODE_FUNC_DECL || strcmp(method->data.func.name,name)) continue;
            found=1;
            if (!method->data.func.is_pure) return 0;
        }
    }
    return found;
}
static int orbit_expression(StillCompiler *c, ASTNode *n) {
    if (!n) return 0;
    switch (n->type) {
    case NODE_LITERAL: case NODE_STRING_LIT: case NODE_VAR_REF: return 1;
    case NODE_BINARY_OP:
        return orbit_expression(c,n->data.bin_op.left) && orbit_expression(c,n->data.bin_op.right);
    case NODE_TERNARY:
        return orbit_expression(c,n->data.ternary.cond) && orbit_expression(c,n->data.ternary.then_expr) &&
            orbit_expression(c,n->data.ternary.else_expr);
    case NODE_MEMBER_ACCESS: return orbit_expression(c,n->data.member_access.object);
    case NODE_INDEX: {
        Type *base=index_base_struct_type(c,n->data.index.object);
        if (base && impl_has_method(c,base->name,"self_index")) {
            size_t size=strlen(base->name)+sizeof("__self_index");
            char *name=arena_alloc(c->arena,size);
            snprintf(name,size,"%s__self_index",base->name);
            if (!orbit_pure_call(c,name)) return 0;
        }
        return orbit_expression(c,n->data.index.object) && orbit_expression(c,n->data.index.index);
    }
    case NODE_DEREF: return orbit_expression(c,n->data.deref.expr);
    case NODE_CAST: return orbit_expression(c,n->data.cast.val);
    case NODE_CALL: {
        ASTNode *callee=n->data.call.callee;
        if (!callee || callee->type!=NODE_VAR_REF) return 0;
        const char *name=callee->data.var_ref.name;
        int pure=!strcmp(name,"mem_len") || !strcmp(name,"mem_capacity") || !strcmp(name,"allocated") || orbit_pure_call(c,name);
        if (!pure) return 0;
        for (ASTNode *a=n->data.call.args; a; a=a->next) if (!orbit_expression(c,a)) return 0;
        return 1;
    }
    default: return 0;
    }
}

void wky_check_orbit(StillCompiler *c, ASTNode *node) {
    if (!orbit_expression(c,node->data.var_decl.init) || wky_contains_managed(c,node->data_type,1)) {
        still_error(STILL_E_TYPE,node,"orbit requires a side-effect-free expression producing a copyable value");
        exit(1);
    }
}

/* A derived binding evaluates in its declaration's environment at each read.
 * It observes alias writes and calls without dirty flags or mutation hooks.
 * LLVM can reuse/hoist reads when it proves the dependencies unchanged. */
LLVMValueRef wky_orbit_value(StillCompiler *c, Scope *binding) {
    Scope *saved=c->scope_stack;
    c->scope_stack=binding->orbit_scope;
    ASTNode *init=binding->node->data.var_decl.init;
    LLVMValueRef value=codegen_expr(c,init);
    value=coerce_value(c,value,wky_expr_type(c,init),binding->type,binding->node->data_type);
    c->scope_stack=saved;
    return value;
}

/* Check declarations even in unreachable code. This lexical pass emits no IR
 * and does not run cleanup/move analysis on paths that cannot execute. */
static void semantic_node(StillCompiler *c, ASTNode *n);
static void semantic_binding(StillCompiler *c, ASTNode *decl) {
    Scope *entry=arena_alloc(c->arena,sizeof(*entry));
    entry->name=decl->data.var_decl.name; entry->node=decl;
    entry->next=c->scope_stack; c->scope_stack=entry;
}
static void semantic_scope(StillCompiler *c, ASTNode *n) {
    Scope *saved=c->scope_stack;
    semantic_node(c,n);
    c->scope_stack=saved;
}
static void semantic_list(StillCompiler *c, ASTNode *n) {
    for (; n; n=n->next) semantic_node(c,n);
}
static void semantic_node(StillCompiler *c, ASTNode *n) {
    if (!n) return;
    switch (n->type) {
    case NODE_BLOCK: {
        Scope *saved=c->scope_stack;
        semantic_list(c,n->data.block.stmts); c->scope_stack=saved;
        break;
    }
    case NODE_VAR_DECL:
        semantic_node(c,n->data.var_decl.init);
        if (n->data.var_decl.is_orbit) wky_check_orbit(c,n);
        semantic_binding(c,n); break;
    case NODE_ASSIGN:
        wky_require_mutable(c,n->data.assign.target);
        semantic_node(c,n->data.assign.target); semantic_node(c,n->data.assign.value); break;
    case NODE_AMP:
        wky_require_mutable(c,n->data.deref.expr);
        semantic_node(c,n->data.deref.expr); break;
    case NODE_DEREF: semantic_node(c,n->data.deref.expr); break;
    case NODE_CALL: {
        ASTNode *callee=n->data.call.callee, *arg=n->data.call.args;
        if (callee && callee->type==NODE_VAR_REF && arg) {
            const char *name=callee->data.var_ref.name;
            if (!strcmp(name,"move") || !strcmp(name,"release") || !strcmp(name,"cancel") ||
                !strcmp(name,"resize") || !strcmp(name,"try_resize")) wky_require_mutable(c,arg);
        }
        semantic_list(c,arg); break;
    }
    case NODE_BINARY_OP: semantic_node(c,n->data.bin_op.left); semantic_node(c,n->data.bin_op.right); break;
    case NODE_TERNARY:
        semantic_node(c,n->data.ternary.cond); semantic_node(c,n->data.ternary.then_expr); semantic_node(c,n->data.ternary.else_expr); break;
    case NODE_MEMBER_ACCESS: semantic_node(c,n->data.member_access.object); break;
    case NODE_INDEX: semantic_node(c,n->data.index.object); semantic_node(c,n->data.index.index); break;
    case NODE_SLICE_INDEX:
        semantic_node(c,n->data.slice_index.object); semantic_node(c,n->data.slice_index.start); semantic_node(c,n->data.slice_index.end); break;
    case NODE_CAST: semantic_node(c,n->data.cast.val); break;
    case NODE_STRUCT_LITERAL:
        for (StructInitItem *item=n->data.struct_lit.items; item; item=item->next) {
            semantic_node(c,item->value); semantic_node(c,item->spread_from);
        }
        break;
    case NODE_IF:
        semantic_node(c,n->data.if_stmt.cond); semantic_scope(c,n->data.if_stmt.then_block); semantic_scope(c,n->data.if_stmt.else_block); break;
    case NODE_WHILE: semantic_node(c,n->data.while_stmt.cond); semantic_scope(c,n->data.while_stmt.body); break;
    case NODE_FOR: {
        Scope *saved=c->scope_stack;
        semantic_list(c,n->data.for_stmt.init); semantic_node(c,n->data.for_stmt.cond);
        semantic_scope(c,n->data.for_stmt.body); semantic_node(c,n->data.for_stmt.step); c->scope_stack=saved;
        break;
    }
    case NODE_RETURN: semantic_node(c,n->data.ret_stmt.expr); break;
    case NODE_PRESS: semantic_node(c,n->data.press.target); break;
    case NODE_DEFER: semantic_scope(c,n->data.defer.stmt); break;
    case NODE_STABLE:
        semantic_node(c,n->data.stable.reference); semantic_scope(c,n->data.stable.body); semantic_scope(c,n->data.stable.otherwise); break;
    case NODE_MATCH:
        semantic_node(c,n->data.match_stmt.target);
        for (ASTNode *arm=n->data.match_stmt.arms; arm; arm=arm->next) {
            Scope *saved=c->scope_stack;
            for (ASTNode *b=arm->data.match_arm.bindings; b; b=b->next) semantic_binding(c,b);
            semantic_node(c,arm->data.match_arm.body); c->scope_stack=saved;
        }
        break;
    case NODE_FILTER: {
        semantic_scope(c,n->data.filter.try_block);
        Scope *saved=c->scope_stack;
        ASTNode error={.type=NODE_VAR_DECL,.data_type=n->data.filter.err_type}; error.data.var_decl.name=n->data.filter.err_var;
        semantic_binding(c,&error); semantic_node(c,n->data.filter.catch_block); c->scope_stack=saved;
        break;
    }
    case NODE_SELECT:
        for (struct SelectCase *item=n->data.select_stmt.cases; item; item=item->next) {
            Scope *saved=c->scope_stack; semantic_node(c,item->chan);
            if (item->var_decl) semantic_binding(c,item->var_decl);
            semantic_node(c,item->body); c->scope_stack=saved;
        }
        semantic_scope(c,n->data.select_stmt.default_body); break;
    case NODE_SWITCH:
        semantic_node(c,n->data.switch_stmt.value);
        for (ASTNode *item=n->data.switch_stmt.cases; item; item=item->next) semantic_scope(c,item->data.case_stmt.body);
        break;
    case NODE_UNSAFE_BLOCK: case NODE_UNCHECKED_BLOCK: semantic_scope(c,n->data.block.stmts); break;
    case NODE_BATCH: semantic_node(c,n->data.batch.collection); semantic_scope(c,n->data.batch.body); break;
    case NODE_SIP: semantic_node(c,n->data.sip.handle); break;
    case NODE_DROP: semantic_node(c,n->data.drop.val); break;
    case NODE_SEND: semantic_node(c,n->data.send.chan); semantic_node(c,n->data.send.value); break;
    case NODE_RECV: semantic_node(c,n->data.recv.chan); break;
    default: break;
    }
}
void wky_verify_semantics(StillCompiler *c, ASTNode *function) {
    Scope *saved=c->scope_stack;
    c->scope_stack=c->global_scope;
    for (ASTNode *arg=function->data.func.args; arg; arg=arg->next) semantic_binding(c,arg);
    semantic_node(c,function->data.func.body);
    c->scope_stack=saved;
}

/* Array decay keeps the original storage, its element type and qualifier.
 * This is shared by declarations, assignment and typed function arguments. */
LLVMValueRef wky_array_view(StillCompiler *c, ASTNode *node, Type *destination) {
    destination=wky_concrete_type(c,destination);
    Type *source=wky_expr_type(c,node);
    if (!source || source->kind!=TYPE_ARRAY || !destination ||
        (destination->kind!=TYPE_SLICE && destination->kind!=TYPE_PTR)) return NULL;
    if (!wky_types_same(wky_concrete_type(c,source->inner),destination->inner)) {
        still_error(STILL_E_TYPE,node,"array view element type must match its storage"); exit(1);
    }
    wky_require_mutable(c,node);
    LLVMValueRef container=NULL;
    LLVMTypeRef llvm=get_llvm_type(c,source);
    LLVMValueRef address=wky_memory_lvalue(c,node,NULL,&container);
    if (container) {
        still_error(STILL_E_TYPE,node,"use ref_of for a view into managed array storage"); exit(1);
    }
    if (!address) address=get_address(c,node,NULL);
    if (!address) {
        address=create_entry_block_alloca(c,llvm,"array_temporary");
        LLVMBuildStore(c->builder,codegen_expr(c,node),address);
    }
    LLVMTypeRef i64=LLVMInt64TypeInContext(c->context);
    LLVMValueRef zero=LLVMConstNull(i64);
    LLVMValueRef data=LLVMBuildGEP2(c->builder,llvm,address,(LLVMValueRef[]){zero,zero},2,"array_view");
    if (destination->kind==TYPE_PTR) return data;
    LLVMValueRef value=LLVMConstNull(get_llvm_type(c,destination));
    value=LLVMBuildInsertValue(c->builder,value,data,0,"array_view_data");
    return LLVMBuildInsertValue(c->builder,value,LLVMConstInt(i64,source->array_len,0),1,"array_view_length");
}

/* A synthetic merge after returning branches has no executable predecessor.
 * Check the actual graph before diagnosing a missing fallible return. */
int wky_block_reachable(StillCompiler *c, LLVMBasicBlockRef target) {
    unsigned count=LLVMCountBasicBlocks(c->current_func), used=1;
    LLVMBasicBlockRef *queue=arena_alloc(c->arena,sizeof(*queue)*count);
    queue[0]=LLVMGetEntryBasicBlock(c->current_func);
    for (unsigned i=0; i<used; ++i) {
        if (queue[i]==target) return 1;
        LLVMValueRef end=LLVMGetBasicBlockTerminator(queue[i]);
        if (!end) continue;
        for (unsigned j=0; j<LLVMGetNumSuccessors(end); ++j) {
            LLVMBasicBlockRef next=LLVMGetSuccessor(end,j);
            unsigned k=0; while (k<used && queue[k]!=next) ++k;
            if (k==used) queue[used++]=next;
        }
    }
    return 0;
}
