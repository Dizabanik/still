#include "codegen_internal.h"
#include <llvm-c/BitReader.h>
#include <llvm-c/Linker.h>
#include <llvm-c/Error.h>
#include "kawa_runtime_bc.h"
/* Embedded runtime bitcode uses the compiler's target and SDK metadata. The
 * print runtime may have been generated with an older SDK; its code is linked
 * for this host, so reconcile that descriptive flag before module linking. */
static void runtime_target(KawaCompiler *c,LLVMModuleRef runtime) {
	LLVMSetTarget(runtime,LLVMGetTarget(c->module));
	LLVMSetModuleDataLayout(runtime,c->target_data);
	LLVMMetadataRef sdk=LLVMGetModuleFlag(c->module,"SDK Version",11);
	if (!sdk) return;
	unsigned count=LLVMGetNamedMetadataNumOperands(runtime,"llvm.module.flags");
	LLVMValueRef *flags=malloc((count ? count : 1)*sizeof(*flags));
	LLVMGetNamedMetadataOperands(runtime,"llvm.module.flags",flags);
	for (unsigned i=0; i<count; ++i) {
		if (LLVMGetMDNodeNumOperands(flags[i])!=3) continue;
		LLVMValueRef operands[3]; LLVMGetMDNodeOperands(flags[i],operands);
		unsigned size; const char *name=LLVMGetMDString(operands[1],&size);
		if (name && size==11 && !memcmp(name,"SDK Version",11))
			LLVMReplaceMDNodeOperandWith(flags[i],2,sdk);
	}
	free(flags);
}
#include "kawa_memory_bc.h"
#include "kawa_memory_metrics_bc.h"

// Runtime-initialized globals can't emit their kawa_globals_init body during
// pass 3 -- user functions don't exist yet, so a call like `let g = make();`
// would fail to resolve its callee. Pass 3 records them here; the bodies are
// emitted after all functions are generated.
typedef struct PendingGlobalInit {
	ASTNode *decl;
	LLVMValueRef global;
	LLVMTypeRef g_type;
	struct PendingGlobalInit *next;
} PendingGlobalInit;

static void emit_runtime_global_inits(KawaCompiler *c,
									  PendingGlobalInit *pending,
									  LLVMValueRef *init_fn,
									  LLVMTypeRef *init_fn_type);

// Lower a top-level `let x = ...;` / `[N]i32 arr = {...};` declaration into a
// module-level global. Constant initializers become LLVM constants (zero
// runtime cost, folded into every use); anything else gets queued for the
// synthesized `kawa_globals_init` that main() calls first.
static void codegen_global_decl(KawaCompiler *c, ASTNode *n,
                                PendingGlobalInit **pending) {
    if (kawa_contains_managed(c,n->data_type,1)) {
        kerr(KAWA_E_TYPE,n,"global owners require explicit program-lifetime cleanup support");
        exit(1);
    }
	LLVMTypeRef g_type = get_llvm_type(c, n->data_type);

	// Propagate the declared type to a struct/array literal initializer so
	// BOTH the const-eval path and the runtime path see the right layout.
	if (n->data.var_decl.init && n->data.var_decl.init->type == NODE_STRUCT_LITERAL &&
		!n->data.var_decl.init->data_type)
		n->data.var_decl.init->data_type = n->data_type;

	LLVMValueRef init_const = NULL;
	int needs_runtime_init = 0;
	if (n->data.var_decl.init) {
		if (!global_init_is_constant(c, n->data.var_decl.init))
			needs_runtime_init = 1;
		else
			init_const = const_eval_global_init(c, n->data.var_decl.init,
												g_type, n->data_type);
		if (!needs_runtime_init && !init_const) {
			kerr(KAWA_E_SEMANTIC, n, "global initializer is not a valid constant (overflow or invalid operation)");
			exit(1);
		}
	}

	LLVMValueRef global =
		LLVMAddGlobal(c->module, g_type, n->data.var_decl.name);
	LLVMSetInitializer(global,
					   init_const ? init_const : LLVMConstNull(g_type));
	// Whole-program model: every global lives and dies inside this module.
	// Internal linkage lets LLVM see that -- and on Mach-O it decides where
	// zero storage goes: external globals land in __DATA,__common, which
	// costs real time at first touch for huge arrays; internal ones get
	// proper .zerofill __DATA,__bss, same as static C.
	LLVMSetLinkage(global, LLVMInternalLinkage);
	LLVMSetAlignment(global, 16);

	// `const x = ...` at file scope: the value never changes, so say so.
	// This lets the optimizer fold loads and keep the global in registers.
	if (n->data.var_decl.is_const && !n->data.var_decl.is_orbit && !needs_runtime_init)
		LLVMSetGlobalConstant(global, 1);

	// Register so function bodies resolve the name to this storage.
	scope_push(c, n->data.var_decl.name, global, g_type, n);

	if (needs_runtime_init) {
		PendingGlobalInit *p = arena_alloc(c->arena, sizeof(PendingGlobalInit));
		p->decl = n;
		p->global = global;
		p->g_type = g_type;
		p->next = *pending;
		*pending = p;
	}
}

// Emit kawa_globals_init after all user functions exist, so initializers may
// call any of them. Declaration order is preserved by walking the pending
// list back to front.
static void emit_runtime_global_inits(KawaCompiler *c,
									  PendingGlobalInit *pending,
									  LLVMValueRef *init_fn,
									  LLVMTypeRef *init_fn_type) {
	if (!pending)
		return;

	// Reverse into source order.
	PendingGlobalInit *rev = NULL;
	for (PendingGlobalInit *p = pending; p; p = p->next) {
		PendingGlobalInit *cell =
			arena_alloc(c->arena, sizeof(PendingGlobalInit));
		*cell = *p;
		cell->next = rev;
		rev = cell;
	}

	*init_fn_type = LLVMFunctionType(LLVMVoidTypeInContext(c->context), NULL,
									 0, 0);
	*init_fn = LLVMAddFunction(c->module, "kawa_globals_init", *init_fn_type);
	unsigned nw_id = LLVMGetEnumAttributeKindForName("nounwind", 8);
	LLVMAddAttributeAtIndex(*init_fn, LLVMAttributeFunctionIndex,
							LLVMCreateEnumAttribute(c->context, nw_id, 0));
	LLVMPositionBuilderAtEnd(c->builder,
							 kawa_append_block(*init_fn, "entry"));

	LLVMValueRef saved_func = c->current_func;
	LLVMTypeRef saved_ret = c->current_ret_type;
	c->current_func = *init_fn;
	c->current_ret_type = *init_fn_type;

	for (PendingGlobalInit *p = rev; p; p = p->next) {
		ASTNode *n = p->decl;
		LLVMValueRef val = codegen_expr(c, n->data.var_decl.init);
		val = coerce_value(c, val, n->data.var_decl.init->data_type,
						   p->g_type, n->data_type);
		LLVMValueRef store = LLVMBuildStore(c->builder, val, p->global);
		attach_tbaa(c, store, p->g_type);
	}

	if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder)))
		LLVMBuildRetVoid(c->builder);

	c->current_func = saved_func;
	c->current_ret_type = saved_ret;
}

static void register_enum_constructor(KawaCompiler *c, LLVMValueRef fn, ASTNode *en,
                                     EnumVariant *variant, Type *type) {
	ASTNode *decl = arena_alloc(c->arena, sizeof(*decl));
	decl->type = NODE_FUNC_DECL;
	decl->line = en->line;
	decl->data.func.name = arena_strdup(c->arena, LLVMGetValueName(fn));
	decl->data.func.ret_type = type;
	decl->data.func.is_pure = decl->data.func.is_noalloc = 1;
	ASTNode **tail = &decl->data.func.args;
	for (int i=0; i<variant->payload_count; ++i) {
		ASTNode *arg = arena_alloc(c->arena, sizeof(*arg));
		arg->type = NODE_VAR_DECL;
		arg->data_type = variant->payload_types[i];
		char name[24]; snprintf(name,sizeof(name),"payload%d",i);
		arg->data.var_decl.name = arena_strdup(c->arena,name);
		*tail = arg; tail = &arg->next;
	}
	FunctionSignature *sig = arena_alloc(c->arena, sizeof(*sig));
	sig->function = fn; sig->declaration = decl;
	sig->next = c->function_signatures; c->function_signatures = sig;
	LLVMAddTargetDependentFunctionAttr(fn,"kawa.pure","true");
	LLVMAddTargetDependentFunctionAttr(fn,"kawa.noalloc","true");
}

static int reserved_symbol(const char *name) {
    return name && (!strncmp(name,"__kawa_",7) || !strcmp(name,"kawa_trap") ||
        !strcmp(name,"kawa_main") || !strcmp(name,"kawa_globals_init"));
}
static void emit_enum_constructors(KawaCompiler *c, ASTNode *en) {
	const char *enum_name = en->data.enum_decl.name;
	Type *en_type = arena_alloc(c->arena,sizeof(*en_type));
	en_type->kind = TYPE_ENUM;
	en_type->name = (char *)enum_name;
	LLVMTypeRef llvm_en_type = get_llvm_type(c, en_type);

	for (EnumVariant *ev = en->data.enum_decl.variants; ev; ev = ev->next) {
		char mangled[256];
		snprintf(mangled, sizeof(mangled), "%s_%s", enum_name, ev->name);
        if (reserved_symbol(mangled) || reserved_symbol(ev->name)) {
            kerr(KAWA_E_TYPE,en,"enum constructor name is reserved for compiler runtime symbols");
            exit(1);
        }

		LLVMTypeRef param_ts[16];
		for (int i = 0; i < ev->payload_count; i++) {
			param_ts[i] = get_llvm_type(c, ev->payload_types[i]);
		}
		LLVMTypeRef fn_t = LLVMFunctionType(llvm_en_type, param_ts, ev->payload_count, 0);

		// Emit qualified constructor: Shape_Circle
		LLVMValueRef fn = LLVMGetNamedFunction(c->module, mangled);
		if (!fn) {
			fn = LLVMAddFunction(c->module, mangled, fn_t);
			register_enum_constructor(c,fn,en,ev,en_type);
			LLVMSetLinkage(fn, LLVMInternalLinkage);
			unsigned ai_id = LLVMGetEnumAttributeKindForName("alwaysinline", 12);
			LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex, LLVMCreateEnumAttribute(c->context, ai_id, 0));

			LLVMBasicBlockRef prev_bb = LLVMGetInsertBlock(c->builder);
			LLVMBasicBlockRef entry = kawa_append_block(fn, "entry");
			LLVMPositionBuilderAtEnd(c->builder, entry);

			LLVMValueRef alloca_s = LLVMBuildAlloca(c->builder, llvm_en_type, "enum_val");
			LLVMBuildStore(c->builder,LLVMConstNull(llvm_en_type),alloca_s);
			LLVMValueRef tag_ptr = LLVMBuildStructGEP2(c->builder, llvm_en_type, alloca_s, 0, "tag_ptr");
			LLVMBuildStore(c->builder, LLVMConstInt(LLVMInt64TypeInContext(c->context), (unsigned long long)ev->tag, 0), tag_ptr);

			if (ev->payload_count > 0) {
				LLVMValueRef payload_ptr = LLVMBuildStructGEP2(c->builder, llvm_en_type, alloca_s, 1, "payload_raw");
				LLVMTypeRef payload_struct_t = LLVMStructTypeInContext(c->context, param_ts, ev->payload_count, 0);
				LLVMValueRef typed_payload = LLVMBuildPointerCast(c->builder, payload_ptr,
					LLVMPointerType(payload_struct_t, 0), "typed_payload");
				for (int i = 0; i < ev->payload_count; i++) {
					LLVMValueRef param_val = LLVMGetParam(fn, i);
					LLVMValueRef fld_ptr = LLVMBuildStructGEP2(c->builder, payload_struct_t, typed_payload, i, "fld_ptr");
					LLVMBuildStore(c->builder, param_val, fld_ptr);
				}
			}

			LLVMValueRef res = LLVMBuildLoad2(c->builder, llvm_en_type, alloca_s, "res");
			LLVMBuildRet(c->builder, res);
			if (prev_bb)
				LLVMPositionBuilderAtEnd(c->builder, prev_bb);
		}

		// Also emit unqualified alias if not already defined
		LLVMValueRef bare_fn = LLVMGetNamedFunction(c->module, ev->name);
		if (!bare_fn) {
			bare_fn = LLVMAddFunction(c->module, ev->name, fn_t);
			register_enum_constructor(c,bare_fn,en,ev,en_type);
			LLVMSetLinkage(bare_fn, LLVMInternalLinkage);
			unsigned ai_id = LLVMGetEnumAttributeKindForName("alwaysinline", 12);
			LLVMAddAttributeAtIndex(bare_fn, LLVMAttributeFunctionIndex, LLVMCreateEnumAttribute(c->context, ai_id, 0));

			LLVMBasicBlockRef prev_bb = LLVMGetInsertBlock(c->builder);
			LLVMBasicBlockRef entry = kawa_append_block(bare_fn, "entry");
			LLVMPositionBuilderAtEnd(c->builder, entry);

			LLVMValueRef alloca_s = LLVMBuildAlloca(c->builder, llvm_en_type, "enum_val");
			LLVMBuildStore(c->builder,LLVMConstNull(llvm_en_type),alloca_s);
			LLVMValueRef tag_ptr = LLVMBuildStructGEP2(c->builder, llvm_en_type, alloca_s, 0, "tag_ptr");
			LLVMBuildStore(c->builder, LLVMConstInt(LLVMInt64TypeInContext(c->context), (unsigned long long)ev->tag, 0), tag_ptr);

			if (ev->payload_count > 0) {
				LLVMValueRef payload_ptr = LLVMBuildStructGEP2(c->builder, llvm_en_type, alloca_s, 1, "payload_raw");
				LLVMTypeRef payload_struct_t = LLVMStructTypeInContext(c->context, param_ts, ev->payload_count, 0);
				LLVMValueRef typed_payload = LLVMBuildPointerCast(c->builder, payload_ptr,
					LLVMPointerType(payload_struct_t, 0), "typed_payload");
				for (int i = 0; i < ev->payload_count; i++) {
					LLVMValueRef param_val = LLVMGetParam(bare_fn, i);
					LLVMValueRef fld_ptr = LLVMBuildStructGEP2(c->builder, payload_struct_t, typed_payload, i, "fld_ptr");
					LLVMBuildStore(c->builder, param_val, fld_ptr);
				}
			}

			LLVMValueRef res = LLVMBuildLoad2(c->builder, llvm_en_type, alloca_s, "res");
			LLVMBuildRet(c->builder, res);
			if (prev_bb)
				LLVMPositionBuilderAtEnd(c->builder, prev_bb);
		}
	}
}

void kawa_compile(KawaCompiler *c, ASTNode *root) {
	c->program_root = root; // comptime fn lookup
    ASTNode *cur = root->next;
    for (ASTNode *n=cur; n; n=n->next) {
        const char *name=n->type==NODE_FUNC_DECL ? n->data.func.name :
            n->type==NODE_EXTERN_FN ? n->data.extern_fn.name :
            n->type==NODE_VAR_DECL ? n->data.var_decl.name : NULL;
        if (reserved_symbol(name)) {
            kerr(KAWA_E_TYPE,n,"name is reserved for compiler runtime symbols");
            exit(1);
        }
    }

	// Pass 1: Forward-declare named structs + register aliases. Aliases are
	// resolved lazily through resolve_alias_type, so we only need to track
	// the target Type* here.
	for (ASTNode *scanner = cur; scanner; scanner = scanner->next) {
		if (scanner->type == NODE_STRUCT_DECL) {
			if (!LLVMGetTypeByName(c->module, scanner->data.struct_decl.name))
				LLVMStructCreateNamed(c->context,
									  scanner->data.struct_decl.name);
		}
		if (scanner->type == NODE_ALIAS)
			register_alias(c, scanner->data.alias.name, scanner->data_type);
	}

	// Pass 2: Define struct bodies + populate the field table in one go.
	for (ASTNode *scanner = cur; scanner; scanner = scanner->next) {
		if (scanner->type != NODE_STRUCT_DECL)
			continue;
		LLVMTypeRef struct_t =
			LLVMGetTypeByName(c->module, scanner->data.struct_decl.name);

		int field_count = 0;
		for (ASTNode *f = scanner->data.struct_decl.fields; f; f = f->next) {
			field_count++;
		}

		LLVMTypeRef *elem_types =
			arena_alloc(c->arena, sizeof(LLVMTypeRef) *
									  (field_count > 0 ? field_count : 1));
		int idx = 0;
		for (ASTNode *f = scanner->data.struct_decl.fields; f; f = f->next)
			elem_types[idx++] = get_llvm_type(c, f->data_type);
		LLVMStructSetBody(struct_t, elem_types, field_count, 0);

		register_struct(c, scanner->data.struct_decl.name, struct_t,
						scanner->data.struct_decl.fields);
	}

	// Pass 2.5: Emit enum types and constructor functions
	for (ASTNode *scanner = cur; scanner; scanner = scanner->next) {
		if (scanner->type == NODE_ENUM_DECL) {
			emit_enum_constructors(c, scanner);
		}
	}

	// Function overloading: find bare names declared more than once so
	// those functions emit under mangled symbols and call sites resolve
	// by argument type. Runs before emission so codegen_func_decl sees it.
	collect_overloads(c, cur);
	collect_impl_methods(c, cur);
	// Pass 3: Globals first (function bodies may reference them), then
	// functions. Constant-initialized globals are done here; runtime-
	// initialized ones are queued and emitted after all functions exist so
	// their initializers can call any user function.
	LLVMValueRef globals_init_fn = NULL;
	LLVMTypeRef globals_init_type = NULL;
	PendingGlobalInit *pending_inits = NULL;

	for (ASTNode *scanner = cur; scanner; scanner = scanner->next) {
		if (scanner->type == NODE_VAR_DECL)
			codegen_global_decl(c, scanner, &pending_inits);
		else if (scanner->type == NODE_IMPL_BLOCK) {
			for (ASTNode *m = scanner->data.impl.methods; m; m = m->next) {
				if (m->type == NODE_VAR_DECL)
					codegen_global_decl(c, m, &pending_inits);
			}
		}
	}

	// Snapshot the file-level scope (globals) so coroutine bodies can
	// reference them without seeing the spawning function's locals.
	c->global_scope = c->scope_stack;

	// Generic detection (IDEAS 2.2): a fn whose return or param type
	// mentions `T` (a single-uppercase-letter "struct" type) is generic.
	// Such fns are registered here and instantiated per call site with a
	// mangled name -- no IR is emitted for the template itself.
	for (ASTNode *g = cur; g; g = g->next) {
		if (g->type != NODE_FUNC_DECL || c->generic_fn_count >= 64)
			continue;
		Type *sig[32];
		int sn = 0;
		if (g->data.func.ret_type && sn < 32)
			sig[sn++] = g->data.func.ret_type;
		for (ASTNode *a = g->data.func.args; a && sn < 32; a = a->next)
			if (a->data_type)
				sig[sn++] = a->data_type;
		int is_generic = 0;
		for (int ti = 0; ti < sn && !is_generic; ti++) {
			Type *ty = sig[ti];
			while (ty &&
                   (ty->kind == TYPE_ARRAY || ty->kind == TYPE_SLICE ||
                    ty->kind == TYPE_PTR || ty->kind == TYPE_AMP || ty->kind==TYPE_OWNER ||
                    ty->kind==TYPE_REF || ty->kind==TYPE_CHAN || ty->kind==TYPE_SET))
				ty = ty->inner;
			if (ty && ty->kind == TYPE_STRUCT && ty->name &&
				strlen(ty->name) == 1 && ty->name[0] == 'T')
				is_generic = 1;
		}
		if (is_generic)
			c->generic_fns[c->generic_fn_count++] = g;
	}

	while (cur) {
		if (cur->type == NODE_EXTERN_FN) {
			if (!strncmp(cur->data.extern_fn.name, "__kawa_mem_", 11) ||
				kawa_contains_managed(c, cur->data.extern_fn.ret_type, 0)) {
				kerr(KAWA_E_TYPE, cur, "extern declarations cannot expose managed representations");
				exit(1);
			}
			for (ASTNode *a = cur->data.extern_fn.args; a; a = a->next)
				if (kawa_contains_managed(c, a->data_type, 0)) {
					kerr(KAWA_E_TYPE, a, "extern parameters cannot expose managed representations");
					exit(1);
				}
			// Declare the C symbol with its exact prototype. External
			// linkage, no body: the linker resolves it from any library
			// on the link line -- no header translation needed.
			LLVMTypeRef ret_t = LLVMInt32TypeInContext(c->context);
			if (cur->data.extern_fn.ret_type)
				ret_t = get_llvm_type(c, cur->data.extern_fn.ret_type);
			int argc = 0;
			for (ASTNode *a = cur->data.extern_fn.args; a; a = a->next)
				argc++;
			LLVMTypeRef *params =
				arena_alloc(c->arena, sizeof(LLVMTypeRef) * (argc > 0 ? argc : 1));
			int pi = 0;
			for (ASTNode *a = cur->data.extern_fn.args; a; a = a->next)
				params[pi++] = get_llvm_type(c, a->data_type);
			LLVMTypeRef fn_t =
				LLVMFunctionType(ret_t, params, (unsigned)argc,
								 (int)cur->data.extern_fn.is_variadic);
			LLVMValueRef fn =
				LLVMAddFunction(c->module, cur->data.extern_fn.name, fn_t);
			// A declared-but-never-called extern costs nothing; marking
			// nounwind lets the optimizer treat calls as leaf ops.
			const char *nw = "nounwind";
			LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex,
									LLVMCreateEnumAttribute(
										c->context,
										LLVMGetEnumAttributeKindForName(
											nw, strlen(nw)),
										0));
			cur = cur->next;
			continue;
		}
		if (cur->type == NODE_FUNC_DECL) {
			// Generic fn: registered earlier, instantiated at call sites.
			int skip_generic = 0;
			for (int gi3 = 0; gi3 < c->generic_fn_count; gi3++)
				if (c->generic_fns[gi3] == cur)
					skip_generic = 1;
			if (skip_generic) {
				cur = cur->next;
				continue;
			}
			if (cur->data.func.is_test &&
				c->test_fn_count < 256)
				c->test_fns[c->test_fn_count++] = cur;
			codegen_func_decl(c, cur, NULL);
		} else if (cur->type == NODE_IMPL_BLOCK) {
			for (ASTNode *method = cur->data.impl.methods; method;
				 method = method->next) {
				if (method->type == NODE_FUNC_DECL)
					codegen_func_decl(c, method, cur->data.impl.struct_name);
			}
		}
		cur = cur->next;
	}

	emit_runtime_global_inits(c, pending_inits, &globals_init_fn,
							  &globals_init_type);


	// Test mode: synthesize kawa__run_all_tests() -- calls each #[test] fn
	// in order, prints PASS/FAIL, returns the failure count. @main then
	// calls the runner instead of user main; exit code is the failures.
	if (c->test_mode && c->test_fn_count > 0) {
		LLVMTypeRef i32_t = LLVMInt32TypeInContext(c->context);
		LLVMTypeRef i8ptr =
			LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
		LLVMTypeRef printf_t = LLVMFunctionType(
			i32_t, (LLVMTypeRef[]){i8ptr}, 1, 1);
		LLVMValueRef printf_fn = LLVMGetNamedFunction(c->module, "printf");
		if (!printf_fn)
			printf_fn = LLVMAddFunction(c->module, "printf", printf_t);

		LLVMTypeRef runner_t = LLVMFunctionType(i32_t, NULL, 0, 0);
		LLVMValueRef runner =
			LLVMAddFunction(c->module, "kawa__run_all_tests", runner_t);
		LLVMBasicBlockRef rb = kawa_append_block(runner, "entry");
		LLVMPositionBuilderAtEnd(c->builder, rb);

		for (int ti = 0; ti < c->test_fn_count; ti++) {
			ASTNode *tf = c->test_fns[ti];
			// The LLVM function name is what codegen_func_decl created;
			// find it by source name (methods are mangled, tests never are).
			LLVMValueRef tfn = LLVMGetNamedFunction(
				c->module, tf->data.func.name);
			if (!tfn || tf->data.func.is_ignored)
				continue;
			const char *nm = tf->data.func.name;
			LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(tfn), tfn,
						   NULL, 0, "");
			char fmtbuf[64];
			snprintf(fmtbuf, sizeof(fmtbuf),
					 "PASS %s\n", nm);
			if (getenv("KAWA_NO_PRINTF"))
				continue;
			// LLVMBuildGlobalStringPtr is the same path user string
			// literals take: it creates a properly-typed private constant
			// and folds to i8* without any manual GEP arithmetic.
			LLVMValueRef fmt_ptr =
				LLVMBuildGlobalStringPtr(c->builder, fmtbuf, "passmsg");
			LLVMBuildCall2(c->builder, printf_t, printf_fn,
						   (LLVMValueRef[]){fmt_ptr}, 1, "");
		}
		LLVMBuildRet(c->builder,
					 LLVMConstInt(LLVMInt32TypeInContext(c->context), 0, 0));
	}

	// If user `main` was renamed kawa_main (any signature that isn't
	// exactly (i32 argc, ptr argv)), synthesize the real entry point:
	//   i32 @main(i32 argc, ptr argv) { return kawa_main(); }
	LLVMValueRef renamed = LLVMGetNamedFunction(c->module, "kawa_main");
	LLVMValueRef test_runner =
		c->test_mode ? LLVMGetNamedFunction(c->module,
											"kawa__run_all_tests")
					 : NULL;
	int wrapper_handled_globals_init = 0;
	if (c->test_mode && test_runner && !renamed) {
		// Tests without a user main: entry calls the runner directly.
		LLVMTypeRef i32_t = LLVMInt32TypeInContext(c->context);
		LLVMTypeRef i8ptr =
			LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
		LLVMValueRef wrapper = LLVMAddFunction(
			c->module, "main",
			LLVMFunctionType(i32_t, (LLVMTypeRef[]){i32_t, i8ptr}, 2, 0));
		LLVMBasicBlockRef bb = kawa_append_block(wrapper, "entry");
		LLVMPositionBuilderAtEnd(c->builder, bb);
		if (globals_init_fn) {
			LLVMBuildCall2(c->builder, globals_init_type, globals_init_fn,
						   NULL, 0, "");
			wrapper_handled_globals_init = 1;
		}
		LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(test_runner),
					   test_runner, NULL, 0, "");
		if (c->uses_print) {
			LLVMValueRef flush_fn1 = declare_kawa_runtime_fn(c, "__kawa_flush");
			if (flush_fn1)
				LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(flush_fn1), flush_fn1, NULL, 0, "");
		}
		LLVMBuildRet(c->builder,
					 LLVMConstInt(LLVMInt32TypeInContext(c->context), 0, 0));
	} else if (renamed) {
		LLVMTypeRef i32_t = LLVMInt32TypeInContext(c->context);
		LLVMTypeRef i8ptr =
			LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
		LLVMTypeRef params[] = {i32_t, i8ptr};
		LLVMTypeRef main_t = LLVMFunctionType(i32_t, params, 2, 0);
		LLVMValueRef wrapper = LLVMAddFunction(c->module, "main", main_t);
		LLVMBasicBlockRef bb = kawa_append_block(wrapper, "entry");
		LLVMPositionBuilderAtEnd(c->builder, bb);
		// The globals-init injection below targets @main's entry; build the
		// call AFTER positioning so it lands inside this new block.
		if (globals_init_fn) {
			LLVMBuildCall2(c->builder, globals_init_type, globals_init_fn,
						   NULL, 0, "");
			wrapper_handled_globals_init = 1;
		}
		LLVMValueRef result;
		if (test_runner) {
			result = LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(test_runner),
				test_runner, NULL, 0, "tests_result");
		} else {
			LLVMValueRef args[] = {LLVMGetParam(wrapper, 0), LLVMGetParam(wrapper, 1)};
			unsigned argc = LLVMCountParams(renamed);
			if (argc == 2 && c->main_argv_views) {
				LLVMTypeRef i64_t = LLVMInt64TypeInContext(c->context);
				LLVMTypeRef view_t = LLVMStructTypeInContext(c->context, (LLVMTypeRef[]){i8ptr, i64_t}, 2, 0);
				LLVMValueRef count = LLVMBuildZExt(c->builder, args[0], i64_t, "argc64");
				LLVMValueRef views = LLVMBuildArrayAlloca(c->builder, view_t, count, "argv_views");
				LLVMBasicBlockRef start = LLVMGetInsertBlock(c->builder);
				LLVMBasicBlockRef loop = kawa_append_block(wrapper, "argv_loop");
				LLVMBasicBlockRef body = kawa_append_block(wrapper, "argv_body");
				LLVMBasicBlockRef done = kawa_append_block(wrapper, "argv_done");
				LLVMBuildBr(c->builder, loop);
				LLVMPositionBuilderAtEnd(c->builder, loop);
				LLVMValueRef idx = LLVMBuildPhi(c->builder, i64_t, "argv_i");
				LLVMValueRef zero = LLVMConstNull(i64_t);
				LLVMAddIncoming(idx, &zero, &start, 1);
				LLVMBuildCondBr(c->builder, LLVMBuildICmp(c->builder, LLVMIntULT, idx, count, "argv_more"), body, done);
				LLVMPositionBuilderAtEnd(c->builder, body);
				LLVMValueRef slot = LLVMBuildGEP2(c->builder, i8ptr, args[1], &idx, 1, "arg_slot");
				LLVMValueRef str = LLVMBuildLoad2(c->builder, i8ptr, slot, "arg");
				LLVMValueRef lenfn = LLVMGetNamedFunction(c->module, "strlen");
				if (!lenfn) lenfn = LLVMAddFunction(c->module, "strlen", LLVMFunctionType(i64_t, &i8ptr, 1, 0));
				LLVMValueRef len = LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(lenfn), lenfn, &str, 1, "arg_len");
				LLVMValueRef view = LLVMBuildInsertValue(c->builder, LLVMGetUndef(view_t), str, 0, "arg_data");
				view = LLVMBuildInsertValue(c->builder, view, len, 1, "arg_view");
				LLVMValueRef dest = LLVMBuildGEP2(c->builder, view_t, views, &idx, 1, "view_slot");
				LLVMBuildStore(c->builder, view, dest);
				LLVMValueRef next = LLVMBuildAdd(c->builder, idx, LLVMConstInt(i64_t, 1, 0), "argv_next");
				LLVMBuildBr(c->builder, loop);
				LLVMAddIncoming(idx, &next, &body, 1);
				LLVMPositionBuilderAtEnd(c->builder, done);
				args[1] = views;
			}
			result = LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(renamed), renamed, args, argc, "");
		}
		if (c->uses_print) {
			LLVMValueRef flush_fn2 = declare_kawa_runtime_fn(c, "__kawa_flush");
			if (flush_fn2)
				LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(flush_fn2), flush_fn2, NULL, 0, "");
		}
		LLVMBuildRet(c->builder, LLVMGetTypeKind(LLVMTypeOf(result)) == LLVMVoidTypeKind ? LLVMConstNull(i32_t) : coerce_value(c, result, c->main_ret_ast, i32_t, NULL));
	}

	// Capture OS argc/argv for std.process.arg_count/arg_at. If a user
	// main has the (i32, ptr) shape its params ARE the entry's; otherwise
	// the synthesized wrapper receives them. Either way the values live in
	// @main's first two params (or don't exist -> globals stay null and
	// arg_count() returns 0).
	{
		LLVMValueRef main_fn = LLVMGetNamedFunction(c->module, "main");
		if (main_fn && LLVMCountParams(main_fn) >= 2) {
			LLVMTypeRef i32_t = LLVMInt32TypeInContext(c->context);
			LLVMTypeRef i8t =
				LLVMInt8TypeInContext(c->context);
			LLVMValueRef argc_g = LLVMGetNamedGlobal(c->module,
													 "__kawa_argc");
			if (!argc_g) {
				argc_g = LLVMAddGlobal(c->module, i32_t, "__kawa_argc");
				LLVMSetInitializer(argc_g, LLVMConstNull(i32_t));
				LLVMSetLinkage(argc_g, LLVMPrivateLinkage);
			}
			LLVMValueRef argv_g = LLVMGetNamedGlobal(c->module,
													 "__kawa_argv");
			if (!argv_g) {
				argv_g = LLVMAddGlobal(
					c->module, LLVMPointerType(LLVMPointerType(i8t, 0), 0),
					"__kawa_argv");
				LLVMSetInitializer(
					argv_g,
					LLVMConstNull(LLVMPointerType(
						LLVMPointerType(i8t, 0), 0)));
				LLVMSetLinkage(argv_g, LLVMPrivateLinkage);
			}
			LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(main_fn);
			LLVMBuilderRef tmp = LLVMCreateBuilderInContext(c->context);
			LLVMValueRef first = LLVMGetFirstInstruction(entry);
			if (first)
				LLVMPositionBuilderBefore(tmp, first);
			else
				LLVMPositionBuilderAtEnd(tmp, entry);
			LLVMBuildStore(tmp, LLVMGetParam(main_fn, 0), argc_g);
			LLVMBuildStore(tmp, LLVMGetParam(main_fn, 1), argv_g);
			LLVMDisposeBuilder(tmp);
		}
	}

	// Inject a call to kawa_globals_init at the top of main() so runtime-
	// initialized globals are ready before any user code runs. Constant-
	// initialized globals need no call at all. (Skipped when the synthesized
	// kawa_main wrapper already emitted the call -- it would run twice.)
	if (globals_init_fn && !wrapper_handled_globals_init) {
		LLVMValueRef main_fn = LLVMGetNamedFunction(c->module, "main");
		if (main_fn) {
			LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(main_fn);
			LLVMBuilderRef tmp = LLVMCreateBuilderInContext(c->context);
			LLVMValueRef first = LLVMGetFirstInstruction(entry);
			if (first)
				LLVMPositionBuilderBefore(tmp, first);
			else
				LLVMPositionBuilderAtEnd(tmp, entry);
			LLVMBuildCall2(tmp, globals_init_type, globals_init_fn, NULL, 0,
						   "");
			LLVMDisposeBuilder(tmp);
		}
	}

	if (c->uses_memory) {
		LLVMMemoryBufferRef memory = LLVMCreateMemoryBufferWithMemoryRange(
			(const char *)(c->memory_metrics ? kawa_memory_metrics_bc : kawa_memory_bc),
			c->memory_metrics ? kawa_memory_metrics_bc_len : kawa_memory_bc_len, "kawa_memory", 0);
		LLVMModuleRef runtime = NULL;
		if (LLVMParseBitcodeInContext2(c->context, memory, &runtime)) {
			kdiag_error_at(KAWA_E_SEMANTIC, c->source_filename, NULL, 0,
				"could not link the managed-memory runtime");
			exit(1);
		}
		runtime_target(c,runtime);
		if (LLVMLinkModules2(c->module,runtime)) {
			kdiag_error_at(KAWA_E_SEMANTIC,c->source_filename,NULL,0,"could not link the managed-memory runtime");
			exit(1);
		}
		LLVMDisposeMemoryBuffer(memory);
	}
	if (c->uses_print) {
		LLVMMemoryBufferRef rt_mem = LLVMCreateMemoryBufferWithMemoryRange(
			(const char *)kawa_runtime_bc, kawa_runtime_bc_len, "kawa_runtime", 0);
		LLVMModuleRef rt_mod = NULL;
		if (!LLVMParseBitcodeInContext2(c->context, rt_mem, &rt_mod)) {
			runtime_target(c,rt_mod);
			LLVMLinkModules2(c->module, rt_mod);
		}
        LLVMDisposeMemoryBuffer(rt_mem);
    }
    /* Reserved compiler helpers have no external callers. Internal linkage
     * enables specialization and removes unused runtime entry points while
     * public source functions retain their ABI. */
    for (LLVMValueRef fn=LLVMGetFirstFunction(c->module); fn; fn=LLVMGetNextFunction(fn)) {
        if (LLVMCountBasicBlocks(fn) && !strncmp(LLVMGetValueName(fn),"__kawa_",7) &&
            !LLVMGetStringAttributeAtIndex(fn,LLVMAttributeFunctionIndex,"kawa.source",11))
            LLVMSetLinkage(fn,LLVMInternalLinkage);
    }
}

static const uint32_t K256[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static inline uint32_t sha256_rotr(uint32_t x, uint32_t n) {
	return (x >> n) | (x << (32 - n));
}

static void sha256_transform(uint32_t state[8], const uint8_t data[64]) {
	uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
	uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
	uint32_t w[64];
	for (int i = 0; i < 16; i++) {
		w[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
			   ((uint32_t)data[i * 4 + 2] << 8) | ((uint32_t)data[i * 4 + 3]);
	}
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = sha256_rotr(w[i - 15], 7) ^ sha256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = sha256_rotr(w[i - 2], 17) ^ sha256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	for (int i = 0; i < 64; i++) {
		uint32_t S1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
		uint32_t ch = (e & f) ^ ((~e) & g);
		uint32_t temp1 = h + S1 + ch + K256[i] + w[i];
		uint32_t S0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t temp2 = S0 + maj;

		h = g;
		g = f;
		f = e;
		e = d + temp1;
		d = c;
		c = b;
		b = a;
		a = temp1 + temp2;
	}
	state[0] += a; state[1] += b; state[2] += c; state[3] += d;
	state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void compute_sha256_hex(const unsigned char *data, size_t len, char out_hex[65]) {
	uint32_t state[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
	};
	size_t rem = len;
	const uint8_t *p = data;
	while (rem >= 64) {
		sha256_transform(state, p);
		p += 64;
		rem -= 64;
	}
	uint8_t buf[128];
	memset(buf, 0, sizeof(buf));
	memcpy(buf, p, rem);
	buf[rem] = 0x80;
	size_t pad_len = (rem < 56) ? 64 : 128;
	uint64_t total_bits = (uint64_t)len * 8;
	for (int i = 0; i < 8; i++) {
		buf[pad_len - 1 - i] = (uint8_t)(total_bits >> (i * 8));
	}
	sha256_transform(state, buf);
	if (pad_len == 128)
		sha256_transform(state, buf + 64);

	for (int i = 0; i < 8; i++) {
		snprintf(out_hex + i * 8, 9, "%08x", state[i]);
	}
	out_hex[64] = '\0';
}

void kawa_optimize_and_write(KawaCompiler *c, const char *filename) {
	kawa_di_finalize(c); // Finish metadata before verification or optimization.

	// Verify the module *before* any optimization runs. This catches
	// malformed metadata, type mismatches, and structural IR errors with
	// the most precise diagnostics (post-optimization errors are harder to
	// attribute).
	{
		char *error = NULL;
		if (LLVMVerifyModule(c->module, LLVMPrintMessageAction, &error)) {
			if (!c->check_only && getenv("KAWA_DUMP_BAD"))
				LLVMPrintModuleToFile(c->module, "tmp/bad2.ll", NULL);
			kdiag_error_at(KAWA_E_SEMANTIC,
						   c->source_filename ? c->source_filename : "<kawa>",
						   NULL, 0, "LLVM module verification failed:\n%s",
						   error);
			LLVMDumpModule(c->module);
			LLVMDisposeMessage(error);
			exit(1);
		}
		if (error)
			LLVMDisposeMessage(error);
	}

	char *error_msg = NULL;
	LLVMTargetMachineRef machine = c->target_machine;
	kawa_verify_safety(c, machine);
	if (c->check_only) {
		LLVMDisposeTargetMachine(machine);
		LLVMDisposeTargetData(c->target_data);
		c->target_machine = NULL;
		c->target_data = NULL;
		return;
    }

    KawaOptimizationSnapshot *report_before=kawa_report_snapshot(c);
    // Coroutine transforms must run before the main pipeline so coro-split
	// lowers the frame before inlining decisions are made. The pass
	// pipeline follows the -O level: O0 skips optimization entirely, O1/O2
	// use LLVM's curated defaults (O2 is kawac's default), and O3 layers
	// aggressive vectorization + unrolling on top.
    LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
    char pipeline[2048] = "";
    if (!getenv("KAWA_NO_OPT")) {
		if (c->pgo_use) {
			char pgo_opt[1024];
			snprintf(pgo_opt, sizeof(pgo_opt), "-pgo-test-profile-file=%s", c->pgo_use);
			const char *pgo_argv[] = { "kawac", pgo_opt };
			LLVMParseCommandLineOptions(2, pgo_argv, "");
		}

		if (c->pgo_gen) {
			strcat(pipeline, "pgo-instr-gen,instrprof,");
		} else if (c->pgo_use) {
			strcat(pipeline, "pgo-instr-use,");
		}
		strcat(pipeline, "coro-early,coro-split,coro-elide,coro-cleanup");

		switch (c->opt_level) {
		case 0:
			break;
		case 1:
			strcat(pipeline, ",default<O1>");
			break;
		case 3:
			strcat(pipeline, ",default<O3>");
			break;
		default:
			strcat(pipeline, ",default<O2>");
			break;
		}
		if (c->enable_lto || c->opt_level == 3) {
			int lto_lvl = c->opt_level > 0 ? c->opt_level : 2;
			char lto_buf[32];
			snprintf(lto_buf, sizeof(lto_buf), ",lto<O%d>", lto_lvl);
			strcat(pipeline, lto_buf);
		}
		if (c->opt_level >= 2)
			strcat(pipeline, ",function(sroa,instcombine,simplifycfg)");
		LLVMErrorRef pass_error = LLVMRunPasses(c->module, pipeline, machine, opts);
		if (pass_error) {
			char *message = LLVMGetErrorMessage(pass_error);
			kdiag_error_at(KAWA_E_SEMANTIC, c->source_filename, NULL, 0, "optimization failed: %s", message);
			LLVMDisposeErrorMessage(message);
			exit(1);
		}
	}
    LLVMDisposePassBuilderOptions(opts);
    kawa_report_write(c,report_before,*pipeline ? pipeline : "disabled by KAWA_NO_OPT");

    if (LLVMWriteBitcodeToFile(c->module, filename) != 0) {
		kdiag_error_at(KAWA_E_SEMANTIC,
					   c->source_filename ? c->source_filename : "<kawa>", NULL,
                       0, "error writing bitcode");
        exit(1);
    }
	if (LLVMPrintModuleToFile(c->module, "output.ll", &error_msg)) {
		kdiag_error_at(KAWA_E_SEMANTIC,
					   c->source_filename ? c->source_filename : "<kawa>", NULL,
					   0, "writing file failed: %s", error_msg);
        LLVMDisposeMessage(error_msg);
        exit(1);
	}

	// Emit the object file directly through the same TargetMachine. This
	// keeps DWARF sections (clang's .bc pipeline was dropping them) and
	// gives the driver something to link without recompiling LLVM IR.
	const char *obj_path = "output.o";
	if (LLVMTargetMachineEmitToFile(machine, c->module, obj_path,
									LLVMObjectFile, &error_msg)) {
		kdiag_error_at(KAWA_E_SEMANTIC,
					   c->source_filename ? c->source_filename : "<kawa>", NULL,
					   0, "emitting object failed: %s", error_msg);
        LLVMDisposeMessage(error_msg);
        exit(1);
	}

	if (c->emit_hash) {
		FILE *f = fopen(filename, "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			long sz = ftell(f);
			fseek(f, 0, SEEK_SET);
			unsigned char *buf = malloc(sz);
			if (buf && fread(buf, 1, sz, f) == (size_t)sz) {
				char hash_hex[65];
				compute_sha256_hex(buf, (size_t)sz, hash_hex);
				printf("hash: %s\n", hash_hex);
			}
			free(buf);
			fclose(f);
		}
	}

	LLVMDisposeTargetMachine(machine);
	LLVMDisposeTargetData(c->target_data);
	c->target_machine = NULL;
	c->target_data = NULL;
}
