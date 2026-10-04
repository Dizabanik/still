#include "ast.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct CloneEntry {
    ASTNode *source, *copy;
    struct CloneEntry *next;
} CloneEntry;
typedef struct {
    Arena *arena;
    int count;
    char **parameters;
    Type **concretes;
    const char *generic_struct, *instance_struct;
    CloneEntry *nodes[256];
} Clone;
static Type *type_copy(Clone *c,Type *source,int substitute) {
    if (!source) return NULL;
    if (substitute && source->kind==TYPE_STRUCT && source->name) {
        for (int i=0; i<c->count; ++i)
            if (!strcmp(source->name,c->parameters[i]))
                return type_copy(c,c->concretes[i],0);
    }
    Type *copy=arena_alloc(c->arena,sizeof(*copy));
    *copy=*source;
    if (substitute && c->generic_struct && source->name &&
        !strcmp(source->name,c->generic_struct))
        copy->name=(char *)c->instance_struct;
    copy->inner=type_copy(c,source->inner,substitute);
    return copy;
}
static char *rename_symbol(Clone *c,char *name) {
    if (!name || !c->generic_struct || !c->instance_struct) return name;
    size_t length=strlen(c->generic_struct);
    if (!strcmp(name,c->generic_struct)) return (char *)c->instance_struct;
    if (strncmp(name,c->generic_struct,length) || strncmp(name+length,"__",2)) return name;
    size_t size=strlen(c->instance_struct)+strlen(name+length)+1;
    char *out=arena_alloc(c->arena,size);
    snprintf(out,size,"%s%s",c->instance_struct,name+length);
    return out;
}
static ASTNode *node_copy(Clone *,ASTNode *);
static ASTNode *list_copy(Clone *c,ASTNode *source) {
    ASTNode *head=NULL, **tail=&head;
    for (; source; source=source->next) {
        *tail=node_copy(c,source);
        tail=&(*tail)->next;
    }
    return head;
}
#define NODE(field) copy->data.field=node_copy(c,source->data.field)
#define LIST(field) copy->data.field=list_copy(c,source->data.field)
#define TYPE(field) copy->data.field=type_copy(c,source->data.field,1)
static ASTNode *node_copy(Clone *c,ASTNode *source) {
    if (!source) return NULL;
    unsigned bucket=((uintptr_t)source>>4)&255;
    for (CloneEntry *e=c->nodes[bucket]; e; e=e->next)
        if (e->source==source) return e->copy;
    ASTNode *copy=arena_alloc(c->arena,sizeof(*copy));
    *copy=*source; copy->next=NULL; copy->dependents=NULL;
    CloneEntry *entry=arena_alloc(c->arena,sizeof(*entry));
    *entry=(CloneEntry){source,copy,c->nodes[bucket]};
    c->nodes[bucket]=entry;
    copy->data_type=type_copy(c,source->data_type,1);
    switch (source->type) {
    case NODE_PROGRAM: case NODE_BLOCK: LIST(block.stmts); break;
    case NODE_UNCHECKED_BLOCK: NODE(block.stmts); break;
    case NODE_FUNC_DECL:
        copy->data.func.name=rename_symbol(c,source->data.func.name);
        LIST(func.args); NODE(func.body); TYPE(func.ret_type); break;
    case NODE_STRUCT_DECL: LIST(struct_decl.fields); break;
    case NODE_IMPL_BLOCK: LIST(impl.methods); break;
    case NODE_VAR_DECL: case NODE_CONST_DECL:
        NODE(var_decl.init); NODE(var_decl.field_default); break;
    case NODE_ASSIGN: NODE(assign.target); NODE(assign.value); break;
    case NODE_RETURN: NODE(ret_stmt.expr); break;
    case NODE_BINARY_OP: NODE(bin_op.left); NODE(bin_op.right); break;
    case NODE_TERNARY: NODE(ternary.cond); NODE(ternary.then_expr); NODE(ternary.else_expr); break;
    case NODE_VAR_REF: copy->data.var_ref.name=rename_symbol(c,source->data.var_ref.name); break;
    case NODE_MEMBER_ACCESS: NODE(member_access.object); break;
    case NODE_INDEX: NODE(index.object); NODE(index.index); break;
    case NODE_CALL: NODE(call.callee); LIST(call.args); break;
    case NODE_SET_LITERAL: LIST(set_lit.items); break;
    case NODE_SET_POUR: NODE(set_pour.target); NODE(set_pour.value); break;
    case NODE_BREW: NODE(brew.body); break;
    case NODE_SIP: NODE(sip.handle); break;
    case NODE_DROP: NODE(drop.val); break;
    case NODE_IF: NODE(if_stmt.cond); NODE(if_stmt.then_block); NODE(if_stmt.else_block); break;
    case NODE_WHILE: NODE(while_stmt.cond); NODE(while_stmt.body); break;
    case NODE_FOR: NODE(for_stmt.init); NODE(for_stmt.cond); NODE(for_stmt.step); NODE(for_stmt.body); break;
    case NODE_SWITCH: NODE(switch_stmt.value); LIST(switch_stmt.cases); break;
    case NODE_CASE: NODE(case_stmt.expr); NODE(case_stmt.body); break;
    case NODE_BATCH: NODE(batch.collection); NODE(batch.body); break;
    case NODE_DEFER: NODE(defer.stmt); LIST(defer.captures); break;
    case NODE_FILTER: NODE(filter.try_block); NODE(filter.catch_block); TYPE(filter.err_type); break;
    case NODE_PRESS: NODE(press.target); break;
    case NODE_ALIAS: TYPE(alias.target_type); break;
    case NODE_EXTERN_FN: LIST(extern_fn.args); TYPE(extern_fn.ret_type); break;
    case NODE_ASM: LIST(asm_block.outputs); break;
    case NODE_STABLE: NODE(stable.reference); NODE(stable.body); NODE(stable.otherwise); break;
    case NODE_SEND: NODE(send.chan); NODE(send.value); break;
    case NODE_RECV: NODE(recv.chan); break;
    case NODE_SELECT: {
        copy->data.select_stmt.cases=NULL;
        struct SelectCase **tail=&copy->data.select_stmt.cases;
        for (struct SelectCase *s=source->data.select_stmt.cases; s; s=s->next) {
            *tail=arena_alloc(c->arena,sizeof(**tail));
            **tail=*s;
            (*tail)->var_decl=node_copy(c,s->var_decl);
            (*tail)->chan=node_copy(c,s->chan);
            (*tail)->body=node_copy(c,s->body);
            (*tail)->next=NULL;
            tail=&(*tail)->next;
        }
        NODE(select_stmt.default_body); break;
    }
    case NODE_SIZEOF: NODE(size_of.value); TYPE(size_of.type_val); break;
    case NODE_CAST: NODE(cast.val); break;
    case NODE_MATCH: NODE(match_stmt.target); LIST(match_stmt.arms); break;
    case NODE_MATCH_ARM: LIST(match_arm.bindings); NODE(match_arm.body); break;
    case NODE_RANGE: NODE(range.left); NODE(range.right); break;
    case NODE_SLICE_INDEX: NODE(slice_index.object); NODE(slice_index.start); NODE(slice_index.end); break;
    case NODE_STRUCT_LITERAL: {
        copy->data.struct_lit.items=NULL;
        StructInitItem **tail=&copy->data.struct_lit.items;
        for (StructInitItem *s=source->data.struct_lit.items; s; s=s->next) {
            *tail=arena_alloc(c->arena,sizeof(**tail));
            **tail=*s;
            (*tail)->value=node_copy(c,s->value);
            (*tail)->spread_from=node_copy(c,s->spread_from);
            (*tail)->next=NULL;
            tail=&(*tail)->next;
        }
        break;
    }
    case NODE_DEREF: case NODE_AMP: NODE(deref.expr); break;
    case NODE_ENUM_DECL: {
        copy->data.enum_decl.variants=NULL;
        EnumVariant **tail=&copy->data.enum_decl.variants;
        for (EnumVariant *v=source->data.enum_decl.variants; v; v=v->next) {
            *tail=arena_alloc(c->arena,sizeof(**tail)); **tail=*v;
            for (int i=0; i<v->payload_count; ++i)
                (*tail)->payload_types[i]=type_copy(c,v->payload_types[i],1);
            (*tail)->next=NULL; tail=&(*tail)->next;
        }
        LIST(enum_decl.fields); break;
    }
    default: break; /* leaves carry immutable strings or scalar data */
    }
    Dependency **tail=&copy->dependents;
    for (Dependency *d=source->dependents; d; d=d->next) {
        *tail=arena_alloc(c->arena,sizeof(**tail));
        (*tail)->dependent_node=node_copy(c,d->dependent_node);
        (*tail)->logic_expr=node_copy(c,d->logic_expr);
        (*tail)->next=NULL; tail=&(*tail)->next;
    }
    return copy;
}
ASTNode *kawa_clone_ast(Arena *arena,ASTNode *source,int count,char **parameters,
                        Type **concretes,const char *generic_struct,const char *instance_struct) {
    Clone c={.arena=arena,.count=count,.parameters=parameters,.concretes=concretes,
             .generic_struct=generic_struct,.instance_struct=instance_struct};
    return node_copy(&c,source);
}
