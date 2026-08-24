#include "codegen_internal.h"

static LLVMValueRef codegen_short_circuit(KawaCompiler *c, ASTNode *n);

void trigger_orbit_updates(KawaCompiler *c, ASTNode *origin_node) {
	if (!origin_node || !origin_node->dependents)
		return;
	for (Dependency *dep = origin_node->dependents; dep; dep = dep->next) {
		LLVMValueRef new_val = codegen_expr(c, dep->logic_expr);
		// dep->dependent_node is a var_decl AST node. The scope is
		// guaranteed to exist because the dependent was registered when
		// the var_decl was lowered.
		Scope *s = scope_find(c, dep->dependent_node->data.var_decl.name);
		if (s) {
			LLVMValueRef store = LLVMBuildStore(c->builder, new_val, s->val);
			attach_tbaa(c, store, s->type);
		}
		trigger_orbit_updates(c, dep->dependent_node);
	}
}

// Sign- and width-correct integer arithmetic. NSW/NUW flags give the
// optimizer extra freedom (vectorization, reassociation) without changing
// semantics for well-defined Kawa programs.
static LLVMValueRef build_int_binop(KawaCompiler *c, int op, LLVMValueRef l,
									LLVMValueRef r, int lhs_signed,
									int rhs_signed) {
	int both_unsigned = !lhs_signed && !rhs_signed;
	switch (op) {
	case TOK_PLUS:
		if (both_unsigned)
			return LLVMBuildNUWAdd(c->builder, l, r, "add");
		return LLVMBuildNSWAdd(c->builder, l, r, "add");
	case TOK_MINUS:
		if (both_unsigned)
			return LLVMBuildNUWSub(c->builder, l, r, "sub");
		return LLVMBuildNSWSub(c->builder, l, r, "sub");
	case TOK_STAR:
		if (both_unsigned)
			return LLVMBuildNUWMul(c->builder, l, r, "mul");
		return LLVMBuildNSWMul(c->builder, l, r, "mul");
	case TOK_SLASH:
		// sdiv on an unsigned operand with the high bit set is wrong. When
		// types are mixed we follow the RHS's signedness (the usual rule in
		// C-like languages for `x / literal`).
		if (both_unsigned || !rhs_signed)
			return LLVMBuildUDiv(c->builder, l, r, "udiv");
		return LLVMBuildSDiv(c->builder, l, r, "sdiv");
	case TOK_PERCENT:
		// Remainder follows the same signedness rule as division.
		if (both_unsigned || !rhs_signed)
			return LLVMBuildURem(c->builder, l, r, "urem");
		return LLVMBuildSRem(c->builder, l, r, "srem");
	default:
		return NULL;
	}
}

static LLVMValueRef declare_libc_fn(KawaCompiler *c, const char *name);

// Resolve the callee of a call expression to a module-level function name.
// Handles plain calls (`foo()`), stdc passthrough (`stdc.printf`),
// type-qualified calls (`User__add`) and method sugar already mangled by
// the parser.
static LLVMValueRef resolve_callee(KawaCompiler *c, ASTNode *callee, char *out,
								   size_t out_size) {
	out[0] = '\0';
	if (callee->type == NODE_VAR_REF) {
		const char *nm = callee->data.var_ref.name;
		if (strlen(nm) >= out_size) {
			timbr_err("Function name too long\n");
			exit(1);
		}
		strcpy(out, nm);
	} else if (callee->type == NODE_MEMBER_ACCESS) {
		ASTNode *obj = callee->data.member_access.object;
		const char *member = callee->data.member_access.member;
		if (strlen(member) >= out_size) {
			timbr_err("Function name too long\n");
			exit(1);
		}
		if (obj->type == NODE_VAR_REF &&
			strcmp(obj->data.var_ref.name, "stdc") == 0) {
			strcpy(out, member);
		} else if (obj->type == NODE_VAR_REF) {
			snprintf(out, out_size, "%s__%s", obj->data.var_ref.name, member);
		} else {
			snprintf(out, out_size, "%s__%s", resolve_type_name(c, obj),
					 member);
		}
	} else {
		return NULL;
	}
	return LLVMGetNamedFunction(c->module, out);
}

LLVMValueRef codegen_expr(KawaCompiler *c, ASTNode *n) {
	if (!n)
		return LLVMConstInt(LLVMInt32TypeInContext(c->context), 0, 0);

	switch (n->type) {

	case NODE_LITERAL: {
		if (n->data_type &&
			(n->data_type->kind == TYPE_F32 || n->data_type->kind == TYPE_F64))
			return LLVMConstReal(get_llvm_type(c, n->data_type),
								 (double)n->data.literal.f_val);
		// Integer literals are i32; coerce_value widens/truncates at the
		// point of use when the context demands another width.
		return LLVMConstInt(LLVMInt32TypeInContext(c->context),
							n->data.literal.i_val,
							n->data_type ? type_is_signed(c, n->data_type) : 0);
	}

	case NODE_SIZEOF: {
		LLVMTypeRef measured = NULL;
		if (n->data.size_of.type_val)
			measured = get_llvm_type(c, n->data.size_of.type_val);
		else if (n->data.size_of.value && n->data.size_of.value->data_type)
			measured = get_llvm_type(c, n->data.size_of.value->data_type);

		if (measured) {
			LLVMValueRef size = LLVMSizeOf(measured);
			return LLVMBuildTruncOrBitCast(c->builder, size,
										   LLVMInt32TypeInContext(c->context),
										   "sizeof_cast");
		}
		timbr_err("sizeof: cannot determine type\n");
		exit(1);
	}

	case NODE_STRING_LIT:
		return LLVMBuildGlobalStringPtr(c->builder, n->data.str_lit.s_val,
										"str");

	case NODE_VAR_REF:
	case NODE_MEMBER_ACCESS:
	case NODE_INDEX:
	case NODE_DEREF:
	case NODE_AMP:
		return value_of_lvalue(c, n);

	case NODE_CALL: {
		char func_name[256];
		LLVMValueRef fn = resolve_callee(c, n->data.call.callee, func_name,
										 sizeof(func_name));
		if (!fn) {
			if (strcmp(func_name, "printf") == 0) {
				LLVMTypeRef args[] = {
					LLVMPointerType(LLVMInt8TypeInContext(c->context), 0)};
				fn = LLVMAddFunction(
					c->module, "printf",
					LLVMFunctionType(LLVMInt32TypeInContext(c->context), args,
									 1, 1));
			} else if (strcmp(func_name, "malloc") == 0) {
				fn = c->malloc_fn;
			} else if (strcmp(func_name, "free") == 0) {
				fn = c->free_fn;
			} else if (n->data.call.callee->type == NODE_MEMBER_ACCESS &&
					   n->data.call.callee->data.member_access.object->type ==
						   NODE_VAR_REF &&
					   strcmp(n->data.call.callee->data.member_access.object
								  ->data.var_ref.name,
							  "stdc") == 0) {
				// stdc.<anything>: declare it and call through. The C
				// library is the runtime surface; every libc symbol should
				// just work without per-symbol stubs. Known signatures get
				// exact prototypes (a wrong one is UB -- e.g. a variadic
				// decl of strcpy miscompiles on arm64); everything else is
				// assumed `i32 f(ptr, ...)` which covers printf-style use.
				fn = declare_libc_fn(c, func_name);
				if (!fn) {
					LLVMTypeRef fn_t = LLVMFunctionType(
						LLVMInt32TypeInContext(c->context),
						(LLVMTypeRef[]){LLVMPointerType(
							LLVMInt8TypeInContext(c->context), 0)},
						1, 1);
					fn = LLVMAddFunction(c->module, func_name, fn_t);
				}
			} else {
				char *f_path = get_var_path(c, func_name);
				timbr_err("Undefined function: %s\n", f_path);
				exit(1);
			}
		}

		int arg_count = 0;
		for (ASTNode *a = n->data.call.args; a; a = a->next)
			arg_count++;

		LLVMTypeRef func_type = LLVMGlobalGetValueType(fn);
		int param_count = LLVMCountParamTypes(func_type);

		// Named arguments: `f(y: 2, x: 1)` -- match labels to parameter
		// names and reorder into positional slots. Mixed positional/named
		// is allowed as long as every named arg finds its slot; anything
		// unmatched is an error (no defaults in v1). Zero runtime cost:
		// this is a compile-time permutation of the argument list.
		if (n->data.call.args &&
			n->data.call.args->has_arg_label) {
			// Build the reordered chain by param index.
			ASTNode **reord =
				arena_alloc(c->arena, sizeof(ASTNode *) * (arg_count > 0 ? arg_count : 1));
			for (int s = 0; s < arg_count; s++)
				reord[s] = NULL;
			int used[256] = {0};
			int pos = 0;
			int ok = 1;
			for (ASTNode *a = n->data.call.args; a; a = a->next, pos++) {
				const char *label = a->has_arg_label ? a->arg_label : NULL;
				if (!label) {
					// Positional in a mixed call: keep relative order among
					// positionals is NOT guaranteed with named present --
					// v1 rule: if any arg is named, all must be named.
					ok = 0;
					break;
				}
				int matched = -1;
				for (int p_i = 0; p_i < param_count; p_i++) {
					LLVMValueRef pv = LLVMGetParam(fn, p_i);
					if (!pv)
						continue;
					size_t sz = 0;
					const char *pn = LLVMGetValueName2(pv, &sz);
					if (pn && strlen(pn) == strlen(label) &&
						strncmp(pn, label, strlen(label)) == 0) {
						matched = p_i;
						break;
					}
				}
				if (matched < 0 || matched >= 256 || used[matched]) {
					timbr_err("No unique parameter '%s' in call\n", label);
					exit(1);
				}
				used[matched] = 1;
				reord[matched] = a;
			}
			if (!ok) {
				timbr_err("If any argument is named, all must be named\n");
				exit(1);
			}
			// Relink the chain in parameter order.
			ASTNode *new_head = NULL;
			ASTNode **new_tail = &new_head;
			for (int p_i = 0; p_i < param_count; p_i++) {
				if (!reord[p_i])
					continue;
				*new_tail = reord[p_i];
				new_tail = &(*new_tail)->next;
			}
			*new_tail = NULL;
			n->data.call.args = new_head;
			arg_count = 0;
			for (ASTNode *a = new_head; a; a = a->next)
				arg_count++;
		}

		size_t args_bytes =
			sizeof(LLVMValueRef) * (arg_count > 0 ? arg_count : 1);
		size_t params_bytes =
			sizeof(LLVMTypeRef) * (param_count > 0 ? param_count : 1);
		char *blob = arena_alloc(c->arena, args_bytes + params_bytes);
		LLVMValueRef *llvm_args = (LLVMValueRef *)blob;
		LLVMTypeRef *param_types = (LLVMTypeRef *)(blob + args_bytes);
		if (param_count > 0)
			LLVMGetParamTypes(func_type, param_types);

		ASTNode *arg_node = n->data.call.args;
		for (int i = 0; i < arg_count; i++) {
			LLVMValueRef val = codegen_expr(c, arg_node);

			if (i < param_count) {
				LLVMTypeRef expected = param_types[i];

				// Auto-spill: function expects a pointer (e.g. i8* self)
				// but caller is passing a struct value. Spill to a local
				// alloca and pass its address.
				if (LLVMGetTypeKind(expected) == LLVMPointerTypeKind &&
					LLVMGetTypeKind(LLVMTypeOf(val)) == LLVMStructTypeKind) {
					LLVMValueRef temp_alloc = create_entry_block_alloca(
						c, LLVMTypeOf(val), "self_temp");
					LLVMBuildStore(c->builder, val, temp_alloc);
					val = temp_alloc;
				}
				val = coerce_value(c, val, arg_node->data_type, expected, NULL);
			} else {
				// Vararg slot: C varargs require integer promotion to i32
				// and float promotion to f64.
				LLVMTypeRef vt = LLVMTypeOf(val);
				LLVMTypeKind k = LLVMGetTypeKind(vt);
				if (k == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(vt) < 32)
					val = LLVMBuildZExt(c->builder, val,
										LLVMInt32TypeInContext(c->context),
										"vararg_prom");
				else if (k == LLVMFloatTypeKind)
					val = LLVMBuildFPExt(c->builder, val,
										 LLVMDoubleTypeInContext(c->context),
										 "float_prom");
			}
			llvm_args[i] = val;
			arg_node = arg_node->next;
		}
		LLVMValueRef call = LLVMBuildCall2(c->builder, func_type, fn,
										   llvm_args, arg_count, "");
		// Self-recursion in tail position: mark it so the backend emits a
		// jmp instead of call+ret (no stack growth on tail-recursive loops).
		// Only safe when this call is the whole result of the function --
		// approximated by: callee == current function and the call is not
		// inside a coroutine body.
		if (fn == c->current_func && !c->in_coroutine)
			LLVMSetTailCall(call, true);
		return call;
	}

	case NODE_STRUCT_LITERAL: {
		LLVMTypeRef s_type = get_llvm_type(c, n->data_type);
		if (!s_type) {
			timbr_err("Internal error: literal missing type\n");
			exit(1);
		}

		// Array literal: { e0, e1, ... } with TYPE_ARRAY context.
		if (LLVMGetTypeKind(s_type) == LLVMArrayTypeKind) {
			LLVMTypeRef elem_t = LLVMGetElementType(s_type);
			LLVMValueRef alloca =
				create_entry_block_alloca(c, s_type, "arr_lit");
			int idx = 0;
			for (StructInitItem *item = n->data.struct_lit.items; item;
				 idx++, item = item->next) {
				LLVMValueRef val = codegen_expr(c, item->value);
				val =
					coerce_value(c, val, item->value->data_type, elem_t, NULL);
				LLVMValueRef gep = LLVMBuildStructGEP2(c->builder, s_type,
													   alloca, idx, "elem");
				LLVMBuildStore(c->builder, val, gep);
			}
			return LLVMBuildLoad2(c->builder, s_type, alloca, "arr_val");
		}

		if (LLVMGetTypeKind(s_type) != LLVMStructTypeKind) {
			timbr_err("Internal error: struct literal missing type\n");
			exit(1);
		}

		LLVMValueRef alloca = create_entry_block_alloca(c, s_type, "lit");
		StructInitItem *item = n->data.struct_lit.items;
		for (int idx = 0; item; idx++, item = item->next) {
			LLVMValueRef val = codegen_expr(c, item->value);
			int field_idx;
			LLVMTypeRef field_ty;
			if (item->field_name) {
				field_idx = get_field_index(c, s_type, item->field_name);
				field_ty = get_field_type(c, s_type, item->field_name);
			} else {
				// Positional init: use the running index.
				field_idx = idx;
				field_ty = LLVMStructGetTypeAtIndex(s_type, idx);
			}
			LLVMValueRef gep = LLVMBuildStructGEP2(c->builder, s_type, alloca,
												   field_idx, "fld");
			val = coerce_value(c, val, item->value->data_type, field_ty, NULL);
			LLVMBuildStore(c->builder, val, gep);
		}
		return LLVMBuildLoad2(c->builder, s_type, alloca, "lit_val");
	}

	case NODE_BINARY_OP:
		// Short-circuit logical ops need custom control flow -- the RHS
		// must not be evaluated unless the LHS demands it.
		if (n->data.bin_op.op == TOK_ANDAND || n->data.bin_op.op == TOK_OROR)
			return codegen_short_circuit(c, n);
		return build_binop(c, n, codegen_expr(c, n->data.bin_op.left),
						   codegen_expr(c, n->data.bin_op.right));

	case NODE_SET_POUR: {
		LLVMTypeRef ignored;
		LLVMValueRef set_ptr =
			get_address(c, n->data.set_pour.target, &ignored);
		LLVMValueRef val_to_add = codegen_expr(c, n->data.set_pour.value);

		// Set layout: { i32* buf, i64 len, i64 cap } -- must match
		// get_llvm_type's TYPE_SET case.
		LLVMContextRef ctx = c->context;
		LLVMTypeRef i32_ptr_t = LLVMPointerType(LLVMInt32TypeInContext(ctx), 0);
		LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx);
		LLVMTypeRef set_struct_t = LLVMStructTypeInContext(
			ctx, (LLVMTypeRef[]){i32_ptr_t, i64_t, i64_t}, 3, 0);

		LLVMValueRef buf_gep =
			LLVMBuildStructGEP2(c->builder, set_struct_t, set_ptr, 0, "buf_p");
		LLVMValueRef cnt_gep =
			LLVMBuildStructGEP2(c->builder, set_struct_t, set_ptr, 1, "cnt_p");
		LLVMValueRef cap_gep =
			LLVMBuildStructGEP2(c->builder, set_struct_t, set_ptr, 2, "cap_p");

		LLVMValueRef cur_cnt =
			LLVMBuildLoad2(c->builder, i64_t, cnt_gep, "cur_cnt");
		LLVMValueRef cur_cap =
			LLVMBuildLoad2(c->builder, i64_t, cap_gep, "cur_cap");

		LLVMValueRef is_full =
			LLVMBuildICmp(c->builder, LLVMIntUGE, cur_cnt, cur_cap, "is_full");

		LLVMBasicBlockRef grow_bb =
			LLVMAppendBasicBlock(c->current_func, "set_grow");
		LLVMBasicBlockRef append_bb =
			LLVMAppendBasicBlock(c->current_func, "set_append");

		LLVMValueRef br =
			LLVMBuildCondBr(c->builder, is_full, grow_bb, append_bb);
		set_branch_weights(c, br, 1, 99); // growth is rare

		// --- GROW BLOCK (Realloc) ---
		LLVMPositionBuilderAtEnd(c->builder, grow_bb);
		LLVMValueRef new_cap = LLVMBuildMul(
			c->builder, cur_cap, LLVMConstInt(i64_t, 2, 0), "new_cap");
		LLVMValueRef new_bytes = LLVMBuildMul(
			c->builder, new_cap, LLVMConstInt(i64_t, 4, 0), "new_bytes");
		LLVMValueRef old_buf =
			LLVMBuildLoad2(c->builder, i32_ptr_t, buf_gep, "old_buf");
		LLVMValueRef new_mem = LLVMBuildCall2(
			c->builder, c->realloc_type, c->realloc_fn,
			(LLVMValueRef[]){LLVMBuildPointerCast(
								 c->builder, old_buf,
								 LLVMPointerType(LLVMInt8TypeInContext(ctx), 0),
								 "void_ptr"),
							 new_bytes},
			2, "new_mem");
		LLVMBuildStore(c->builder,
					   LLVMBuildPointerCast(c->builder, new_mem, i32_ptr_t,
											"new_buf_cast"),
					   buf_gep);
		LLVMBuildStore(c->builder, new_cap, cap_gep);
		LLVMBuildBr(c->builder, append_bb);

		// --- APPEND BLOCK ---
		LLVMPositionBuilderAtEnd(c->builder, append_bb);
		LLVMValueRef final_buf =
			LLVMBuildLoad2(c->builder, i32_ptr_t, buf_gep, "final_buf");
		LLVMValueRef slot =
			LLVMBuildGEP2(c->builder, LLVMInt32TypeInContext(ctx), final_buf,
						  &cur_cnt, 1, "slot");
		LLVMBuildStore(c->builder, val_to_add, slot);
		LLVMValueRef next_cnt = LLVMBuildNUWAdd(
			c->builder, cur_cnt, LLVMConstInt(i64_t, 1, 0), "next_cnt");
		LLVMBuildStore(c->builder, next_cnt, cnt_gep);

		return val_to_add;
	}

	case NODE_SET_LITERAL: {
		LLVMTypeRef set_t = get_llvm_type(c, n->data_type);
		LLVMContextRef ctx = c->context;
		LLVMValueRef set_alloca =
			create_entry_block_alloca(c, set_t, "set_tmp");

		int count = 0;
		for (ASTNode *cur = n->data.set_lit.items; cur; cur = cur->next)
			count++;

		LLVMValueRef size = LLVMConstInt(LLVMInt64TypeInContext(ctx),
										 count > 0 ? count * 4 : 4, 0);
		LLVMValueRef buf_void = LLVMBuildCall2(
			c->builder, c->malloc_type, c->malloc_fn, &size, 1, "malloc");
		LLVMValueRef buf = LLVMBuildPointerCast(
			c->builder, buf_void,
			LLVMPointerType(LLVMInt32TypeInContext(ctx), 0), "buf_cast");

		int idx = 0;
		for (ASTNode *cur = n->data.set_lit.items; cur;
			 cur = cur->next, idx++) {
			LLVMValueRef val = codegen_expr(c, cur);
			val = coerce_value(c, val, cur->data_type,
							   LLVMInt32TypeInContext(ctx), NULL);
			LLVMValueRef gep =
				LLVMBuildGEP2(c->builder, LLVMInt32TypeInContext(ctx), buf,
							  (LLVMValueRef[]){LLVMConstInt(
								  LLVMInt64TypeInContext(ctx), idx, 0)},
							  1, "ptr");
			LLVMValueRef store = LLVMBuildStore(c->builder, val, gep);
			attach_tbaa(c, store, LLVMInt32TypeInContext(ctx));
		}

		LLVMBuildStore(
			c->builder, buf,
			LLVMBuildStructGEP2(c->builder, set_t, set_alloca, 0, ""));
		LLVMBuildStore(
			c->builder, LLVMConstInt(LLVMInt64TypeInContext(ctx), count, 0),
			LLVMBuildStructGEP2(c->builder, set_t, set_alloca, 1, ""));
		LLVMBuildStore(
			c->builder,
			LLVMConstInt(LLVMInt64TypeInContext(ctx), count > 0 ? count : 1, 0),
			LLVMBuildStructGEP2(c->builder, set_t, set_alloca, 2, ""));
		LLVMValueRef set_load =
			LLVMBuildLoad2(c->builder, set_t, set_alloca, "set_load");
		attach_tbaa(c, set_load, set_t);
		return set_load;
	}

	case NODE_SIP: {
		LLVMValueRef hdl = codegen_expr(c, n->data.sip.handle);
		LLVMValueRef is_done = LLVMBuildCall2(c->builder, c->coro_done_type,
											  c->coro_done, &hdl, 1, "is_done");
		LLVMBasicBlockRef resume_bb =
			LLVMAppendBasicBlock(c->current_func, "sip_resume");
		LLVMBasicBlockRef cont_bb =
			LLVMAppendBasicBlock(c->current_func, "sip_cont");
		LLVMBuildCondBr(c->builder, is_done, cont_bb, resume_bb);
		LLVMPositionBuilderAtEnd(c->builder, resume_bb);
		LLVMBuildCall2(c->builder, c->coro_resume_type, c->coro_resume, &hdl, 1,
					   "");
		LLVMBuildBr(c->builder, cont_bb);
		LLVMPositionBuilderAtEnd(c->builder, cont_bb);
		LLVMValueRef promise_ptr_void = LLVMBuildCall2(
			c->builder, c->coro_promise_type, c->coro_promise,
			(LLVMValueRef[]){
				hdl,
				LLVMConstInt(LLVMInt32TypeInContext(c->context),
							 c->drip_promise_index, 0),
				LLVMConstInt(LLVMInt1TypeInContext(c->context), 0, 0)},
			3, "prom_ptr_void");
		LLVMValueRef promise_ptr = LLVMBuildPointerCast(
			c->builder, promise_ptr_void,
			LLVMPointerType(LLVMInt32TypeInContext(c->context), 0), "prom_ptr");
		LLVMValueRef val =
			LLVMBuildLoad2(c->builder, LLVMInt32TypeInContext(c->context),
						   promise_ptr, "sip_val");
		LLVMSetVolatile(val, 1);
		attach_tbaa(c, val, LLVMInt32TypeInContext(c->context));
		return val;
	}

	case NODE_CAST: {
		LLVMValueRef val = codegen_expr(c, n->data.cast.val);
		LLVMTypeRef dest_type = get_llvm_type(c, n->data_type);
		return coerce_value(c, val, n->data.cast.val->data_type, dest_type,
							n->data_type);
	}

	case NODE_BREW:
		return codegen_brew(c, n);

	default:
		break;
	}

	timbr_err("Internal error: unknown AST node type in codegen_expr (%d)\n",
			  n->type);
	exit(1);
}

// Short-circuit evaluation for && and ||. Emits a branch so the RHS is
// only evaluated when the LHS doesn't decide the result -- the same shape
// clang produces, which lets the optimizer flatten it later.
static LLVMValueRef codegen_short_circuit(KawaCompiler *c, ASTNode *n) {
	int is_and = (n->data.bin_op.op == TOK_ANDAND);
	LLVMValueRef func = c->current_func;

	LLVMValueRef lhs = cond_to_bool(c, codegen_expr(c, n->data.bin_op.left));

	LLVMBasicBlockRef lhs_end = LLVMGetInsertBlock(c->builder);
	LLVMBasicBlockRef rhs_bb =
		LLVMAppendBasicBlock(func, is_and ? "and_rhs" : "or_rhs");
	LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlock(func, "bool_merge");
	LLVMBuildCondBr(c->builder, lhs, is_and ? rhs_bb : merge_bb,
					is_and ? merge_bb : rhs_bb);

	LLVMPositionBuilderAtEnd(c->builder, rhs_bb);
	LLVMValueRef rhs = cond_to_bool(c, codegen_expr(c, n->data.bin_op.right));
	LLVMBasicBlockRef rhs_end = LLVMGetInsertBlock(c->builder);
	LLVMBuildBr(c->builder, merge_bb);

	LLVMPositionBuilderAtEnd(c->builder, merge_bb);
	LLVMValueRef phi =
		LLVMBuildPhi(c->builder, LLVMInt1TypeInContext(c->context), "sc_val");
	LLVMAddIncoming(phi, (LLVMValueRef[]){lhs, rhs},
					(LLVMBasicBlockRef[]){lhs_end, rhs_end}, 2);
	return phi;
}


// Relational operators on `str` become strcmp(...) OP 0 -- content
// semantics, not pointer identity (identical literals dedupe to one global,
// so raw pointer == "works" for them and silently miscompares runtime
// strings).
static LLVMValueRef build_strcmp_call(KawaCompiler *c, LLVMValueRef l,
									  LLVMValueRef r) {
	LLVMTypeRef i8ptr =
		LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
	LLVMTypeRef fn_t =
		LLVMFunctionType(LLVMInt32TypeInContext(c->context),
						 (LLVMTypeRef[]){i8ptr, i8ptr}, 2, 0);
	LLVMValueRef fn = LLVMGetNamedFunction(c->module, "strcmp");
	if (!fn)
		fn = LLVMAddFunction(c->module, "strcmp", fn_t);
	LLVMValueRef args[2] = {l, r};
	return LLVMBuildCall2(c->builder, fn_t, fn, args, 2, "str_cmp");
}

// Exact prototypes for common libc functions used via stdc.*. A mismatched
// declaration is UB -- e.g. declaring strcpy variadic miscompiles on arm64
// because the backend routes varargs calls through a different ABI path.
static LLVMValueRef declare_libc_fn(KawaCompiler *c, const char *name) {
	LLVMContextRef ctx = c->context;
	LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
	LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx);
	LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx);
	// Existing definition/declaration wins.
	if (LLVMGetNamedFunction(c->module, name))
		return NULL;
	struct {
		const char *name;
		LLVMTypeRef ret;
		LLVMTypeRef params[4];
		unsigned n;
	} table[] = {
		{"strcpy", i8ptr, {i8ptr, i8ptr}, 2},
		{"strncpy", i8ptr, {i8ptr, i8ptr, i64}, 3},
		{"strcat", i8ptr, {i8ptr, i8ptr}, 2},
		{"strcmp", i32, {i8ptr, i8ptr}, 2},
		{"strncmp", i32, {i8ptr, i8ptr, i64}, 3},
		{"strlen", i64, {i8ptr}, 1},
		{"strchr", i8ptr, {i8ptr, i32}, 2},
		{"strstr", i8ptr, {i8ptr, i8ptr}, 2},
		{"memset", i8ptr, {i8ptr, i32, i64}, 3},
		{"memcpy", i8ptr, {i8ptr, i8ptr, i64}, 3},
		{"memmove", i8ptr, {i8ptr, i8ptr, i64}, 3},
		{"memcmp", i32, {i8ptr, i8ptr, i64}, 3},
		{"puts", i32, {i8ptr}, 1},
	};
	for (unsigned k = 0; k < sizeof(table) / sizeof(table[0]); k++) {
		if (strcmp(table[k].name, name) != 0)
			continue;
		return LLVMAddFunction(
			c->module, name,
			LLVMFunctionType(table[k].ret, table[k].params, table[k].n, 0));
	}
	return NULL;
}

// A Kawa `str` is ptr<char>; both sides being char-pointers means the user
// wrote a relational operator on strings.
static int str_relational(KawaCompiler *c, ASTNode *side) {
	Type *t = NULL;
	if (side && side->data_type) {
		t = side->data_type;
	} else if (side && side->type == NODE_VAR_REF) {
		// Bare VAR_REFs carry no parser-side type; resolve through scope
		// like NODE_INDEX does for array indices.
		Scope *sv = scope_find(c, side->data.var_ref.name);
		t = sv ? sv->node->data_type : NULL;
	}
	if (!t || t->kind != TYPE_PTR || !t->inner)
		return 0;
	return t->inner->kind == TYPE_CHAR;
}

LLVMValueRef build_binop(KawaCompiler *c, ASTNode *n, LLVMValueRef l,
						 LLVMValueRef r) {
	int op = n->data.bin_op.op;

	LLVMTypeRef l_ty = LLVMTypeOf(l);
	LLVMTypeRef r_ty = LLVMTypeOf(r);

	// String relations: == != < > <= >= become strcmp(...) OP 0. Content
	// semantics, not pointer identity. (String literals with identical
	// contents dedupe to one global, so raw pointer == "works" for them --
	// and silently miscompares anything built at runtime.)
	switch (op) {
	case TOK_ISEQ:
	case TOK_NOTEQ:
	case TOK_LANGLE:
	case TOK_RANGLE:
	case TOK_LEQ:
	case TOK_REQ:
		if (str_relational(c, n->data.bin_op.left) &&
			str_relational(c, n->data.bin_op.right)) {
			LLVMValueRef cmp = build_strcmp_call(c, l, r);
			return LLVMBuildICmp(c->builder,
								 op == TOK_ISEQ	 ? LLVMIntEQ
								 : op == TOK_NOTEQ ? LLVMIntNE
								 : op == TOK_LANGLE ? LLVMIntSLT
								 : op == TOK_RANGLE ? LLVMIntSGT
								 : op == TOK_LEQ	? LLVMIntSLE
													: LLVMIntSGE,
								 cmp,
								 LLVMConstInt(LLVMInt32TypeInContext(
									 c->context),0,1),
								 "str_rel");
		}
		break;
	default:
		break;
	}

	int l_is_fp = (LLVMGetTypeKind(l_ty) == LLVMFloatTypeKind ||
				   LLVMGetTypeKind(l_ty) == LLVMDoubleTypeKind);
	int r_is_fp = (LLVMGetTypeKind(r_ty) == LLVMFloatTypeKind ||
				   LLVMGetTypeKind(r_ty) == LLVMDoubleTypeKind);

	// Promote Int to Float/Double if mixed (respect source signedness).
	if (l_is_fp && !r_is_fp) {
		r = coerce_value(c, r, n->data.bin_op.right->data_type, l_ty,
						 n->data.bin_op.left->data_type);
		r_is_fp = 1;
		r_ty = l_ty;
	} else if (!l_is_fp && r_is_fp) {
		l = coerce_value(c, l, n->data.bin_op.left->data_type, r_ty,
						 n->data.bin_op.right->data_type);
		l_is_fp = 1;
		l_ty = r_ty;
	}

	// Same-kind FP with different precision: promote float to double.
	if (l_is_fp && r_is_fp &&
		LLVMGetTypeKind(l_ty) != LLVMGetTypeKind(LLVMTypeOf(r))) {
		if (LLVMGetTypeKind(l_ty) == LLVMFloatTypeKind) {
			l = LLVMBuildFPExt(c->builder, l,
							   LLVMDoubleTypeInContext(c->context),
							   "promote_l_dbl");
			l_ty = LLVMTypeOf(l);
		}
		if (LLVMGetTypeKind(LLVMTypeOf(r)) == LLVMFloatTypeKind) {
			r = LLVMBuildFPExt(c->builder, r,
							   LLVMDoubleTypeInContext(c->context),
							   "promote_r_dbl");
		}
	}

	if (l_is_fp) {
		switch (op) {
		case TOK_PLUS:
		case TOK_MINUS:
		case TOK_STAR:
		case TOK_SLASH: {
			LLVMValueRef res;
			switch (op) {
			case TOK_PLUS:
				res = LLVMBuildFAdd(c->builder, l, r, "fadd");
				break;
			case TOK_MINUS:
				res = LLVMBuildFSub(c->builder, l, r, "fsub");
				break;
			case TOK_STAR:
				res = LLVMBuildFMul(c->builder, l, r, "fmul");
				break;
			default:
				res = LLVMBuildFDiv(c->builder, l, r, "fdiv");
				break;
			}
			set_fast_math(res);
			return res;
		}
		case TOK_LANGLE:
			return LLVMBuildFCmp(c->builder, LLVMRealOLT, l, r, "flt");
		case TOK_RANGLE:
			return LLVMBuildFCmp(c->builder, LLVMRealOGT, l, r, "fgt");
		case TOK_LEQ:
			return LLVMBuildFCmp(c->builder, LLVMRealOLE, l, r, "fle");
		case TOK_REQ:
			return LLVMBuildFCmp(c->builder, LLVMRealOGE, l, r, "fge");
		case TOK_ISEQ:
			return LLVMBuildFCmp(c->builder, LLVMRealOEQ, l, r, "feq");
		case TOK_NOTEQ:
			return LLVMBuildFCmp(c->builder, LLVMRealUNE, l, r, "fne");
		default:
			return l;
		}
	}

	// Integer path. Use sign info from AST types where available; bare
	// variable references carry no parser-side type, so resolve (and stamp)
	// from their declaration -- otherwise i32 vars would shift/compare as
	// unsigned.
	if (!n->data.bin_op.left->data_type &&
		n->data.bin_op.left->type == NODE_VAR_REF) {
		Scope *sv = scope_find(c, n->data.bin_op.left->data.var_ref.name);
		if (sv && sv->node && sv->node->data_type)
			n->data.bin_op.left->data_type = sv->node->data_type;
	}
	if (!n->data.bin_op.right->data_type &&
		n->data.bin_op.right->type == NODE_VAR_REF) {
		Scope *sv = scope_find(c, n->data.bin_op.right->data.var_ref.name);
		if (sv && sv->node && sv->node->data_type)
			n->data.bin_op.right->data_type = sv->node->data_type;
	}
	int l_signed = n->data.bin_op.left->data_type
					   ? type_is_signed(c, n->data.bin_op.left->data_type)
					   : 0;
	int r_signed = n->data.bin_op.right->data_type
					   ? type_is_signed(c, n->data.bin_op.right->data_type)
					   : 0;

	// Mixed-width integers: widen the narrower side to the wider type so
	// both LLVM operands match (e.g. u64 + int-literal).
	if (LLVMGetTypeKind(l_ty) == LLVMIntegerTypeKind &&
		LLVMGetTypeKind(r_ty) == LLVMIntegerTypeKind && l_ty != r_ty) {
		unsigned lw = LLVMGetIntTypeWidth(l_ty);
		unsigned rw = LLVMGetIntTypeWidth(r_ty);
		if (lw > rw) {
			r = type_is_signed(c, n->data.bin_op.left->data_type)
					? LLVMBuildSExt(c->builder, r, l_ty, "widen_r")
					: LLVMBuildZExt(c->builder, r, l_ty, "widen_r");
			r_ty = l_ty;
		} else {
			l = type_is_signed(c, n->data.bin_op.right->data_type)
					? LLVMBuildSExt(c->builder, l, r_ty, "widen_l")
					: LLVMBuildZExt(c->builder, l, r_ty, "widen_l");
			l_ty = r_ty;
		}
	}

	switch (op) {
	case TOK_LANGLE:
		return (l_signed || r_signed)
				   ? LLVMBuildICmp(c->builder, LLVMIntSLT, l, r, "lt")
				   : LLVMBuildICmp(c->builder, LLVMIntULT, l, r, "ult");
	case TOK_RANGLE:
		return (l_signed || r_signed)
				   ? LLVMBuildICmp(c->builder, LLVMIntSGT, l, r, "gt")
				   : LLVMBuildICmp(c->builder, LLVMIntUGT, l, r, "ugt");
	case TOK_LEQ:
		return (l_signed || r_signed)
				   ? LLVMBuildICmp(c->builder, LLVMIntSLE, l, r, "le")
				   : LLVMBuildICmp(c->builder, LLVMIntULE, l, r, "ule");
	case TOK_REQ:
		return (l_signed || r_signed)
				   ? LLVMBuildICmp(c->builder, LLVMIntSGE, l, r, "ge")
				   : LLVMBuildICmp(c->builder, LLVMIntUGE, l, r, "uge");
	case TOK_ISEQ:
		return LLVMBuildICmp(c->builder, LLVMIntEQ, l, r, "eq");
	case TOK_NOTEQ:
		return LLVMBuildICmp(c->builder, LLVMIntNE, l, r, "ne");
	case TOK_PLUS:
	case TOK_MINUS:
	case TOK_STAR:
	case TOK_SLASH:
	case TOK_PERCENT: {
		LLVMValueRef res = build_int_binop(c, op, l, r, l_signed, r_signed);
		if (res)
			return res;
		return l;
	}
	case TOK_AMP:
		return LLVMBuildAnd(c->builder, l, r, "and");
	case TOK_PIPE:
		return LLVMBuildOr(c->builder, l, r, "or");
	case TOK_CARET:
		return LLVMBuildXor(c->builder, l, r, "xor");
	case TOK_SHL:
		return LLVMBuildShl(c->builder, l, r, "shl");
	case TOK_SHR:
		// Arithmetic shift for signed operands, logical for unsigned --
		// mirrors C semantics with zero extra instructions.
		return (l_signed || r_signed)
				   ? LLVMBuildAShr(c->builder, l, r, "ashr")
				   : LLVMBuildLShr(c->builder, l, r, "lshr");
	default:
		return l;
	}
}
