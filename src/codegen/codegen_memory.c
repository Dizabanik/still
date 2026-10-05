#include "codegen_internal.h"
static void error(KawaCompiler *c, ASTNode *n, const char *message);

int kawa_is_managed(Type *t) {
    return t && (t->kind == TYPE_OWNER || t->kind == TYPE_REF || t->kind == TYPE_ARENA);
}
int kawa_is_owner(Type *t) {
    return t && (t->kind == TYPE_OWNER || t->kind == TYPE_ARENA);
}
static int contains(KawaCompiler *c, Type *t, int owners_only, unsigned depth) {
    if (!t || depth > 64) return 0;
    t = kawa_resolve_type(c, t);
    if (owners_only ? kawa_is_owner(t) : kawa_is_managed(t)) return 1;
    if (t->kind == TYPE_REF) return 0; /* its referent is not embedded */
    if (t->kind == TYPE_STRUCT && t->name) {
        for (ASTNode *s = c->program_root ? c->program_root->next : NULL; s; s = s->next) {
            if (s->type != NODE_STRUCT_DECL || strcmp(s->data.struct_decl.name, t->name)) continue;
            for (ASTNode *f = s->data.struct_decl.fields; f; f = f->next)
                if (contains(c, f->data_type, owners_only, depth + 1)) return 1;
        }
    }
    if (t->kind == TYPE_ENUM && t->name) {
        ASTNode *en = find_enum_decl(c, t->name);
        for (EnumVariant *v = en ? en->data.enum_decl.variants : NULL; v; v = v->next)
            for (int i=0; i<v->payload_count; ++i)
                if (contains(c, v->payload_types[i], owners_only, depth+1)) return 1;
    }
    return contains(c, t->inner, owners_only, depth + 1);
}
int kawa_contains_managed(KawaCompiler *c, Type *t, int owners_only) {
    return contains(c, t, owners_only, 0);
}
Type *kawa_expr_type(KawaCompiler *c, ASTNode *n) {
    if (!n) return NULL;
    if (n->data_type) return kawa_resolve_type(c, n->data_type);
    if (n->type == NODE_VAR_REF) {
        Scope *s = scope_find(c, n->data.var_ref.name);
        if (s && s->node) n->data_type = s->node->data_type;
    }
    if (n->type==NODE_BINARY_OP) {
        Type *left=kawa_expr_type(c,n->data.bin_op.left), *right=kawa_expr_type(c,n->data.bin_op.right);
        if (left && right) {
            if (left->kind>=TYPE_BOOL && left->kind<=TYPE_F64 && right->kind>=TYPE_BOOL && right->kind<=TYPE_F64) {
                LLVMTypeRef l=get_llvm_type(c,left), r=get_llvm_type(c,right);
                if (is_fp_kind(LLVMGetTypeKind(l)) || is_fp_kind(LLVMGetTypeKind(r)))
                    n->data_type=is_fp_kind(LLVMGetTypeKind(l)) ?
                        (is_fp_kind(LLVMGetTypeKind(r)) && right->kind>left->kind ? right : left) : right;
                else {
                    unsigned lw=LLVMGetIntTypeWidth(l), rw=LLVMGetIntTypeWidth(r);
                    n->data_type=lw==rw ? (type_is_signed(c,left) ? right : left) : lw>rw ? left : right;
                }
            }
        } else n->data_type=left ? left : right;
    }
    if (n->type == NODE_INDEX || n->type == NODE_DEREF) {
        Type *base=kawa_expr_type(c,n->type==NODE_INDEX ? n->data.index.object : n->data.deref.expr);
        if (base) n->data_type=base->inner;
    }
    if (n->type == NODE_MEMBER_ACCESS) {
        Type *base=kawa_expr_type(c,n->data.member_access.object);
        if (base && base->kind==TYPE_STRUCT) {
            LLVMTypeRef type=get_llvm_type(c,base);
            for (unsigned depth=0; depth<16; ++depth) {
                StructDef *sd=find_struct_def_pub(c,type);
                if (!sd) break;
                for (int i=0; i<sd->field_count; ++i)
                    if (!strcmp(sd->fields[i].name,n->data.member_access.member)) {
                        n->data_type=sd->fields[i].ast_type;
                        return kawa_resolve_type(c,n->data_type);
                    }
                int middle,field;
                if (!try_promoted_field(c,type,n->data.member_access.member,&middle,&field)) break;
                type=LLVMStructGetTypeAtIndex(type,(unsigned)middle);
            }
        }
    }
    return kawa_resolve_type(c, n->data_type);
}
void kawa_check_value_type(KawaCompiler *c, ASTNode *n, Type *type) {
    type=kawa_resolve_type(c,type);
    if (!kawa_is_owner(type) && kawa_contains_managed(c,type,1)) {
        error(c,n,"aggregates with owned fields currently require managed heap storage");
    }
}
static LLVMTypeRef i64(KawaCompiler *c) { return LLVMInt64TypeInContext(c->context); }
static LLVMValueRef constant(KawaCompiler *c, uint64_t v) { return LLVMConstInt(i64(c), v, 0); }
static LLVMTypeRef ptr(KawaCompiler *c) { return LLVMPointerTypeInContext(c->context, 0); }
static LLVMTypeRef ref_type(KawaCompiler *c) {
    Type t = {.kind = TYPE_REF};
    return get_llvm_type(c, &t);
}
static LLVMValueRef call_runtime(KawaCompiler *c, ASTNode *node,const char *name, LLVMTypeRef ret,
                                  LLVMValueRef *args, unsigned count) {
    c->uses_memory = 1;
    LLVMValueRef fn = LLVMGetNamedFunction(c->module, name);
    if (!fn) {
        LLVMTypeRef types[8];
        for (unsigned i = 0; i < count; ++i) types[i] = LLVMTypeOf(args[i]);
        fn = LLVMAddFunction(c->module, name, LLVMFunctionType(ret, types, count, 0));
    }
    LLVMValueRef call=LLVMBuildCall2(c->builder,LLVMGlobalGetValueType(fn),fn,args,count,
                                    LLVMGetTypeKind(ret)==LLVMVoidTypeKind ? "" : "memory");
    if (strcmp(name,"__kawa_mem_metric") && strcmp(name,"__kawa_mem_budget")) {
        const char *kind=!strcmp(name,"__kawa_mem_address") ? "lifetime_and_bounds" :
            !strcmp(name,"__kawa_mem_write_address") ? "lifetime_and_extent" :
            !strcmp(name,"__kawa_mem_view") ? "subobject_view" :
            strstr(name,"pin") ? "stability" : strstr(name,"alloc") || !strcmp(name,"__kawa_mem_arena") ?
            "allocation" : strstr(name,"clone") ? "clone" : strstr(name,"resize") ?
            "resize" : "memory_operation";
        kawa_report_attach(c,call,kawa_report_site(c,node,kind,name,NULL));
    }
    return call;
}
static void error(KawaCompiler *c, ASTNode *n, const char *message) {
    kerr(KAWA_E_TYPE, n, "%s", message);
    exit(1);
}
static LLVMValueRef integer_arg(KawaCompiler *c, ASTNode *n) {
    LLVMValueRef value = codegen_expr(c, n);
    if (LLVMGetTypeKind(LLVMTypeOf(value)) != LLVMIntegerTypeKind)
        error(c, n, "memory sizes and indices must be integers");
    return coerce_value(c, value, kawa_expr_type(c, n), i64(c), NULL);
}
static LLVMValueRef element_size(KawaCompiler *c, Type *type) {
    return LLVMSizeOf(get_llvm_type(c, type));
}
static int managed_element(KawaCompiler *c, Type *t, int depth) {
    if (!t || depth>64) return 0;
    t=kawa_resolve_type(c,t);
    if (t->kind==TYPE_OWNER || t->kind==TYPE_REF ||
        (t->kind>=TYPE_BOOL && t->kind<=TYPE_F64)) return 1;
    if (t->kind==TYPE_ARRAY) return managed_element(c,t->inner,depth+1);
    if (t->kind==TYPE_STRUCT) {
        StructDef *sd=find_struct_def_pub(c,get_llvm_type(c,t));
        if (!sd) return 0;
        for (int i=0; i<sd->field_count; ++i)
            if (!managed_element(c,sd->fields[i].ast_type,depth+1)) return 0;
        return 1;
    }
    return 0;
}
LLVMValueRef kawa_memory_value(KawaCompiler *c, ASTNode *n) {
    if (!kawa_is_managed(kawa_expr_type(c, n))) error(c, n, "expected a managed owner or reference");
    if (n->type == NODE_VAR_REF || n->type == NODE_MEMBER_ACCESS || n->type == NODE_INDEX || n->type == NODE_DEREF) {
        LLVMValueRef slot = get_address(c, n, NULL);
        return LLVMBuildLoad2(c->builder, ref_type(c), slot, "reference");
    }
    if (kawa_is_owner(n->data_type))
        error(c, n, "bind the temporary owner before borrowing it");
    return codegen_expr(c, n);
}
static LLVMValueRef spill(KawaCompiler *c, LLVMValueRef value) {
    LLVMValueRef slot = create_entry_block_alloca(c, LLVMTypeOf(value), "memory_arg");
    LLVMBuildStore(c->builder, value, slot);
    return slot;
}
void kawa_memory_cleanup(KawaCompiler *c, LLVMValueRef slot, int unpin) {
    call_runtime(c,NULL, unpin ? "__kawa_mem_unpin" : "__kawa_mem_drop",
                 LLVMVoidTypeInContext(c->context), &slot, 1);
}
void kawa_memory_defer(KawaCompiler *c, LLVMValueRef slot, int unpin) {
    DeferFrame *d = arena_alloc(c->arena, sizeof(*d));
    d->memory_slot = slot;
    d->memory_unpin = unpin;
    d->next = c->defer_stack;
    c->defer_stack = d;
}
LLVMValueRef kawa_memory_address(KawaCompiler *c, ASTNode *n, ASTNode *base,
                                  ASTNode *index, LLVMTypeRef *out_type, LLVMValueRef *container) {
    Type *type = kawa_expr_type(c, base);
    if (!kawa_is_managed(type)) return NULL;
    if (type->kind == TYPE_ARENA || !type->inner) error(c, n, "an arena is not an indexable reference");
    LLVMTypeRef elem = get_llvm_type(c, type->inner);
    if (out_type) *out_type = elem;
    n->data_type = type->inner;
    LLVMValueRef value = kawa_memory_value(c, base);
    if (container) *container=value;
    LLVMValueRef idx = index ? integer_arg(c, index) : constant(c, 0);
    LLVMValueRef size = element_size(c, type->inner);
    /* An immutable binding within stable has one lifetime validation; callbacks
     * still cannot free/relocate its storage because the runtime is pinned. */
    if (base->type == NODE_VAR_REF) {
        Scope *s = scope_find(c, base->data.var_ref.name);
        for (StableFrame *f = c->stable_stack; f; f = f->next) {
            if (f->slot != s->val) continue;
            if (container) *container=f->reference;
            LLVMValueRef extent = LLVMBuildExtractValue(c->builder, f->reference, 3, "extent");
            LLVMValueRef length = LLVMBuildUDiv(c->builder, extent, size, "length");
            emit_check_or_trap(c, n, LLVMBuildICmp(c->builder, LLVMIntULT, idx, length, "in_bounds"),
                               "reference index out of bounds");
            return LLVMBuildGEP2(c->builder, elem, f->data, &idx, 1, "stable_element");
        }
    }
    LLVMValueRef args[6];
    for (unsigned i = 0; i < 4; ++i) args[i] = LLVMBuildExtractValue(c->builder, value, i, "ref_part");
    args[4] = idx; args[5] = size;
    return call_runtime(c,n, "__kawa_mem_address", ptr(c), args, 6);
}
/* Resolve the managed container alongside its slot, evaluating each source
 * expression once. Field/array offsets keep the nearest allocation identity
 * so a later callback cannot turn a saved slot address into an unchecked
 * write to freed or recycled storage. */
static int index_may_invalidate(ASTNode *n) {
    if (!n) return 0;
    switch (n->type) {
    case NODE_LITERAL: case NODE_STRING_LIT: case NODE_VAR_REF: return 0;
    case NODE_BINARY_OP:
        return index_may_invalidate(n->data.bin_op.left) || index_may_invalidate(n->data.bin_op.right);
    case NODE_MEMBER_ACCESS: return index_may_invalidate(n->data.member_access.object);
    case NODE_INDEX: return index_may_invalidate(n->data.index.object) || index_may_invalidate(n->data.index.index);
    case NODE_DEREF: case NODE_AMP: return index_may_invalidate(n->data.deref.expr);
    case NODE_CAST: return index_may_invalidate(n->data.cast.val);
    default: return 1;
    }
}
LLVMValueRef kawa_memory_lvalue(KawaCompiler *c, ASTNode *n, LLVMTypeRef *out_type,
                               LLVMValueRef *container) {
    if (!n) return NULL;
    LLVMValueRef local_container=NULL;
    if (!container) container=&local_container;
    if (n->type==NODE_INDEX || n->type==NODE_DEREF) {
        ASTNode *base=n->type==NODE_INDEX ? n->data.index.object : n->data.deref.expr;
        LLVMValueRef address=kawa_memory_address(c,n,base,
            n->type==NODE_INDEX ? n->data.index.index : NULL,out_type,container);
        if (address || n->type==NODE_DEREF) return address;
        Type *type=kawa_expr_type(c,base);
        if (!type || type->kind!=TYPE_ARRAY) return NULL;
        LLVMTypeRef array_type=NULL;
        address=kawa_memory_lvalue(c,base,&array_type,container);
        if (!address) return NULL;
        LLVMValueRef index=integer_arg(c,n->data.index.index);
        emit_check_or_trap(c,n,LLVMBuildICmp(c->builder,LLVMIntULT,index,constant(c,type->array_len),"array_bounds"),
                           "array index out of bounds");
        if (index_may_invalidate(n->data.index.index))
            address=kawa_memory_write_address(c,address,array_type,*container);
        LLVMValueRef indices[]={constant(c,0),index};
        if (out_type) *out_type=get_llvm_type(c,type->inner);
        n->data_type=type->inner;
        return LLVMBuildGEP2(c->builder,array_type,address,indices,2,"managed_array_element");
    }
    if (n->type!=NODE_MEMBER_ACCESS) return NULL;
    LLVMTypeRef type=NULL;
    LLVMValueRef address=kawa_memory_lvalue(c,n->data.member_access.object,&type,container);
    if (!address) return NULL;
    const char *name=n->data.member_access.member;
    if (LLVMGetTypeKind(type)==LLVMArrayTypeKind) {
        if (!strcmp(name,"data")) error(c,n,"managed arrays cannot decay to raw pointers");
        if (strcmp(name,"len")) error(c,n,"array has no such field");
        LLVMValueRef slot=create_entry_block_alloca(c,i64(c),"array_length");
        LLVMBuildStore(c->builder,constant(c,LLVMGetArrayLength(type)),slot);
        if (out_type) *out_type=i64(c);
        if (container) *container=NULL;
        return slot;
    }
    for (unsigned depth=0; depth<16 && !has_direct_field(c,type,name); ++depth) {
        int middle,field;
        if (!try_promoted_field(c,type,name,&middle,&field)) break;
        address=LLVMBuildStructGEP2(c->builder,type,address,middle,"managed_embedded");
        type=LLVMStructGetTypeAtIndex(type,(unsigned)middle);
    }
    int index=get_field_index(c,type,name);
    StructDef *sd=find_struct_def_pub(c,type);
    if (sd) n->data_type=sd->fields[index].ast_type;
    if (out_type) *out_type=get_field_type(c,type,name);
    return LLVMBuildStructGEP2(c->builder,type,address,index,"managed_field");
}
LLVMValueRef kawa_memory_write_address(KawaCompiler *c, LLVMValueRef slot,
                                      LLVMTypeRef type, LLVMValueRef container) {
    if (!container) return slot;
    for (StableFrame *f=c->stable_stack; f; f=f->next)
        if (f->reference==container) return slot;
    LLVMValueRef args[]={spill(c,container),slot,LLVMSizeOf(type)};
    return call_runtime(c,NULL,"__kawa_mem_write_address",ptr(c),args,3);
}
void kawa_memory_store_owner(KawaCompiler *c, LLVMValueRef slot,
                             LLVMValueRef value, LLVMValueRef container) {
    if (!container) {
        LLVMValueRef args[]={slot,spill(c,value)};
        call_runtime(c,NULL,"__kawa_mem_replace",LLVMVoidTypeInContext(c->context),args,2);
        return;
    }
    LLVMValueRef args[]={slot,spill(c,value),spill(c,container)};
    call_runtime(c,NULL,"__kawa_mem_store_owner",LLVMVoidTypeInContext(c->context),args,3);
}
void kawa_memory_stable(KawaCompiler *c, ASTNode *n) {
    ASTNode *reference = n->data.stable.reference;
    if (reference->type != NODE_VAR_REF || !kawa_is_managed(kawa_expr_type(c, reference)))
        error(c, n, "stable expects a named owner or reference");
    if (c->in_coroutine) error(c, n, "stable access cannot span a coroutine suspension");
    LLVMValueRef value = kawa_memory_value(c, reference);
    LLVMValueRef guard = spill(c, value);
    LLVMValueRef data = call_runtime(c,n, n->data.stable.optional ? "__kawa_mem_try_pin" : "__kawa_mem_pin", ptr(c), &guard, 1);
    LLVMBasicBlockRef missing = NULL, done = NULL;
    if (n->data.stable.optional) {
        LLVMBasicBlockRef acquired = kawa_append_block(c->current_func, "access_acquired");
        missing = kawa_append_block(c->current_func, "access_missing");
        done = kawa_append_block(c->current_func, "access_done");
        LLVMBuildCondBr(c->builder, LLVMBuildIsNotNull(c->builder, data, "acquired"), acquired, missing);
        LLVMPositionBuilderAtEnd(c->builder, acquired);
    }
    StableFrame frame = {get_address(c, reference, NULL), value, data, c->stable_stack};
    DeferFrame *saved = c->defer_stack;
    c->stable_stack = &frame;
    kawa_memory_defer(c, guard, 1);
    codegen_stmt(c, n->data.stable.body);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder))) kawa_memory_cleanup(c, guard, 1);
    c->defer_stack = saved;
    c->stable_stack = frame.next;
    if (missing) {
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder))) LLVMBuildBr(c->builder, done);
        LLVMPositionBuilderAtEnd(c->builder, missing);
        codegen_stmt(c, n->data.stable.otherwise);
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder))) LLVMBuildBr(c->builder, done);
        LLVMPositionBuilderAtEnd(c->builder, done);
    }
}
LLVMValueRef kawa_memory_binary(KawaCompiler *c, ASTNode *n) {
    ASTNode *left=n->data.bin_op.left, *right=n->data.bin_op.right;
    Type *lt=kawa_expr_type(c,left), *rt=kawa_expr_type(c,right);
    if (!kawa_is_managed(lt) && !kawa_is_managed(rt)) return NULL;
    int op=n->data.bin_op.op;
    if (op==TOK_PLUS && lt && (lt->kind==TYPE_OWNER || lt->kind==TYPE_REF) && !kawa_is_managed(rt)) {
        LLVMValueRef reference=kawa_memory_value(c,left);
        LLVMValueRef offset=integer_arg(c,right), size=element_size(c,lt->inner);
        LLVMValueRef length=LLVMBuildUDiv(c->builder,LLVMBuildExtractValue(c->builder,reference,3,"extent"),size,"length");
        LLVMValueRef input=spill(c,reference), out=create_entry_block_alloca(c,ref_type(c),"advanced_reference");
        LLVMValueRef args[]={out,input,offset,length,size};
        call_runtime(c,n,"__kawa_mem_slice",LLVMVoidTypeInContext(c->context),args,5);
        Type *result=arena_alloc(c->arena,sizeof(*result)); *result=*lt; result->kind=TYPE_REF; n->data_type=result;
        return LLVMBuildLoad2(c->builder,ref_type(c),out,"advanced_reference");
    }
    if ((op==TOK_ISEQ || op==TOK_NOTEQ) && kawa_is_managed(lt) && kawa_is_managed(rt) && lt->kind!=TYPE_ARENA && rt->kind!=TYPE_ARENA &&
        kawa_types_same(kawa_resolve_type(c,lt->inner),kawa_resolve_type(c,rt->inner))) {
        LLVMValueRef l=kawa_memory_value(c,left), r=kawa_memory_value(c,right);
        LLVMValueRef equal=LLVMConstInt(LLVMInt1TypeInContext(c->context),1,0);
        // Identity is allocation + generation + position. A narrower view at
        // the same position compares equal; recycled addresses never do.
        for (unsigned i=0; i<3; ++i)
            equal=LLVMBuildAnd(c->builder,equal,LLVMBuildICmp(c->builder,LLVMIntEQ,
                LLVMBuildExtractValue(c->builder,l,i,"left_identity"),
                LLVMBuildExtractValue(c->builder,r,i,"right_identity"),"identity_equal"),"reference_equal");
        return op==TOK_ISEQ ? equal : LLVMBuildNot(c->builder,equal,"reference_not_equal");
    }
    error(c,n,"managed references support equality and forward offsets within their extent");
    return NULL;
}
LLVMValueRef kawa_memory_builtin(KawaCompiler *c, ASTNode *n, const char *name) {
    const char *names[] = {"own", "try_own", "ref_of", "move", "clone", "try_clone", "release",
        "ref_slice", "mem_len", "allocated", "arena", "arena_new", "try_arena_new", "remove",
        "mem_budget", "mem_metric", "resize", "try_resize", "mem_capacity", NULL};
    unsigned which = 0;
    while (names[which] && strcmp(names[which], name)) ++which;
    if (!names[which]) return NULL;
    ASTNode *a = n->data.call.args;
    unsigned count = 0;
    for (ASTNode *it = a; it; it = it->next) ++count;
    unsigned arity = !strcmp(name, "arena") ? 0 : !strcmp(name, "ref_slice") ? 3 :
        (!strcmp(name, "arena_new") || !strcmp(name, "try_arena_new") ||
         !strcmp(name, "resize") || !strcmp(name, "try_resize")) ? 2 : 1;
    if (count != arity) {
        kerr(KAWA_E_ARITY, n, "%s expects %u arguments, got %u", name, arity, count);
        exit(1);
    }
    LLVMTypeRef rt = ref_type(c), vi = LLVMVoidTypeInContext(c->context);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(c->context);
    Type *at = kawa_expr_type(c, a);
    if (a && a->type == NODE_VAR_REF && !at) (void)get_address(c,a,NULL);
    if (!strcmp(name,"resize") || !strcmp(name,"try_resize")) {
        if (!at || at->kind != TYPE_OWNER || a->type != NODE_VAR_REF || !managed_element(c,at->inner,0))
            error(c,n,"resize requires a named owner of supported managed elements");
        LLVMValueRef slot=get_address(c,a,NULL);
        for (StableFrame *f=c->stable_stack; f; f=f->next)
            if (f->slot==slot) error(c,n,"cannot resize a stable binding");
        LLVMValueRef args[]={slot,integer_arg(c,a->next),element_size(c,at->inner)};
        LLVMValueRef ok=call_runtime(c,n,"__kawa_mem_resize",i32,args,3);
        if (!strcmp(name,"try_resize")) return cond_to_bool(c,ok);
        emit_check_or_trap(c,n,cond_to_bool(c,ok),"managed resize failed");
        return ok;
    }
    if (!strcmp(name, "mem_budget")) {
        LLVMValueRef arg = integer_arg(c, a);
        return call_runtime(c,n, "__kawa_mem_budget", vi, &arg, 1);
    }
    if (!strcmp(name, "mem_metric")) {
        LLVMValueRef arg = integer_arg(c, a);
        arg = LLVMBuildTrunc(c->builder, arg, i32, "metric_id");
        return call_runtime(c,n, "__kawa_mem_metric", i64(c), &arg, 1);
    }
    if (!strcmp(name, "own") || !strcmp(name, "try_own") || !strcmp(name, "arena") ||
        !strcmp(name, "arena_new") || !strcmp(name, "try_arena_new")) {
        Type *t = n->data_type;
        int arena = !strcmp(name, "arena");
        int child = !strcmp(name, "arena_new") || !strcmp(name, "try_arena_new");
        if (!t || t->kind != (arena ? TYPE_ARENA : child ? TYPE_REF : TYPE_OWNER))
            error(c, n, "annotate allocation with owner<T>, ref<T> for arena_new, or arena");
        if (!arena && !managed_element(c, t->inner, 0))
            error(c, n, "managed allocation requires scalar, ref, owner, fixed array, or supported struct elements");
        LLVMValueRef out = create_entry_block_alloca(c, rt, "new_owner");
        LLVMValueRef args[4] = {out};
        unsigned argc = 1;
        if (child) {
            if (!at || at->kind != TYPE_ARENA) error(c, n, "arena_new requires an arena");
            args[argc++] = spill(c, kawa_memory_value(c, a));
        }
        if (!arena) {
            args[argc++] = integer_arg(c, child ? a->next : a);
            args[argc++] = element_size(c, t->inner);
        }
        LLVMValueRef ok = call_runtime(c,n, arena ? "__kawa_mem_arena" : child ?
            "__kawa_mem_arena_alloc" : "__kawa_mem_alloc", i32, args, argc);
        if (strncmp(name, "try_", 4))
            emit_check_or_trap(c, n, cond_to_bool(c, ok), "managed allocation failed");
        return LLVMBuildLoad2(c->builder, rt, out, "allocated_reference");
    }
    if (!kawa_is_managed(at) && strcmp(name,"ref_of"))
        error(c, n, "memory operation expects an owner or reference");
    if (!strcmp(name, "move") || !strcmp(name, "release") || !strcmp(name, "remove")) {
        int remove = !strcmp(name, "remove");
        int lvalue=a->type==NODE_VAR_REF || a->type==NODE_MEMBER_ACCESS || a->type==NODE_INDEX || a->type==NODE_DEREF;
        if ((!remove && !kawa_is_owner(at)) || (remove && at->kind != TYPE_REF) || !lvalue)
            error(c, n, "move/release require an owning lvalue; remove requires an arena reference");
        LLVMValueRef container=NULL;
        LLVMValueRef slot=kawa_memory_lvalue(c,a,NULL,&container);
        if (!slot) slot=get_address(c,a,NULL);
        for (StableFrame *f = c->stable_stack; f; f = f->next)
            if (f->slot == slot) error(c, n, "cannot move or invalidate a stable binding");
        if (!strcmp(name, "move")) {
            if (container) {
                LLVMValueRef out=create_entry_block_alloca(c,rt,"moved_field");
                LLVMValueRef args[]={out,slot,spill(c,container)};
                call_runtime(c,n,"__kawa_mem_take",vi,args,3);
                return LLVMBuildLoad2(c->builder,rt,out,"moved_owner");
            }
            LLVMValueRef value = LLVMBuildLoad2(c->builder,rt,slot,"moved_owner");
            LLVMBuildStore(c->builder, LLVMConstNull(rt), slot);
            return value;
        }
        return call_runtime(c,n, remove ? "__kawa_mem_remove" : "__kawa_mem_drop", vi, &slot, 1);
    }
    if (!strcmp(name,"ref_of") && !kawa_is_managed(at)) {
        LLVMTypeRef type=NULL;
        LLVMValueRef container=NULL, slot=kawa_memory_lvalue(c,a,&type,&container);
        at=kawa_expr_type(c,a);
        if (!slot || !container || !at || !managed_element(c,at,0))
            error(c,n,"ref_of requires managed storage; stack and raw addresses cannot acquire managed identity");
        Type *borrowed=arena_alloc(c->arena,sizeof(*borrowed));
        borrowed->kind=TYPE_REF;
        borrowed->inner=at->kind==TYPE_ARRAY ? at->inner : at;
        n->data_type=borrowed;
        LLVMValueRef out=create_entry_block_alloca(c,rt,"subobject_reference");
        LLVMValueRef args[]={out,spill(c,container),slot,LLVMSizeOf(type)};
        call_runtime(c,n,"__kawa_mem_view",vi,args,4);
        return LLVMBuildLoad2(c->builder,rt,out,"subobject_reference");
    }
    LLVMValueRef value = kawa_memory_value(c, a);
    if (!strcmp(name, "allocated"))
        return LLVMBuildIsNotNull(c->builder, LLVMBuildExtractValue(c->builder, value, 0, "descriptor"), "allocated");
    if (!strcmp(name, "ref_of")) {
        if (at->kind == TYPE_ARENA) error(c, n, "borrow arena objects with arena_new");
        return value;
    }
    if (at->kind == TYPE_ARENA) error(c, n, "operation requires an allocation, not an arena");
    if (!strcmp(name, "mem_len"))
        return LLVMBuildUDiv(c->builder, LLVMBuildExtractValue(c->builder, value, 3, "extent"),
                             element_size(c, at->inner), "length");
    LLVMValueRef in = spill(c, value);
    if (!strcmp(name,"mem_capacity")) {
        LLVMValueRef capacity=call_runtime(c,n,"__kawa_mem_capacity",i64(c),&in,1);
        return LLVMBuildUDiv(c->builder,capacity,element_size(c,at->inner),"capacity");
    }
    LLVMValueRef out = create_entry_block_alloca(c, rt, "memory_result");
    if (!strcmp(name, "ref_slice")) {
        LLVMValueRef start=integer_arg(c,a->next), end=integer_arg(c,a->next->next);
        LLVMValueRef args[] = {out, in, start, end,
                               element_size(c, at->inner)};
        call_runtime(c,n, "__kawa_mem_slice", vi, args, 5);
    } else {
        if (!managed_element(c, at->inner, 0)) error(c, n, "clone requires supported managed elements");
        LLVMValueRef args[] = {out, in};
        LLVMValueRef ok = call_runtime(c,n, "__kawa_mem_clone", i32, args, 2);
        if (!strcmp(name, "clone")) emit_check_or_trap(c, n, cond_to_bool(c, ok), "managed clone failed");
    }
    return LLVMBuildLoad2(c->builder, rt, out, "memory_result");
}
