// Comptime evaluation (IDEAS 2.1): `const X = comptime fib(10);`
//
// A small tree-walking interpreter over the AST: literals, const refs,
// integer binops (via the existing folder) and -- the point of the
// feature -- CALLS to `pure` functions whose bodies are ordinary Kawa
// (lets, constant ifs, returns). The function's IR is never generated for
// a comptime-only call; the compiler just runs it on i64 values.
//
// Guards: recursion depth 512 and a step counter so a runaway comptime
// program fails compilation instead of hanging the driver. Anything the
// evaluator can't model (loops, mutation, FP) makes the call fall back
// to ordinary runtime codegen.
#include "codegen_internal.h"

#define KAWA_COMPTIME_MAX_DEPTH 512
#define KAWA_COMPTIME_MAX_STEPS 10000000

typedef struct {
	long long value;
	int is_signed;
	int ok;
	unsigned width;
} ComptimeInt;

// Lexical environment: params and lets bound during this evaluation,
// newest-first so shadowed names resolve to the innermost binding.
typedef struct CeEnv {
	const char *name;
	ComptimeInt value;
	struct CeEnv *next;
} CeEnv;

static ComptimeInt ce_eval(KawaCompiler *c, ASTNode *n, CeEnv *env,
						   int depth, long long *steps);
static ComptimeInt ce_exec_stmts(KawaCompiler *c, ASTNode *stmts, CeEnv *env,
								 int depth, long long *steps, int *returned);

static ComptimeInt ce_fail(void) {
	ComptimeInt r = {0};
	return r;
}

static ComptimeInt ce_bits(uint64_t bits, unsigned width, int sign) {
	uint64_t mask=width==64 ? UINT64_MAX : (UINT64_C(1)<<width)-1;
	bits &= mask;
	if (sign && (bits & (UINT64_C(1)<<(width-1)))) bits |= ~mask;
	ComptimeInt result={(long long)bits,sign,1,width};
	return result;
}
static ComptimeInt ce_convert(KawaCompiler *c, ComptimeInt v, Type *type, int lossy) {
	if (!v.ok || !type) return ce_fail();
	LLVMTypeRef target=get_llvm_type(c,type);
	if (LLVMGetTypeKind(target)!=LLVMIntegerTypeKind) return ce_fail();
	unsigned width=LLVMGetIntTypeWidth(target);
	int sign=width!=1 && type_is_signed(c,type);
	if (width==1) return ce_bits(v.value!=0,1,0);
	__int128 value=v.is_signed ? (__int128)v.value : (__int128)(uint64_t)v.value;
	__int128 minimum=sign ? -((__int128)1<<(width-1)) : 0;
	__int128 end=(__int128)1<<(width-(sign ? 1 : 0));
	if (!lossy && (value<minimum || value>=end)) return ce_fail();
	return ce_bits((uint64_t)v.value,width,sign);
}

static ASTNode *ce_find_fn(KawaCompiler *c, const char *name) {
	for (ASTNode *s = c->program_root ? c->program_root->next : NULL; s;
		 s = s->next)
		if (s->type == NODE_FUNC_DECL &&
			strcmp(s->data.func.name, name) == 0)
			return s;
	return NULL;
}

static ComptimeInt ce_lookup(CeEnv *env, const char *name) {
	for (CeEnv *b = env; b; b = b->next)
		if (strcmp(b->name, name) == 0)
			return b->value;
	return ce_fail();
}

static ComptimeInt ce_call(KawaCompiler *c, ASTNode *fn, ASTNode *args,
						   CeEnv *caller_env, int depth, long long *steps) {
	if (!fn->data.func.is_pure)
		return ce_fail(); // only pure fns run at comptime

	CeEnv *env = NULL;
	CeEnv **tail = &env;
	ASTNode *param = fn->data.func.args;
	ASTNode *arg = args;
	while (param && arg) {
		ComptimeInt v = ce_eval(c, arg, caller_env, depth + 1, steps);
		if (!v.ok)
			return v;
		v=ce_convert(c,v,param->data_type,0);
		if (!v.ok || arg->has_arg_label) return ce_fail();
		CeEnv *b = arena_alloc(c->arena, sizeof(CeEnv));
		b->name = param->data.var_decl.name;
		b->value = v;
		b->next = NULL;
		*tail = b;
		tail = &b->next;
		param = param->next;
		arg = arg->next;
	}
	if (param || arg)
		return ce_fail(); // arity mismatch: not foldable

	ASTNode *body = fn->data.func.body;
	if (!body || body->type != NODE_BLOCK)
		return ce_fail();
	int returned = 0;
	ComptimeInt result =
		ce_exec_stmts(c, body->data.block.stmts, env, depth, steps, &returned);
	if (!returned)
		return ce_fail(); // fell off the end without a return
	return ce_convert(c,result,fn->data.func.ret_type,0);
}

static ComptimeInt ce_exec_stmts(KawaCompiler *c, ASTNode *stmts, CeEnv *env,
								 int depth, long long *steps, int *returned) {
	ComptimeInt result = ce_fail();
	for (ASTNode *st = stmts; st && !*returned; st = st->next) {
		switch (st->type) {
		case NODE_VAR_DECL: {
			ComptimeInt v =
				st->data.var_decl.init
					? ce_eval(c, st->data.var_decl.init, env, depth + 1,
							  steps)
					: ce_fail();
			if (!v.ok)
				return v;
			if (st->data_type) v=ce_convert(c,v,st->data_type,0);
			if (!v.ok) return v;
			CeEnv *b = arena_alloc(c->arena, sizeof(CeEnv));
			b->name = st->data.var_decl.name;
			b->value = v;
			b->next = env; // newest-first
			env = b;
			break;
		}
		case NODE_RETURN:
			result = st->data.ret_stmt.expr
						 ? ce_eval(c, st->data.ret_stmt.expr, env, depth + 1,
								   steps)
						 : ce_fail();
			*returned = 1;
			break;
		case NODE_IF: {
			ComptimeInt cond =
				ce_eval(c, st->data.if_stmt.cond, env, depth + 1, steps);
			if (!cond.ok)
				return cond;
			ASTNode *branch =
				cond.value ? st->data.if_stmt.then_block
						   : st->data.if_stmt.else_block;
			if (!branch)
				break; // false condition, no else
			if (branch->type == NODE_BLOCK) {
				ComptimeInt r = ce_exec_stmts(c, branch->data.block.stmts,
											  env, depth + 1, steps,
											  returned);
				if (*returned)
					result = r;
			} else if (branch->type == NODE_RETURN) {
				result = ce_eval(c, branch->data.ret_stmt.expr, env,
								 depth + 1, steps);
				*returned = 1;
			} else {
				return ce_fail();
			}
			break;
		}
		default:
			// Mutation, loops, calls-as-statements: future work. Fail so
			// the call falls back to runtime codegen.
			return ce_fail();
		}
	}
	return result;
}

static ComptimeInt ce_eval(KawaCompiler *c, ASTNode *n, CeEnv *env, int depth,
						   long long *steps) {
	if (++*steps > KAWA_COMPTIME_MAX_STEPS)
		return ce_fail(); // runaway comptime program
	if (depth > KAWA_COMPTIME_MAX_DEPTH)
		return ce_fail();

	switch (n->type) {
	case NODE_LITERAL:
		if (n->data_type &&
			(n->data_type->kind == TYPE_F16 || n->data_type->kind == TYPE_BF16 ||
			n->data_type->kind == TYPE_F32 || n->data_type->kind == TYPE_F64))
			return ce_fail(); // FP folding stays with the IR folder
		return ce_bits(n->data.literal.i64_val,n->data_type ? LLVMGetIntTypeWidth(get_llvm_type(c,n->data_type)) : 32,
					 type_is_signed(c, n->data_type));

	case NODE_BINARY_OP: {
		static const int cmp_ops[] = {TOK_LANGLE, TOK_RANGLE, TOK_LEQ,
									  TOK_REQ, TOK_ISEQ, TOK_NOTEQ};
		int is_cmp = 0;
		for (unsigned k = 0; k < sizeof(cmp_ops) / sizeof(cmp_ops[0]); k++)
			if (n->data.bin_op.op == cmp_ops[k])
				is_cmp = 1;
		ComptimeInt l =
			ce_eval(c, n->data.bin_op.left, env, depth + 1, steps);
		int op=n->data.bin_op.op;
		if (l.ok && (op==TOK_ANDAND || op==TOK_OROR)) {
			if ((op==TOK_ANDAND && !l.value) || (op==TOK_OROR && l.value))
				return ce_bits(l.value!=0,1,0);
			ComptimeInt r=ce_eval(c,n->data.bin_op.right,env,depth+1,steps);
			return r.ok ? ce_bits(r.value!=0,1,0) : r;
		}
		ComptimeInt r =
			ce_eval(c, n->data.bin_op.right, env, depth + 1, steps);
		if (!l.ok || !r.ok)
			return ce_fail();
		unsigned width=l.width>r.width ? l.width : r.width;
		int ls=l.is_signed, rs=r.is_signed;
		if (ls && !rs && r.width>=l.width) ls=0;
		else if (!ls && rs && l.width>=r.width) rs=0;
		if (is_cmp) {
			// Mixed signedness compares unsigned, matching runtime icmps.
			long long lv = l.value, rv = r.value;
			int sg = ls || rs;
			lv=ce_bits(l.value,width,sg).value; rv=ce_bits(r.value,width,sg).value;
			unsigned long long ulv = (unsigned long long)lv,
							   urv = (unsigned long long)rv;
			long long res;
			switch (n->data.bin_op.op) {
			case TOK_LANGLE: res = sg ? (lv < rv) : (ulv < urv); break;
			case TOK_RANGLE: res = sg ? (lv > rv) : (ulv > urv); break;
			case TOK_LEQ: res = sg ? (lv <= rv) : (ulv <= urv); break;
			case TOK_REQ: res = sg ? (lv >= rv) : (ulv >= urv); break;
			case TOK_ISEQ: res = (lv == rv); break;
			default: res = (lv != rv); break;
			}
			return ce_bits(res,1,0);
		}
		unsigned long long out;
		if (!fold_int_binop(n->data.bin_op.op,
							(unsigned long long)l.value,
							(unsigned long long)r.value, ls,
							rs, width, &out))
			return ce_fail();
		return ce_bits(out,width,ls || rs);
	}

	case NODE_VAR_REF: {
		// Innermost comptime binding first...
		ComptimeInt bound = ce_lookup(env, n->data.var_ref.name);
		if (bound.ok)
			return bound;
		// ...then file-scope consts.
		Scope *sv = scope_find(c, n->data.var_ref.name);
		if (sv && sv->node && sv->node->type == NODE_VAR_DECL &&
			sv->node->data.var_decl.is_const && sv->node->data.var_decl.init)
			return ce_eval(c, sv->node->data.var_decl.init, NULL, depth + 1,
						   steps);
		return ce_fail();
	}

	case NODE_MEMBER_ACCESS: {
		if (n->data.member_access.object->type == NODE_VAR_REF) {
			char mangled[256];
			snprintf(mangled, sizeof(mangled), "%s__%s",
					 n->data.member_access.object->data.var_ref.name,
					 n->data.member_access.member);
			Scope *asv = scope_find(c, mangled);
			if (asv && asv->node && asv->node->type == NODE_VAR_DECL &&
				asv->node->data.var_decl.is_const && asv->node->data.var_decl.init)
				return ce_eval(c, asv->node->data.var_decl.init, NULL, depth + 1,
							   steps);
		}
		return ce_fail();
	}

	case NODE_CALL: {
		ASTNode *leaf = n->data.call.callee;
		if (!leaf || leaf->type != NODE_VAR_REF)
			return ce_fail();
		ASTNode *fn = ce_find_fn(c, leaf->data.var_ref.name);
		if (!fn)
			return ce_fail();
		return ce_call(c, fn, n->data.call.args, env, depth, steps);
	}
	case NODE_CAST:
		return ce_convert(c,ce_eval(c,n->data.cast.val,env,depth+1,steps),n->data_type,0);
	case NODE_TERNARY: {
		ComptimeInt condition=ce_eval(c,n->data.ternary.cond,env,depth+1,steps);
		if (!condition.ok) return condition;
		return ce_eval(c,condition.value ? n->data.ternary.then_expr : n->data.ternary.else_expr,env,depth+1,steps);
	}

	default:
		return ce_fail();
	}
}

LLVMValueRef kawa_comptime_eval(KawaCompiler *c, ASTNode *n,
								unsigned result_width, int *out_signed) {
	long long steps = 0;
	ComptimeInt v = ce_eval(c, n, NULL, 0, &steps);
	if (!v.ok)
		return NULL;
	if (out_signed)
		*out_signed = v.is_signed;
	if (!result_width) result_width=v.width;
	return LLVMConstInt(LLVMIntTypeInContext(c->context, result_width),
						(unsigned long long)v.value, v.is_signed);
}
