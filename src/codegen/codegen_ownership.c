#include "codegen_internal.h"

/* Affine owners, ordinary mutable aliases. This analysis has no runtime cost.
 * Each branch gets a private environment; joins retain every possible state.
 * Loop backedges and exits are distinct, so moving an outer owner and breaking
 * is legal, while moving it again on another iteration is rejected. */
enum { LIVE = 1, CONSUMED = 2 };
typedef struct Binding {
    ASTNode *declaration, *consumed_at;
    const char *name;
    int owner, state;
    struct Binding *next;
} Binding;
typedef struct Pending {
    ASTNode *statement, *owner;
    struct Pending *next;
} Pending;
typedef struct Edge {
    Binding *bindings, *breaks, *continues;
    Pending *defers;
    int is_switch;
    struct Edge *next;
} Edge;
typedef struct Handler {
    Binding *bindings, *errors;
    Pending *defers;
    int has_errors;
    struct Handler *next;
} Handler;
typedef struct {
    StillCompiler *compiler;
    Pending *defers;
    Edge *loop;
    Handler *handler;
    int in_defer;
} Analysis;

static int statement(Analysis *, ASTNode *, Binding **);
static void expression(Analysis *, ASTNode *, Binding **);
static int scoped_statement(Analysis *a, ASTNode *n, Binding **env) {
    if (!n || n->type==NODE_BLOCK) return statement(a,n,env);
    ASTNode block={.type=NODE_BLOCK,.line=n->line};
    block.data.block.stmts=n;
    return statement(a,&block,env);
}
static Binding *named(Binding *env, const char *name) {
    for (; env; env=env->next) if (!strcmp(env->name,name)) return env;
    return NULL;
}
static Binding *declared(Binding *env, ASTNode *decl) {
    for (; env; env=env->next) if (env->declaration==decl) return env;
    return NULL;
}
static Binding *copy(Analysis *a, Binding *env) {
    Binding *result=NULL, **tail=&result;
    for (; env; env=env->next) {
        *tail=arena_alloc(a->compiler->arena,sizeof(**tail));
        **tail=*env; (*tail)->next=NULL; tail=&(*tail)->next;
    }
    return result;
}
/* Merge only declarations visible at the destination, never branch locals. */
static void merge(Binding *destination, Binding *incoming) {
    for (; destination; destination=destination->next) {
        Binding *b=declared(incoming,destination->declaration);
        if (!b) continue;
        destination->state |= b->state;
        if (b->consumed_at) destination->consumed_at=b->consumed_at;
    }
}
static void edge(Analysis *a, Binding **destination, Binding *base, Binding *incoming) {
    if (!*destination) {
        *destination=copy(a,base);
        for (Binding *b=*destination; b; b=b->next) {
            Binding *other=declared(incoming,b->declaration);
            if (other) { b->state=other->state; b->consumed_at=other->consumed_at; }
        }
    } else merge(*destination,incoming);
}
static void bind(Analysis *a, Binding **env, ASTNode *decl, int cleanup) {
    Binding *b=arena_alloc(a->compiler->arena,sizeof(*b));
    b->declaration=decl; b->name=decl->data.var_decl.name;
    b->owner=wky_contains_managed(a->compiler,decl->data_type,1);
    b->state=LIVE; b->next=*env; *env=b;
    if (b->owner && cleanup) {
        Pending *d=arena_alloc(a->compiler->arena,sizeof(*d));
        d->owner=decl; d->next=a->defers; a->defers=d;
    }
}
static void require_live(Analysis *a, ASTNode *use, Binding *b) {
    if (!b || !b->owner || b->state==LIVE) return;
    StillCompiler *c=a->compiler;
    const char *origin_file=c->source_filename, *consumed_file=c->source_filename;
    int origin_line, consumed_line;
    still_diag_location(b->declaration->line,&origin_file,&origin_line);
    still_diag_location(b->consumed_at ? b->consumed_at->line : 0,&consumed_file,&consumed_line);
    still_diag_note("owner `%s` declared at %s:%d; consumed at %s:%d",
               b->name,origin_file,origin_line,consumed_file,consumed_line);
    still_diag_help("borrow with ref_of before moving, or keep the new owner binding");
    still_error(STILL_E_OWNERSHIP,use,"owner `%s` %s been moved or released",b->name,
         b->state==CONSUMED ? "has" : "may have");
    exit(1);
}
static void cleanup(Analysis *a, Pending *end, Binding **env) {
    Pending *saved=a->defers;
    for (Pending *d=saved; d && d!=end; d=d->next) {
        if (d->owner) {
            Binding *b=declared(*env,d->owner);
            if (b) { b->state=CONSUMED; b->consumed_at=d->owner; }
        } else {
            a->defers=d->next;
            ++a->in_defer;
            statement(a,d->statement,env);
            --a->in_defer;
        }
    }
    a->defers=saved;
}
static void alternatives(Analysis *a, ASTNode *first, ASTNode *second, Binding **env) {
    Binding *left=copy(a,*env), *right=copy(a,*env);
    expression(a,first,&left); expression(a,second,&right);
    merge(left,right);
    for (Binding *b=*env; b; b=b->next) {
        Binding *other=declared(left,b->declaration);
        if (other) { b->state=other->state; b->consumed_at=other->consumed_at; }
    }
}
static void expression(Analysis *a, ASTNode *n, Binding **env) {
    if (!n) return;
    switch(n->type) {
    case NODE_VAR_REF: require_live(a,n,named(*env,n->data.var_ref.name)); break;
    case NODE_CALL: {
        ASTNode *callee=n->data.call.callee, *arg=n->data.call.args;
        const char *name=callee && callee->type==NODE_VAR_REF ? callee->data.var_ref.name : "";
        // Inspecting the nullable owner slot is legal after a move.
        if (!strcmp(name,"allocated") && arg && arg->type==NODE_VAR_REF && !arg->next) break;
        for (ASTNode *it=arg; it; it=it->next) expression(a,it,env);
        if ((!strcmp(name,"move") || !strcmp(name,"release")) && arg &&
            arg->type==NODE_VAR_REF && !arg->next) {
            Binding *b=named(*env,arg->data.var_ref.name);
            if (b && b->owner) { b->state=CONSUMED; b->consumed_at=n; }
        }
        if (callee && callee->type!=NODE_VAR_REF) expression(a,callee,env);
        break;
    }
    case NODE_BINARY_OP:
        expression(a,n->data.bin_op.left,env);
        if (n->data.bin_op.op==TOK_ANDAND || n->data.bin_op.op==TOK_OROR)
            alternatives(a,n->data.bin_op.right,NULL,env);
        else expression(a,n->data.bin_op.right,env);
        break;
    case NODE_TERNARY:
        expression(a,n->data.ternary.cond,env);
        alternatives(a,n->data.ternary.then_expr,n->data.ternary.else_expr,env);
        break;
    case NODE_MEMBER_ACCESS: expression(a,n->data.member_access.object,env); break;
    case NODE_INDEX:
        expression(a,n->data.index.object,env); expression(a,n->data.index.index,env); break;
    case NODE_SLICE_INDEX:
        expression(a,n->data.slice_index.object,env); expression(a,n->data.slice_index.start,env);
        expression(a,n->data.slice_index.end,env); break;
    case NODE_DEREF: case NODE_AMP: expression(a,n->data.deref.expr,env); break;
    case NODE_CAST: expression(a,n->data.cast.val,env); break;
    case NODE_STRUCT_LITERAL:
        for (StructInitItem *it=n->data.struct_lit.items; it; it=it->next) {
            expression(a,it->value,env); expression(a,it->spread_from,env);
        }
        break;
    case NODE_SET_LITERAL:
        for (ASTNode *it=n->data.set_lit.items; it; it=it->next) expression(a,it,env);
        break;
    case NODE_SEND: expression(a,n->data.send.chan,env); expression(a,n->data.send.value,env); break;
    case NODE_RECV: expression(a,n->data.recv.chan,env); break;
    case NODE_SIP: expression(a,n->data.sip.handle,env); break;
    case NODE_DROP: expression(a,n->data.drop.val,env); break;
    case NODE_SET_POUR:
        expression(a,n->data.set_pour.target,env); expression(a,n->data.set_pour.value,env); break;
    case NODE_BLOCK: case NODE_MATCH: statement(a,n,env); break;
    default: break;
    }
}
static int branch(Analysis *a, ASTNode *left, ASTNode *right, Binding **env) {
    Binding *l=copy(a,*env), *r=copy(a,*env);
    int ls=scoped_statement(a,left,&l), rs=scoped_statement(a,right,&r);
    if (ls && rs) return 1;
    if (ls) l=r;
    else if (!rs) merge(l,r);
    for (Binding *b=*env; b; b=b->next) {
        Binding *other=declared(l,b->declaration);
        if (other) { b->state=other->state; b->consumed_at=other->consumed_at; }
    }
    return 0;
}
static void loop(Analysis *a, ASTNode *cond, ASTNode *body, ASTNode *step, Binding **env) {
    Binding *before=copy(a,*env);
    expression(a,cond,env);
    Edge frame={.bindings=copy(a,*env),.defers=a->defers,.next=a->loop};
    a->loop=&frame;
    Binding *iteration=copy(a,*env);
    if (!scoped_statement(a,body,&iteration)) edge(a,&frame.continues,frame.bindings,iteration);
    if (frame.continues) {
        if (!statement(a,step,&frame.continues)) {
            // An owner present before the loop must survive every backedge.
            for (Binding *b=before; b; b=b->next) {
                Binding *after=declared(frame.continues,b->declaration);
                if (b->owner && b->state==LIVE && after && after->state!=LIVE) {
                    StillCompiler *c=a->compiler;
                    still_diag_note("owner `%s` declared at line %d; consumed at line %d",b->name,
                               b->declaration->line,after->consumed_at ? after->consumed_at->line : 0);
                    still_error(STILL_E_OWNERSHIP,after->consumed_at,"owner `%s` may be consumed again on a loop backedge",b->name);
                    exit(1);
                }
            }
            merge(*env,frame.continues);
        }
    }
    if (frame.breaks) merge(*env,frame.breaks);
    a->loop=frame.next;
}
static int statement(Analysis *a, ASTNode *n, Binding **env) {
    if (!n) return 0;
    StillCompiler *c=a->compiler;
    switch(n->type) {
    case NODE_BLOCK: {
        Binding *base=*env;
        Pending *saved=a->defers;
        int stopped=0;
        for (ASTNode *s=n->data.block.stmts; s && !stopped; s=s->next) stopped=statement(a,s,env);
        if (!stopped) cleanup(a,saved,env);
        a->defers=saved;
        *env=base;
        return stopped;
    }
    case NODE_VAR_DECL:
        expression(a,n->data.var_decl.init,env);
        bind(a,env,n,1);
        break;
    case NODE_ASSIGN: {
        ASTNode *target=n->data.assign.target;
        Binding *b=target && target->type==NODE_VAR_REF ? named(*env,target->data.var_ref.name) : NULL;
        if (!b || !b->owner) expression(a,target,env);
        expression(a,n->data.assign.value,env);
        if (b && b->owner) { b->state=LIVE; b->consumed_at=NULL; }
        break;
    }
    case NODE_IF:
        expression(a,n->data.if_stmt.cond,env);
        return branch(a,n->data.if_stmt.then_block,n->data.if_stmt.else_block,env);
    case NODE_WHILE: loop(a,n->data.while_stmt.cond,n->data.while_stmt.body,NULL,env); break;
    case NODE_FOR: {
        Binding *base=*env; Pending *saved=a->defers;
        statement(a,n->data.for_stmt.init,env);
        loop(a,n->data.for_stmt.cond,n->data.for_stmt.body,n->data.for_stmt.step,env);
        cleanup(a,saved,env); a->defers=saved; *env=base;
        break;
    }
    case NODE_BATCH: {
        expression(a,n->data.batch.collection,env);
        Binding *base=*env;
        ASTNode *iter=arena_alloc(c->arena,sizeof(*iter));
        iter->data.var_decl.name=n->data.batch.iterator_var; bind(a,env,iter,0);
        loop(a,NULL,n->data.batch.body,NULL,env); *env=base;
        break;
    }
    case NODE_RETURN:
        expression(a,n->data.ret_stmt.expr,env);
        if (a->in_defer) { still_error(STILL_E_OWNERSHIP,n,"deferred code cannot transfer control"); exit(1); }
        cleanup(a,NULL,env); return 1;
    case NODE_BREAK: case NODE_CONTINUE: {
        if (a->in_defer) { still_error(STILL_E_OWNERSHIP,n,"deferred code cannot transfer control"); exit(1); }
        Edge *target=a->loop;
        if (n->type==NODE_CONTINUE) while (target && target->is_switch) target=target->next;
        if (target) {
            cleanup(a,target->defers,env);
            edge(a,n->type==NODE_BREAK ? &target->breaks : &target->continues,target->bindings,*env);
        }
        return 1;
    }
    case NODE_STABLE:
        expression(a,n->data.stable.reference,env);
        return branch(a,n->data.stable.body,n->data.stable.optional ? n->data.stable.otherwise : NULL,env);
    case NODE_DEFER: {
        for (ASTNode *cap=n->data.defer.captures; cap; cap=cap->next) {
            Binding *b=named(*env,cap->data.var_decl.name);
            if (b && b->owner) { still_error(STILL_E_OWNERSHIP,n,"defer captures cannot copy an owner; borrow it without a capture list"); exit(1); }
        }
        Pending *d=arena_alloc(c->arena,sizeof(*d));
        d->statement=n->data.defer.stmt; d->next=a->defers; a->defers=d;
        break;
    }
    case NODE_FILTER: {
        Handler handler={.bindings=copy(a,*env),.defers=a->defers,.next=a->handler};
        a->handler=&handler;
        Binding *success=copy(a,*env);
        int stopped=statement(a,n->data.filter.try_block,&success);
        a->handler=handler.next;
        if (handler.has_errors) {
            Binding *caught=handler.errors;
            ASTNode *error=arena_alloc(c->arena,sizeof(*error));
            error->data.var_decl.name=n->data.filter.err_var;
            error->data_type=n->data.filter.err_type;
            Pending *catch_defers=a->defers;
            bind(a,&caught,error,1);
            int catch_stopped=statement(a,n->data.filter.catch_block,&caught);
            if (!catch_stopped) cleanup(a,catch_defers,&caught);
            a->defers=catch_defers;
            if (stopped && catch_stopped) return 1;
            if (stopped) success=caught;
            else if (!catch_stopped) merge(success,caught);
        } else if (stopped) return 1;
        for (Binding *b=*env; b; b=b->next) {
            Binding *other=declared(success,b->declaration);
            if (other) { b->state=other->state; b->consumed_at=other->consumed_at; }
        }
        break;
    }
    case NODE_PRESS:
        expression(a,n->data.press.target,env);
        if (a->in_defer) { still_error(STILL_E_OWNERSHIP,n,"deferred code cannot transfer control"); exit(1); }
        if (a->handler) {
            a->handler->has_errors=1;
            cleanup(a,a->handler->defers,env);
            edge(a,&a->handler->errors,a->handler->bindings,*env);
        }
        return 1;
    case NODE_MATCH: {
        expression(a,n->data.match_stmt.target,env);
        Binding *joined=NULL;
        for (ASTNode *arm=n->data.match_stmt.arms; arm; arm=arm->next) {
            Binding *body=copy(a,*env);
            for (ASTNode *b=arm->data.match_arm.bindings; b; b=b->next) bind(a,&body,b,0);
            if (!statement(a,arm->data.match_arm.body,&body)) edge(a,&joined,*env,body);
        }
        if (!joined) return 1;
        for (Binding *b=*env; b; b=b->next) {
            Binding *other=declared(joined,b->declaration);
            if (other) { b->state=other->state; b->consumed_at=other->consumed_at; }
        }
        break;
    }
    case NODE_SWITCH: {
        expression(a,n->data.switch_stmt.value,env);
        Edge frame={.bindings=copy(a,*env),.defers=a->defers,.is_switch=1,.next=a->loop};
        a->loop=&frame;
        Binding *fallthrough=NULL;
        for (ASTNode *cs=n->data.switch_stmt.cases; cs; cs=cs->next) {
            Binding *body=copy(a,*env);
            if (fallthrough) merge(body,fallthrough);
            fallthrough=statement(a,cs->data.case_stmt.body,&body) ? NULL : body;
        }
        if (fallthrough) merge(*env,fallthrough);
        if (frame.breaks) merge(*env,frame.breaks);
        a->loop=frame.next;
        break;
    }
    case NODE_UNCHECKED_BLOCK: statement(a,n->data.block.stmts,env); break;
    default: expression(a,n,env); break;
    }
    return 0;
}

void wky_verify_ownership(StillCompiler *c, ASTNode *function) {
    Analysis a={.compiler=c};
    Binding *env=NULL;
    for (Scope *s=c->global_scope; s; s=s->next)
        if (s->node && s->node->type==NODE_VAR_DECL) bind(&a,&env,s->node,0);
    for (ASTNode *param=function->data.func.args; param; param=param->next) bind(&a,&env,param,1);
    if (!statement(&a,function->data.func.body,&env)) cleanup(&a,NULL,&env);
}
