#include "codegen_internal.h"

static LLVMValueRef codegen_short_circuit(KawaCompiler *c, ASTNode *n);

// `base[i]` on a struct providing impl `self_index`: evaluate the base's
// ADDRESS (the method mutates through it for self* receivers, and by-value
// receivers copy from the same storage), the index, then call.
LLVMValueRef codegen_index_overload(KawaCompiler *c, ASTNode *n) {
	char mangled[256];
	ASTNode *obj = n->data.index.object;
	Type *bt = index_base_struct_type(c, obj);
	if (!bt)
		return NULL;
	snprintf(mangled, sizeof(mangled), "%s__self_index", bt->name);
	LLVMValueRef fn = LLVMGetNamedFunction(c->module, mangled);
	if (!fn) {
		kerr(KAWA_E_UNDEF, n, "missing `%s` -- index overload not emitted",
			 mangled);
		exit(1);
	}
	LLVMTypeRef fn_t = LLVMGlobalGetValueType(fn);
	LLVMTypeRef base_elem_unused = NULL;
	LLVMValueRef base_addr =
		get_address(c, obj, &base_elem_unused);
	// By-value receivers take the struct VALUE: load from the resolved
	// storage. Pointer receivers (`self*`) take the address as-is -- zero
	// copy either way once the optimizer runs.
	LLVMTypeRef recv_t = LLVMTypeOf(LLVMGetParam(fn, 0));
	LLVMValueRef self_arg =
		LLVMGetTypeKind(recv_t) == LLVMPointerTypeKind
			? base_addr
			: LLVMBuildLoad2(c->builder, recv_t, base_addr, "idx_self");
	LLVMValueRef idx = codegen_expr(c, n->data.index.index);
	// Widen/narrow the index to whatever the method declares (usually
	// i64), matching how ordinary integer args are coerced at call sites.
	// Bare VAR_REF indices carry no parser-side type -- stamp it from the
	// declaration so a signed i32 widens with sext, not zext.
	Type *src_t = n->data.index.index->data_type;
	if (!src_t && n->data.index.index->type == NODE_VAR_REF) {
		Scope *sv = scope_find(
			c, n->data.index.index->data.var_ref.name);
		src_t = (sv && sv->node) ? sv->node->data_type : NULL;
		if (src_t)
			n->data.index.index->data_type = src_t;
	}
	LLVMTypeRef want_t = LLVMTypeOf(LLVMGetParam(fn, 1));
	if (want_t && LLVMGetTypeKind(want_t) == LLVMIntegerTypeKind &&
		LLVMTypeOf(idx) != want_t) {
		Type dst = {0};
		dst.kind = TYPE_I64;
		dst.is_signed = 1; // index slots are signed; sext negatives
		idx = coerce_value(c, idx, src_t, want_t, &dst);
	}
	return LLVMBuildCall2(c->builder, fn_t, fn,
						  (LLVMValueRef[]){self_arg, idx}, 2,
						  "idx_overload");
}

// True when evaluating the expression cannot produce side effects or trap:
// literals, variable reads, member/index reads, pure operators over pure
// operands, and calls to functions declared `pure fn` with pure arguments.
// Used to decide whether a value may be (re)evaluated after a suspension
// point -- e.g. a channel send that blocks must not re-run its operand.
static int expr_is_pure(KawaCompiler *c, ASTNode *n) {
	if (!n)
		return 0;
	switch (n->type) {
	case NODE_LITERAL:
	case NODE_STRING_LIT:
	case NODE_VAR_REF:
		return 1;
	case NODE_BINARY_OP:
		return expr_is_pure(c, n->data.bin_op.left) &&
			   expr_is_pure(c, n->data.bin_op.right);
	case NODE_CAST:
		return expr_is_pure(c, n->data.cast.val);
	case NODE_MEMBER_ACCESS:
		// Slice/chan field reads are plain loads; safe to speculate.
		return expr_is_pure(c, n->data.member_access.object);
	case NODE_INDEX:
		return expr_is_pure(c, n->data.index.object) &&
			   expr_is_pure(c, n->data.index.index);
	case NODE_CALL: {
		if (n->data.call.callee->type != NODE_VAR_REF)
			return 0;
		for (ASTNode *s = c->program_root ? c->program_root->next : NULL;
			 s; s = s->next)
			if (s->type == NODE_FUNC_DECL &&
				strcmp(s->data.func.name,
					   n->data.call.callee->data.var_ref.name) == 0)
				return s->data.func.is_pure &&
					   expr_is_pure(c, n->data.call.args);
		return 0;
	}
	default:
		return 0;
	}
}

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
static void rehome_wide_literal(KawaCompiler *c, ASTNode *n);

static LLVMValueRef build_int_binop(KawaCompiler *c, int op, LLVMValueRef l,
									LLVMValueRef r, int lhs_signed,
									int rhs_signed) {
	int both_unsigned = !lhs_signed && !rhs_signed;
	switch (op) {
	case TOK_PLUS:
		// Unsigned arithmetic WRAPS (C semantics): no nuw flag -- marking
		// `u8 200 + 100` nuw makes the wrap a poison value. Signed keeps
		// nsw to match C's UB and unlock optimizer reasoning.
		return both_unsigned ? LLVMBuildAdd(c->builder, l, r, "add")
							 : LLVMBuildNSWAdd(c->builder, l, r, "add");
	case TOK_MINUS:
		return both_unsigned ? LLVMBuildSub(c->builder, l, r, "sub")
							 : LLVMBuildNSWSub(c->builder, l, r, "sub");
	case TOK_STAR:
		return both_unsigned ? LLVMBuildMul(c->builder, l, r, "mul")
							 : LLVMBuildNSWMul(c->builder, l, r, "mul");
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

static LLVMValueRef declare_std_fn(KawaCompiler *c, const char *qualified);

// Exact prototypes for the std.* process/io surface. Like declare_libc_fn,
// a wrong prototype is UB, so each symbol is spelled out; anything not
// listed returns NULL.
static LLVMTypeRef kawa_std_fn_type(KawaCompiler *c, const char *qualified) {
	LLVMContextRef ctx = c->context;
	LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx);
	LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx);
	LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
	if (strcmp(qualified, "std.process.exit") == 0)
		return LLVMFunctionType(LLVMVoidTypeInContext(ctx),
								(LLVMTypeRef[]){i32}, 1, 0);
	if (strcmp(qualified, "std.io.puts") == 0)
		return LLVMFunctionType(i32, (LLVMTypeRef[]){ptr}, 1, 0);
	if (strcmp(qualified, "std.io.eputs") == 0)
		// macOS exposes `stderr` as a macro over __stderrp, so there is no
		// linkable C symbol; back eputs with write(2) instead (fd 2), and
		// return the byte count like fputs would.
		return LLVMFunctionType(i64,
								(LLVMTypeRef[]){ptr, i64},
								2, 0);
	if (strcmp(qualified, "std.process.arg_count") == 0)
		return LLVMFunctionType(i32, NULL, 0, 0);
	if (strcmp(qualified, "std.process.arg_at") == 0)
		return LLVMFunctionType(LLVMStructTypeInContext(ctx,
			(LLVMTypeRef[]){ptr, i64}, 2, 0), (LLVMTypeRef[]){i32}, 1, 0);
	return NULL;
}

// std.io.puts is libc puts(3) itself: same newline-appending contract,
// goes through stdio so it interleaves correctly with printf. (eputs
// keeps the raw write(2) path -- stderr is unbuffered by C standard.)
static LLVMValueRef declare_puts_std(KawaCompiler *c) {
	LLVMTypeRef i32 = LLVMInt32TypeInContext(c->context);
	LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
	if (!LLVMGetNamedFunction(c->module, "puts"))
		LLVMAddFunction(c->module, "puts",
						LLVMFunctionType(i32, (LLVMTypeRef[]){ptr}, 1, 0));
	return LLVMGetNamedFunction(c->module, "puts");
}

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
			kerr(KAWA_E_SEMANTIC, callee, "function name `%s` too long", nm);
			exit(1);
		}
		strcpy(out, nm);
	} else if (callee->type == NODE_MEMBER_ACCESS) {
		ASTNode *obj = callee->data.member_access.object;
		const char *member = callee->data.member_access.member;
		if (strlen(member) >= out_size) {
			kerr(KAWA_E_SEMANTIC, callee,
				 "method name `%s` too long", member);
			exit(1);
		}
		// std.* / std.*.* functions live in the module under their bare
		// leaf name (`exit`, `puts`); resolve straight past the namespace.
		// Walk down through nested member access to find the root; any
		// path rooted at `std` names a std function.
		if (obj->type == NODE_MEMBER_ACCESS || obj->type == NODE_VAR_REF) {
			ASTNode *root = obj;
			while (root->type == NODE_MEMBER_ACCESS)
				root = root->data.member_access.object;
			if (root->type == NODE_VAR_REF &&
				strcmp(root->data.var_ref.name, "std") == 0) {
				LLVMValueRef f = LLVMGetNamedFunction(c->module, member);
				if (f)
					return f;
				// Not declared yet: hand back the bare leaf name so the
				// call site maps it to its exact prototype.
				strncpy(out, member, out_size - 1);
				out[out_size - 1] = '\0';
				return NULL;
			}
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

// --- Channel helpers (IDEAS 3) ---------------------------------------
// The ring buffer is cooperative-only: a blocked op re-suspends the
// enclosing coroutine via drop{}-style yields. No atomics, no locks --
// on the ready path an op is one load, one store, two counter bumps.

static LLVMValueRef chan_field_ptr(KawaCompiler *c, LLVMTypeRef chan_t,
								   LLVMValueRef chan_addr, int field,
								   const char *name) {
	return LLVMBuildStructGEP2(c->builder, chan_t, chan_addr, field, name);
}

static void chan_yield(KawaCompiler *c) {
	LLVMContextRef ctx = c->context;
	if (!c->in_coroutine || !c->current_coro_hdl)
		return;
	LLVMValueRef save_token =
		LLVMBuildCall2(c->builder, c->coro_save_type, c->coro_save,
					   &c->current_coro_hdl, 1, "save");
	LLVMValueRef suspend = LLVMBuildCall2(
		c->builder, c->coro_suspend_type, c->coro_suspend,
		(LLVMValueRef[]){save_token,
						 LLVMConstInt(LLVMInt1TypeInContext(c->context),
									  0, 0)},
		2, "yield");
	LLVMBasicBlockRef resume_bb =
		kawa_append_block(c->current_func, "chan_resume");
	LLVMValueRef sw =
		LLVMBuildSwitch(c->builder, suspend, c->coro_suspend_block, 2);
	LLVMAddCase(sw, LLVMConstInt(LLVMInt8TypeInContext(c->context), 0, 0),
				resume_bb);
	LLVMAddCase(sw, LLVMConstInt(LLVMInt8TypeInContext(c->context), 1, 0),
				c->coro_cleanup_block);
	LLVMPositionBuilderAtEnd(c->builder, resume_bb);
}


LLVMValueRef codegen_expr(KawaCompiler *c, ASTNode *n) {
	if (!n)
		return LLVMConstInt(LLVMInt32TypeInContext(c->context), 0, 0);

	switch (n->type) {

	case NODE_LITERAL: {
		if (n->data_type && (n->data_type->kind == TYPE_F16 ||
							 n->data_type->kind == TYPE_BF16 ||
							 n->data_type->kind == TYPE_F32 ||
							 n->data_type->kind == TYPE_F64))
			return LLVMConstReal(get_llvm_type(c, n->data_type),
								 (double)n->data.literal.f_val);
		// Integer literals emit at their parser-assigned width (i32/u32/
		// i64/u64); coerce_value widens/truncates where context demands
		// another width. i64_val is authoritative -- it holds values an
		// int cannot (u64 constants, -2147483648, comptime folds).
		unsigned lit_w = 32;
		int lit_s = 1;
		if (n->data_type) {
			lit_s = type_is_signed(c, n->data_type);
			switch (n->data_type->kind) {
			case TYPE_I64: case TYPE_U64: lit_w = 64; break;
			default: break;
			}
		}
		return LLVMConstInt(LLVMIntTypeInContext(c->context, lit_w),
							(unsigned long long)n->data.literal.i64_val,
							lit_s);
	}

	case NODE_BLOCK: {
		for (ASTNode *s = n->data.block.stmts; s; s = s->next)
			codegen_stmt(c, s);
		return LLVMConstInt(LLVMInt32TypeInContext(c->context), 0, 0);
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
		kerr(KAWA_E_TYPE, n, "sizeof: cannot determine type");
		exit(1);
	}

	case NODE_STRING_LIT: {
		// str = {ptr,len} view (IDEAS 3). The constant global keeps the
		// trailing NUL so decaying to char* at C boundaries stays valid;
		// the view's len excludes it. Both fields are constants, so a
		// literal costs nothing at runtime -- same as C, plus O(1) length.
		LLVMContextRef ctx = c->context;
		Type u8t = {0};
		u8t.kind = TYPE_U8;
		size_t slen = n->data.str_lit.len;
		static int str_lit_counter = 0;
		char gname[32];
		snprintf(gname, sizeof(gname), ".strlit.%d", str_lit_counter++);
		LLVMValueRef cstr_init = LLVMConstStringInContext(c->context,
			n->data.str_lit.s_val, (unsigned)slen, 0);
		LLVMTypeRef arr_t = LLVMTypeOf(cstr_init);
		LLVMValueRef global = LLVMAddGlobal(c->module, arr_t, gname);
		LLVMSetGlobalConstant(global, 1);
		LLVMSetLinkage(global, LLVMPrivateLinkage);
		LLVMSetUnnamedAddress(global, LLVMGlobalUnnamedAddr);
		LLVMSetInitializer(global, cstr_init);
		LLVMValueRef data = LLVMBuildGEP2(
			c->builder, arr_t, global,
			(LLVMValueRef[]){LLVMConstInt(LLVMInt64TypeInContext(ctx), 0, 0),
							 LLVMConstInt(LLVMInt32TypeInContext(ctx), 0, 0)},
			2, "str_data");
		Type slice_t = {0};
		slice_t.kind = TYPE_SLICE;
		slice_t.inner = &u8t;
		LLVMTypeRef view_t = get_llvm_type(c, &slice_t);
		LLVMValueRef view = LLVMGetUndef(view_t);
		view = LLVMBuildInsertValue(c->builder, view, data, 0,
									"str_ins_data");
		view = LLVMBuildInsertValue(
			c->builder, view,
			LLVMConstInt(LLVMInt64TypeInContext(ctx),
						 (unsigned long long)slen, 0),
			1, "str_ins_len");
		return view;
	}

	case NODE_MATCH: {
		Type *res_type = n->data_type;
		if (!res_type && n->data.match_stmt.arms && n->data.match_stmt.arms->data.match_arm.body) {
			res_type = n->data.match_stmt.arms->data.match_arm.body->data_type;
		}
		if (!res_type) {
			res_type = arena_alloc(c->arena, sizeof(Type));
			res_type->kind = TYPE_I64;
		}
		LLVMTypeRef llvm_res_type = get_llvm_type(c, res_type);
		LLVMValueRef res_slot = create_entry_block_alloca(c, llvm_res_type, "match_res_slot");
		codegen_match(c, n, res_slot, llvm_res_type);
		return LLVMBuildLoad2(c->builder, llvm_res_type, res_slot, "match_res");
	}

	case NODE_SLICE_INDEX: {
		ASTNode *obj = n->data.slice_index.object;
		LLVMContextRef ctx = c->context;
		LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx);

		LLVMValueRef start_val = NULL;
		if (n->data.slice_index.start) {
			start_val = codegen_expr(c, n->data.slice_index.start);
			start_val = coerce_value(c, start_val, n->data.slice_index.start->data_type, i64_t, NULL);
		} else {
			start_val = LLVMConstInt(i64_t, 0, 0);
		}

		LLVMValueRef base_ptr = NULL;
		LLVMValueRef total_len = NULL;
		LLVMTypeRef elem_t = NULL;

		Type *obj_ast = obj->data_type;
		Scope *s_obj = NULL;
		if (obj->type == NODE_VAR_REF) {
			s_obj = scope_find(c, obj->data.var_ref.name);
			if (s_obj && s_obj->node && s_obj->node->data_type)
				obj_ast = s_obj->node->data_type;
		}

		if (obj_ast && obj_ast->kind == TYPE_ARRAY) {
			elem_t = get_llvm_type(c, obj_ast->inner);
			total_len = LLVMConstInt(i64_t, obj_ast->array_len, 0);
			if (s_obj) {
				base_ptr = s_obj->val;
			} else {
				base_ptr = codegen_expr(c, obj);
			}
			base_ptr = LLVMBuildGEP2(c->builder, elem_t, base_ptr,
				(LLVMValueRef[]){ LLVMConstInt(i64_t, 0, 0) }, 1, "arr_base");
		} else if (obj_ast && obj_ast->kind == TYPE_SLICE) {
			elem_t = get_llvm_type(c, obj_ast->inner);
			LLVMTypeRef slice_llvm_t = get_llvm_type(c, obj_ast);
			LLVMValueRef slice_val = NULL;
			if (s_obj) {
				slice_val = LLVMBuildLoad2(c->builder, slice_llvm_t, s_obj->val, "slice_tmp");
			} else {
				slice_val = codegen_expr(c, obj);
			}
			base_ptr = LLVMBuildExtractValue(c->builder, slice_val, 0, "slice_base");
			total_len = LLVMBuildExtractValue(c->builder, slice_val, 1, "slice_len");
		} else if (obj_ast && obj_ast->kind == TYPE_PTR) {
			elem_t = get_llvm_type(c, obj_ast->inner);
			base_ptr = codegen_expr(c, obj);
			total_len = LLVMConstInt(i64_t, 0x7FFFFFFF, 0);
		} else {
			LLVMValueRef obj_val = codegen_expr(c, obj);
			LLVMTypeRef val_t = LLVMTypeOf(obj_val);
			if (LLVMGetTypeKind(val_t) == LLVMStructTypeKind) {
				base_ptr = LLVMBuildExtractValue(c->builder, obj_val, 0, "slice_base");
				total_len = LLVMBuildExtractValue(c->builder, obj_val, 1, "slice_len");
				elem_t = LLVMGetElementType(LLVMTypeOf(base_ptr));
			} else if (LLVMGetTypeKind(val_t) == LLVMPointerTypeKind) {
				base_ptr = obj_val;
				elem_t = LLVMGetElementType(val_t);
				total_len = LLVMConstInt(i64_t, 0x7FFFFFFF, 0);
			}
		}

		if (!elem_t)
			elem_t = LLVMInt32TypeInContext(ctx);

		LLVMValueRef end_val = NULL;
		if (n->data.slice_index.end) {
			end_val = codegen_expr(c, n->data.slice_index.end);
			end_val = coerce_value(c, end_val, n->data.slice_index.end->data_type, i64_t, NULL);
		} else {
			end_val = total_len;
		}

		if (c->bounds_check_mode != -1 && c->unchecked_depth == 0) {
			LLVMValueRef ordered = LLVMBuildICmp(c->builder, LLVMIntULE,
				start_val, end_val, "slice_ordered");
			LLVMValueRef fits = LLVMBuildICmp(c->builder,
				n->data.slice_index.is_inclusive ? LLVMIntULT : LLVMIntULE,
				end_val, total_len, "slice_fits");
			LLVMValueRef ok = LLVMBuildAnd(c->builder, ordered, fits, "slice_bounds_ok");
			emit_check_or_trap(c, n, ok, "slice range out of bounds");
		}
		LLVMValueRef slice_len = LLVMBuildSub(c->builder, end_val, start_val, "slice_len_raw");
		if (n->data.slice_index.is_inclusive) {
			slice_len = LLVMBuildAdd(c->builder, slice_len, LLVMConstInt(i64_t, 1, 0), "slice_len_inc");
		}

		LLVMValueRef sub_data = LLVMBuildGEP2(c->builder, elem_t, base_ptr, &start_val, 1, "sub_slice_ptr");

		LLVMTypeRef ptr_elem_t = LLVMPointerType(elem_t, 0);
		LLVMTypeRef ret_slice_t = LLVMStructTypeInContext(ctx, (LLVMTypeRef[]){ ptr_elem_t, i64_t }, 2, 0);
		LLVMValueRef res_slice = LLVMGetUndef(ret_slice_t);
		res_slice = LLVMBuildInsertValue(c->builder, res_slice, sub_data, 0, "res_data");
		res_slice = LLVMBuildInsertValue(c->builder, res_slice, slice_len, 1, "res_len");
		return res_slice;
	}

	case NODE_VAR_REF:
	case NODE_MEMBER_ACCESS:
	case NODE_INDEX: {
		// Operator-provided indexing (IDEAS 1.2): a struct with an impl
		// `self_index` method turns `base[i]` into that call. Arrays,
		// slices, and pointers never hit this -- their types aren't
		// TYPE_STRUCT, so the check is one registry probe for everything
		// else.
		if (n->type == NODE_INDEX) {
			Type *bt = index_base_struct_type(
				c, n->data.index.object);
			if (bt && impl_has_method(c, bt->name, "self_index")) {
				return codegen_index_overload(c, n);
			}
		}
		return value_of_lvalue(c, n);
	}

	case NODE_DEREF:
	case NODE_AMP:
		return value_of_lvalue(c, n);

	case NODE_CALL: {
		char func_name[256];
		LLVMValueRef fn = resolve_callee(c, n->data.call.callee, func_name,
										 sizeof(func_name));

		// Generic instantiation (IDEAS 2.2). A call naming a registered
		// generic fn monomorphizes it for the concrete argument types:
		// the mangled instance (name__T0_T1) is codegen'd on first use and
		// cached in the module -- subsequent calls hit the same symbol.
		if (!fn) {
			for (int gfi = 0; gfi < c->generic_fn_count; gfi++) {
				ASTNode *gfn = c->generic_fns[gfi];
				if (strcmp(gfn->data.func.name, func_name) != 0)
					continue;
				// Collect concrete arg types (scope-stamp bare refs).
				Type *concrete[8];
				int ci = 0;
				int ok = 1;
				for (ASTNode *a = n->data.call.args; a; a = a->next, ci++) {
					Type *at = a->data_type;
					if (!at && a->type == NODE_VAR_REF) {
						Scope *sv = scope_find(c, a->data.var_ref.name);
						if (sv && sv->node && sv->node->data_type)
							at = sv->node->data_type;
					}
					if (!at || ci >= 8) {
						ok = 0;
						break;
					}
					concrete[ci] = at;
				}
				if (!ok)
					break; // untyped args: fall through to error path
				char mangled[192];
				int mo = snprintf(mangled, sizeof(mangled), "%s",
								  func_name);
				for (int mi = 0; mi < ci; mi++) {
					const char *tn = "T";
					switch (concrete[mi]->kind) {
					case TYPE_I8: tn = "i8"; break;
					case TYPE_U8: tn = "u8"; break;
					case TYPE_I16: tn = "i16"; break;
					case TYPE_U16: tn = "u16"; break;
					case TYPE_I32: tn = "i32"; break;
					case TYPE_U32: tn = "u32"; break;
					case TYPE_I64: tn = "i64"; break;
					case TYPE_U64: tn = "u64"; break;
					case TYPE_F16: tn = "f16"; break;
					case TYPE_BF16: tn = "bf16"; break;
					case TYPE_F32: tn = "f32"; break;
					case TYPE_F64: tn = "f64"; break;
					default: tn = concrete[mi]->name ? concrete[mi]->name : "?";
					}
					mo += snprintf(mangled + mo,
								   (unsigned)(sizeof(mangled) - mo),
								   "__%s", tn);
				}
				if (LLVMGetNamedFunction(c->module, mangled)) {
					fn = LLVMGetNamedFunction(c->module, mangled);
					break;
				}
				// Bind params -> concrete types, then emit the instance.
				c->generic_param_count = 0;
				ASTNode *gp = gfn->data.func.args;
				int gpi = 0;
				for (ASTNode *a2 = n->data.call.args; a2 && gp;
					 a2 = a2->next, gp = gp->next) {
					Type *pt = gp->data_type;
					while (pt && (pt->kind == TYPE_ARRAY ||
								  pt->kind == TYPE_SLICE ||
								  pt->kind == TYPE_PTR))
						pt = pt->inner;
					if (pt && pt->kind == TYPE_STRUCT && pt->name &&
						strlen(pt->name) == 1 && pt->name[0] == 'T') {
						c->generic_param_names[gpi] =
							pt->name; // always "T" today
						c->generic_param_types[gpi] = concrete[gpi];
						gpi++;
					}
				}
				c->generic_param_count = gpi;
				c->generic_instantiating = 1;
				// Emit directly under the mangled name so distinct
				// specializations never collide on the plain symbol.
				// Save/restore the caller's emission state: the instance's
				// body leaves the builder parked in its own function.
				LLVMBasicBlockRef saved_bb = LLVMGetInsertBlock(c->builder);
				LLVMValueRef saved_fn2 = c->current_func;
				LLVMTypeRef saved_rt2 = c->current_ret_type;
				Scope *saved_scope = c->scope_stack;
				// Drop any caller debug location first: the instance's body
				// sets locations scoped to ITS subprogram, and a call we
				// emit afterwards must not inherit that scope.
				LLVMSetCurrentDebugLocation2(c->builder,
											 (LLVMMetadataRef)NULL);
				char *saved_name = gfn->data.func.name;
				gfn->data.func.name = arena_strdup(c->arena, mangled);
				codegen_func_decl(c, gfn, NULL);
				gfn->data.func.name = saved_name;
				c->generic_param_count = 0;
				c->generic_instantiating = 0;
				LLVMPositionBuilderAtEnd(c->builder, saved_bb);
				c->current_func = saved_fn2;
				c->current_ret_type = saved_rt2;
				c->scope_stack = saved_scope;
				// n->line can be 0 (postfix nodes aren't stamped); fall
				// back to the enclosing function's entry line so the
				// re-anchor always lands in THIS subprogram's scope.
				kawa_di_set_location(c,
									 n->line > 0 ? n->line : 1);
				fn = LLVMGetNamedFunction(c->module, mangled);
				if (!fn) {
					kerr(KAWA_E_SEMANTIC, n,
						 "generic instantiation of `%s` failed", mangled);
					exit(1);
				}
				// Rewrite this call site to the specialization permanently.
				n->data.call.callee->type = NODE_VAR_REF;
				n->data.call.callee->data.var_ref.name =
					arena_strdup(c->arena, mangled);
				// Give the call its concrete return type so `let x =
				// generic(...)` infers correctly (the parser skipped
				// generics when building its signature table).
				if (gfn->data.func.ret_type && !n->data_type) {
					Type *rt = gfn->data.func.ret_type;
					if (rt->kind == TYPE_STRUCT && rt->name &&
						strlen(rt->name) == 1) {
						// Bare T: clone with the first param's concrete
						// type so later uses don't need the map.
						Type *conc =
							arena_alloc(c->arena, sizeof(Type));
						*conc = *concrete[0];
						n->data_type = conc;
					} else {
						n->data_type = rt;
					}
				}
				break;
			}
		}

		// Overload resolution (IDEAS 1.x): when the bare name belongs to an
		// overload set, pick the declaration whose param types match the
		// argument types exactly and retarget the call to its mangled symbol.
		if (!fn && is_overloaded_name(c, func_name)) {
			const char *mangled =
				resolve_overload(c, n, func_name, n->data.call.args);
			if (mangled)
				fn = LLVMGetNamedFunction(c->module, mangled);
		}

		// Built-in reductions (sum/max/min/dot) and saturating narrow-int
		// ops (qadd/qsub/qmul) intercept before ordinary function
		// resolution. Reductions need array/slice args of a numeric element
		// type; sat ops take two narrow ints. Anything else falls through
		// to normal resolution and the usual "undefined function" path.
		if (!fn && (strcmp(func_name, "qadd") == 0 ||
					strcmp(func_name, "qsub") == 0 ||
					strcmp(func_name, "qmul") == 0)) {
			int arity = 0;
			for (ASTNode *a = n->data.call.args; a; a = a->next)
				arity++;
			if (arity == 2) {
				ASTNode *la = n->data.call.args;
				ASTNode *ra = la->next;
				LLVMValueRef lv = codegen_expr(c, la);
				LLVMValueRef rv = codegen_expr(c, ra);
				Type *lt = la->data_type, *rt = ra->data_type;
				if (!lt && la->type == NODE_VAR_REF) {
					Scope *sv = scope_find(c, la->data.var_ref.name);
					if (sv && sv->node && sv->node->data_type)
						lt = sv->node->data_type;
				}
				if (!rt && ra->type == NODE_VAR_REF) {
					Scope *sv = scope_find(c, ra->data.var_ref.name);
					if (sv && sv->node && sv->node->data_type)
						rt = sv->node->data_type;
				}
				// Both operands must be the SAME narrow type.
				Type *et = (lt && rt && lt->kind == rt->kind) ? lt : NULL;
				LLVMValueRef sat =
					et ? kawa_build_sat_op(c, func_name, et, lv, rv) : NULL;
				if (sat)
					return sat;
			}
			// fall through: user-defined qadd or mismatched types.
		}
		if (!fn && strcmp(func_name, "make_chan") == 0) {
			// make_chan(cap): allocate the ring buffer and fill the
			// {buf, cap, head, count, mask} record. The element type comes
			// from the assignment context (chan<T>). Ring byte size is
			// derived from a null-GEP -- no target data needed at compile
			// time. The allocation is rounded up to a power of two so
			// send/recv index with `& mask` instead of `% cap`; cap stays
			// the user-visible blocking threshold.
			Type *chan_t = n->data_type;
			if (!chan_t || chan_t->kind != TYPE_CHAN) {
				kdiag_help("annotate the binding: `let ch: chan<i32> = "
						   "make_chan(...)`");
				kerr(KAWA_E_TYPE, n, "make_chan requires a chan<T> context");
				exit(1);
			}
			LLVMContextRef ctx = c->context;
			LLVMTypeRef ct = get_llvm_type(c, chan_t);
			LLVMTypeRef elem_t = get_llvm_type(c, chan_t->inner);
			LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx);
			LLVMTypeRef i8ptr =
				LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);

			long long cap = 64; // default capacity
			if (n->data.call.args &&
				n->data.call.args->type == NODE_LITERAL)
				cap = n->data.call.args->data.literal.i64_val;
			if (cap < 1)
				cap = 1;

			// alloc = next power of two >= cap.
			long long alloc = 1;
			while (alloc < cap)
				alloc <<= 1;
			unsigned long long mask = (unsigned long long)alloc - 1;

			LLVMTypeRef malloc_t =
				LLVMFunctionType(i8ptr, (LLVMTypeRef[]){i64_t}, 1, 0);
			LLVMValueRef malloc_f =
				LLVMGetNamedFunction(c->module, "malloc");
			if (!malloc_f)
				malloc_f = LLVMAddFunction(c->module, "malloc", malloc_t);

			// sizeof(elem) = ptrtoint(gep null[1]).
			LLVMValueRef null_elem_ptr =
				LLVMConstNull(LLVMPointerType(elem_t, 0));
			LLVMValueRef one_gep = LLVMBuildGEP2(
				c->builder, elem_t, null_elem_ptr,
				(LLVMValueRef[]){LLVMConstInt(i64_t, 1, 0)}, 1,
				"elem_sz_probe");
			LLVMValueRef elem_sz =
				LLVMBuildPtrToInt(c->builder, one_gep, i64_t, "elem_sz");
			LLVMValueRef ring_bytes = LLVMBuildMul(
				c->builder, elem_sz,
				LLVMConstInt(i64_t, (unsigned long long)alloc, 0),
				"ring_bytes");

			LLVMValueRef buf_v = LLVMBuildCall2(c->builder, malloc_t,
												malloc_f, &ring_bytes, 1,
												"chan_ring");

			LLVMValueRef alloca =
				create_entry_block_alloca(c, ct, "chan_new");
			LLVMTypeRef buf_fld_t = LLVMPointerType(elem_t, 0);
			LLVMBuildStore(
				c->builder,
				LLVMBuildPointerCast(c->builder, buf_v, buf_fld_t,
									 "ring_cast"),
				LLVMBuildStructGEP2(c->builder, ct, alloca, 0, ""));
			LLVMBuildStore(c->builder,
						   LLVMConstInt(i64_t,
										(unsigned long long)cap, 0),
						   LLVMBuildStructGEP2(c->builder, ct, alloca, 1,
											   ""));
			LLVMBuildStore(c->builder, LLVMConstInt(i64_t, 0, 0),
						   LLVMBuildStructGEP2(c->builder, ct, alloca, 2,
											   ""));
			LLVMBuildStore(c->builder, LLVMConstInt(i64_t, 0, 0),
						   LLVMBuildStructGEP2(c->builder, ct, alloca, 3,
											   ""));
			LLVMBuildStore(c->builder, LLVMConstInt(i64_t, mask, 0),
						   LLVMBuildStructGEP2(c->builder, ct, alloca, 4,
											   ""));
			return LLVMBuildLoad2(c->builder, ct, alloca, "chan_val");
		}
		if (!fn && (strcmp(func_name, "sum") == 0 ||
					strcmp(func_name, "max") == 0 ||
					strcmp(func_name, "min") == 0 ||
					strcmp(func_name, "dot") == 0)) {
			int arity = 0;
			for (ASTNode *a = n->data.call.args; a; a = a->next)
				arity++;
			int want = (strcmp(func_name, "dot") == 0) ? 2 : 1;
			if (arity == want) {
				Type *coll = NULL;
				LLVMValueRef views[2] = {NULL, NULL};
				int ok = 1, ai = 0;
				for (ASTNode *a = n->data.call.args; a; a = a->next, ai++) {
					Type *at = a->data_type;
					if (!at && a->type == NODE_VAR_REF) {
						Scope *sv =
							scope_find(c, a->data.var_ref.name);
						if (sv && sv->node && sv->node->data_type)
							at = sv->node->data_type;
					}
					if (!at || (at->kind != TYPE_ARRAY &&
								at->kind != TYPE_SLICE)) {
						ok = 0;
						break;
					}
					if (!coll)
						coll = at->inner ? at->inner : at;
					else if (at->inner &&
							 at->inner->kind != coll->kind)
						ok = 0; // dot over mismatched element types
					if (ok) {
						// Build the {ptr,len} view. Arrays: GEP elem 0 of
						// the ORIGINAL storage -- never spill a 1MB array
						// per call. Slices: pass the pair value through.
						LLVMTypeRef elem =
							get_llvm_type(c, at->inner);
						Type slice_t = {0};
						slice_t.kind = TYPE_SLICE;
						slice_t.inner = at->inner;
						LLVMTypeRef slice_ll =
							get_llvm_type(c, &slice_t);
						if (at->kind == TYPE_ARRAY) {
							Scope *sv0 = NULL;
							if (a->type == NODE_VAR_REF)
								sv0 = scope_find(
									c, a->data.var_ref.name);
							LLVMTypeRef addr_t = NULL;
							LLVMValueRef addr =
								sv0 ? sv0->val
									: get_address(c, a, &addr_t);
							LLVMValueRef data = LLVMBuildGEP2(
								c->builder, elem, addr,
								(LLVMValueRef[]){LLVMConstInt(
									LLVMInt64TypeInContext(
										c->context), 0, 0)},
								1, "view_data");
							LLVMValueRef view = LLVMGetUndef(slice_ll);
							view = LLVMBuildInsertValue(
								c->builder, view, data, 0,
								"view_ins_data");
							view = LLVMBuildInsertValue(
								c->builder, view,
								LLVMConstInt(
									LLVMInt64TypeInContext(c->context),
									(unsigned long long)at->array_len,
									0),
								1, "view_ins_len");
							views[ai] = view;
						} else {
							// Slice arg: already a {ptr,i64} value.
							views[ai] = value_of_lvalue(c, a);
						}
					}
				}
				if (ok && coll && coll->kind >= TYPE_I8 &&
					coll->kind <= TYPE_F64) {
					// Emit the specialized reduction, then extract data
					// pointer + length from each view and call it.
					//
					// The emitter switches the builder into its own new
					// function; remember where the CALLER was building so
					// the call lands in the caller's block, not in the
					// intrinsic's exit.
					LLVMBasicBlockRef caller_bb =
						LLVMGetInsertBlock(c->builder);
					LLVMValueRef bfn = kawa_emit_reduction_fn(
						c, func_name, coll, want);
					LLVMPositionBuilderAtEnd(c->builder, caller_bb);
					LLVMTypeRef bfn_t = LLVMGlobalGetValueType(bfn);
					// extractvalue on a {ptr,i64} first-class value
					// yields the fields directly -- no memory ops.
					LLVMValueRef call_args[4] = {NULL, NULL, NULL, NULL};
					for (int vi = 0; vi < want; vi++) {
						call_args[vi * 2] = LLVMBuildExtractValue(
							c->builder, views[vi], 0, "d");
						call_args[vi * 2 + 1] = LLVMBuildExtractValue(
							c->builder, views[vi], 1, "len");
					}
					return LLVMBuildCall2(c->builder, bfn_t, bfn,
										  call_args, want * 2, "red");
				}
			}
			// Wrong shape/type for a builtin: fall through to normal
			// resolution so user-defined sum(x) etc still work.
		}
		if (!fn) {
			if ((fn = declare_kawa_runtime_fn(c, func_name)) != NULL) {
				// Internal Kawa runtime I/O helper
			} else if (strcmp(func_name, "printf") == 0) {
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
					   n->data.call.callee->data.member_access.object
						   ->type == NODE_MEMBER_ACCESS) {
				// Two-level namespace call (`std.io.puts`) with the leaf
				// name in func_name: rebuild the dotted path from the AST
				// when it is rooted at std and try the exact table.
				ASTNode *mid =
					n->data.call.callee->data.member_access.object;
				if (mid && mid->data.member_access.object &&
					mid->data.member_access.object->type == NODE_VAR_REF &&
					strcmp(mid->data.member_access.object->data.var_ref.name,
						   "std") == 0 && mid->data.member_access.member) {
					char qual[256];
					snprintf(qual, sizeof(qual), "std.%s.%s",
							 mid->data.member_access.member,
							 n->data.call.callee->data.member_access.member);
					fn = declare_std_fn(c, qual);
					if (!fn) {
						kerr(KAWA_E_UNDEF, n,
							 "unknown std function `%s`", qual);
						exit(1);
					}
				}
			} else if (strcmp(func_name, "exit") == 0) {
				fn = declare_std_fn(c, "std.process.exit");
			} else if (strcmp(func_name, "puts") == 0 ||
					   strcmp(func_name, "eputs") == 0) {
				// `puts` bare resolves through libc; `std.io.puts` is the
				// same libc symbol reached via the std namespace.
				fn = declare_libc_fn(c, func_name);
				if (!fn)
					fn = declare_std_fn(
						c, func_name[0] == 'e' ? "std.io.eputs"
											   : "std.io.puts");
			} else if ((fn = declare_libc_fn(c, func_name)) != NULL) {
				// Bare calls of known libc functions (`strlen(s)` etc.)
				// work like stdc.strlen -- exact prototype, zero overhead.
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
					// std.io.* / std.process.* reach here with the bare
					// leaf name in func_name; give them their exact
					// prototypes before falling back to variadic.
					fn = declare_std_fn(
						c, strcmp(func_name, "exit") == 0
							   ? "std.process.exit"
							   : func_name[0] == 'e' ? "std.io.eputs"
													 : "std.io.puts");
				}
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
				// An overloaded name with no type-matching candidate gets
				// its own diagnosis: the function exists, these arguments
				// don't fit any of its signatures.
				if (is_overloaded_name(c, func_name)) {
					int n_overloads = 0;
					for (int oi = 0; oi < c->overload_fn_count; oi++)
						if (strcmp(c->overload_fns[oi].bare, func_name) == 0)
							n_overloads++;
					knerr(KAWA_E_TYPE, n, func_name,
						  "no overload of `%s` matches %d argument(s) "
						  "(%d declared)", f_path,
						  resolve_overload_arg_count(n),
						  n_overloads);
					exit(1);
				}
				// Did-you-mean over every declared fn in the program.
				const char *cands[129];
				int nc = 0;
				for (ASTNode *g = c->program_root ? c->program_root->next
												  : NULL;
					 g && nc < 128; g = g->next)
					if (g->type == NODE_FUNC_DECL)
						cands[nc++] = g->data.func.name;
				cands[nc] = NULL;
				const char *alt = kdiag_closest(func_name, cands);
				if (alt)
					kdiag_help("a function with a similar name exists: "
							   "`%s`",
							   alt);
				knerr(KAWA_E_UNDEF, n, func_name,
					  "cannot find function `%s` in this scope", f_path);
				exit(1);
			}
		}

		int arg_count = 0;
		for (ASTNode *a = n->data.call.args; a; a = a->next)
			arg_count++;

		LLVMTypeRef func_type = LLVMGlobalGetValueType(fn);
		int param_count = LLVMCountParamTypes(func_type);
		if (arg_count < param_count ||
			(!LLVMIsFunctionVarArg(func_type) && arg_count != param_count)) {
			kerr(KAWA_E_ARITY, n, "function `%s` expects %s%d arguments, got %d",
				 func_name, LLVMIsFunctionVarArg(func_type) ? "at least " : "",
				 param_count, arg_count);
			exit(1);
		}

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
					kerr(KAWA_E_ARGS, n,
						 "no unique parameter `%s` in call", label);
					exit(1);
				}
				used[matched] = 1;
				reord[matched] = a;
			}
			if (!ok) {
				kerr(KAWA_E_ARGS, n,
					 "if any argument is named, all must be named");
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
			// Array -> slice decay: passing an `[N]T` lvalue where a
			// `[]T` param is expected builds a {&arr[0], N} view in one
			// constant pair -- no copy, no runtime work.
			if (i < param_count && arg_node->type == NODE_VAR_REF) {
				Scope *sv = scope_find(c, arg_node->data.var_ref.name);
				if (sv && sv->node && sv->node->data_type &&
					sv->node->data_type->kind == TYPE_ARRAY) {
					Type *at = sv->node->data_type;
					Type slice_t = {0};
					slice_t.kind = TYPE_SLICE;
					slice_t.inner = at->inner;
					LLVMTypeRef want = get_llvm_type(c, &slice_t);
					if (param_types[i] == want) {
						LLVMTypeRef ignored;
						LLVMValueRef arr_addr =
							get_address(c, arg_node, &ignored);
						LLVMTypeRef elem =
							get_llvm_type(c, at->inner);
						LLVMValueRef data = LLVMBuildGEP2(
							c->builder, elem, arr_addr,
							(LLVMValueRef[]){LLVMConstInt(
								LLVMInt64TypeInContext(c->context), 0,
								0)},
							1, "slice_data");
						LLVMValueRef len = LLVMConstInt(
							LLVMInt64TypeInContext(c->context),
							(unsigned long long)at->array_len, 0);
						LLVMValueRef view = LLVMGetUndef(want);
						view = LLVMBuildInsertValue(
							c->builder, view, data, 0, "slice_ins_data");
						view = LLVMBuildInsertValue(
							c->builder, view, len, 1, "slice_ins_len");
						llvm_args[i] = view;
						arg_node = arg_node->next;
						continue;
					}
				}
			}
			// Array params decay like C: passing an `[N]T` lvalue to a
			// pointer param passes the address of element 0 -- no copy,
			// no load of the whole array.
			if (i < param_count &&
				LLVMGetTypeKind(param_types[i]) == LLVMPointerTypeKind &&
				arg_node->type == NODE_VAR_REF) {
				Scope *sv = scope_find(c, arg_node->data.var_ref.name);
				if (sv && sv->node && sv->node->data_type &&
					sv->node->data_type->kind == TYPE_ARRAY) {
					LLVMTypeRef ignored;
					llvm_args[i] = get_address(c, arg_node, &ignored);
					arg_node = arg_node->next;
					continue;
				}
			}
			LLVMValueRef val = codegen_expr(c, arg_node);

			if (i < param_count) {
				LLVMTypeRef expected = param_types[i];

				// Pointer parameter + lvalue argument: pass the ADDRESS of
				// the original storage -- `q.birthday()` with
				// `fn void birthday(P* p)` must mutate q itself. Loading
				// the struct value first and spilling to a temporary would
				// silently redirect every store into the temp.
				if (LLVMGetTypeKind(expected) == LLVMPointerTypeKind &&
					arg_node->type != NODE_LITERAL) {
					int is_lvalue = arg_node->type == NODE_VAR_REF ||
									arg_node->type == NODE_MEMBER_ACCESS ||
									arg_node->type == NODE_INDEX ||
									arg_node->type == NODE_DEREF;
					// Slice-typed lvalues decay to their data pointer
					// instead of passing the pair's address.
					Type *lvt = arg_node->data_type;
					if (!lvt && arg_node->type == NODE_VAR_REF) {
						Scope *slv =
							scope_find(c,
									   arg_node->data.var_ref.name);
						lvt = (slv && slv->node) ? slv->node->data_type
												 : NULL;
					}
					if (lvt && lvt->kind == TYPE_SLICE)
						is_lvalue = 0;
					if (is_lvalue) {
						LLVMTypeRef pointee = NULL;
						LLVMValueRef addr =
							get_address(c, arg_node, &pointee);
						if (addr && pointee &&
							LLVMGetTypeKind(pointee) ==
								LLVMStructTypeKind) {
							llvm_args[i] = addr;
							arg_node = arg_node->next;
							continue;
						}
					}
				}
				// Decay at C boundaries: a str/[]u8 view passed where a
				// char* is expected contributes just its data pointer --
				// the storage keeps the trailing NUL so libc stays happy.
				if (!arg_node->data_type &&
					arg_node->type == NODE_VAR_REF) {
					Scope *svp =
						scope_find(c, arg_node->data.var_ref.name);
					arg_node->data_type =
						(svp && svp->node) ? svp->node->data_type : NULL;
				}
				if (LLVMGetTypeKind(expected) == LLVMPointerTypeKind &&
					LLVMGetTypeKind(LLVMTypeOf(val)) ==
						LLVMStructTypeKind &&
					arg_node->data_type &&
					arg_node->data_type->kind == TYPE_SLICE &&
					arg_node->data_type->inner &&
					(arg_node->data_type->inner->kind == TYPE_U8 ||
					 arg_node->data_type->inner->kind == TYPE_CHAR)) {
					LLVMTypeRef want_ptr =
						LLVMPointerType(LLVMInt8TypeInContext(
							c->context), 0);
					val = LLVMBuildExtractValue(c->builder, val, 0,
												"str_decay");
					val = coerce_value(c, val, NULL, want_ptr, NULL);
					llvm_args[i] = val;
					arg_node = arg_node->next;
					continue;
				}
				// Auto-spill: function expects a pointer but caller is
				// passing an rvalue struct (e.g. f(&tmp) shape). Spill to
				// a local alloca and pass its address.
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
				// and float promotion to f64. Signed narrow values must
				// SIGN-extend (%d of an i16 -600 printed 64936 when zext).
				LLVMTypeRef vt = LLVMTypeOf(val);
				LLVMTypeKind k = LLVMGetTypeKind(vt);
				// A str view decays to its char* in a vararg slot
				// (printf("%s\n", s)); the storage keeps the NUL.
				if (!arg_node->data_type &&
					arg_node->type == NODE_VAR_REF) {
					Scope *sva =
						scope_find(c, arg_node->data.var_ref.name);
					arg_node->data_type =
						(sva && sva->node) ? sva->node->data_type : NULL;
				}
				if (k == LLVMStructTypeKind && arg_node->data_type &&
					arg_node->data_type->kind == TYPE_SLICE &&
					arg_node->data_type->inner &&
					(arg_node->data_type->inner->kind == TYPE_U8 ||
					 arg_node->data_type->inner->kind == TYPE_CHAR)) {
					val = LLVMBuildExtractValue(c->builder, val, 0,
												"str_decay_va");
					llvm_args[i] = val;
					arg_node = arg_node->next;
					continue;
				}
				if (k == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(vt) < 32) {
					int va_s =
						arg_node->data_type
							? type_is_signed(c, arg_node->data_type)
							: 0;
					if (!va_s && arg_node->type == NODE_VAR_REF) {
						Scope *sv = scope_find(
							c, arg_node->data.var_ref.name);
						if (sv && sv->node && sv->node->data_type)
							va_s = type_is_signed(c, sv->node->data_type);
					}
					val = va_s ? LLVMBuildSExt(c->builder, val,
											   LLVMInt32TypeInContext(
												   c->context),
											   "vararg_prom")
							   : LLVMBuildZExt(c->builder, val,
											   LLVMInt32TypeInContext(
												   c->context),
											   "vararg_prom");
				}
				else if (is_fp_kind(k) && k != LLVMDoubleTypeKind)
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
			kerr(KAWA_E_SEMANTIC, n, "literal missing type"); // internal
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
			kerr(KAWA_E_SEMANTIC, n, "struct literal missing type"); // internal
			exit(1);
		}

		LLVMValueRef alloca = create_entry_block_alloca(c, s_type, "lit");
		LLVMTypeRef base_ty_unused = NULL;

		// Field defaults + `..base` spread (IDEAS 1.3). Seed order:
		// spread base first (whole-record copy), then defaults for fields
		// the base didn't provide... no -- the base IS a full record, so
		// with a spread the seed is just the base; without one, declared
		// field defaults fill in. Explicit items always overwrite.
		StructInitItem *spread = NULL;
		for (StructInitItem *it = n->data.struct_lit.items; it; it = it->next)
			if (it->spread_from)
				spread = it;
		if (spread) {
			LLVMTypeRef i8_t = LLVMInt8TypeInContext(c->context);
			LLVMTypeRef i8ptr =
				LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
			ASTNode *base_node = spread->spread_from;
			Type *base_ty = base_node->data_type;
			// libc memcpy: same machine code as the intrinsic once the
			// optimizer recognizes the libfunc (lowered inline).
			LLVMTypeRef memcpy_t = LLVMFunctionType(
				i8ptr, (LLVMTypeRef[]){i8ptr, i8ptr,
									   LLVMInt64TypeInContext(c->context)},
				3, 0);
			LLVMValueRef mc =
				LLVMGetNamedFunction(c->module, "memcpy");
			if (!mc || LLVMGetTypeKind(LLVMGlobalGetValueType(mc)) !=
						   LLVMFunctionTypeKind)
				mc = LLVMAddFunction(c->module, "memcpy", memcpy_t);
			LLVMValueRef base_ptr;
			if (base_ty && (base_ty->kind == TYPE_STRUCT)) {
				base_ptr = get_address(c, base_node, &base_ty_unused);
			} else {
				// Rvalue base: spill to a temp alloca and copy from there.
				LLVMValueRef tmp =
					create_entry_block_alloca(c, s_type, "spread_base");
				LLVMBuildStore(c->builder,
							   codegen_expr(c, base_node), tmp);
				base_ptr = tmp;
			}
			LLVMBuildCall2(
				c->builder, memcpy_t, mc,
				(LLVMValueRef[]){
					LLVMBuildBitCast(c->builder, alloca, i8ptr, ""),
					LLVMBuildBitCast(c->builder, base_ptr, i8ptr, ""),
					LLVMSizeOf(s_type)},
				3, "");
		} else {
			// Declared defaults: store each before explicit items run.
			StructDef *sd = find_struct_def_pub(c, s_type);
			if (sd) {
				for (int di = 0; di < sd->field_count; di++) {
					if (!sd->fields[di].default_expr)
						continue;
					ASTNode *dflt = sd->fields[di].default_expr;
					LLVMValueRef dv = codegen_expr(c, dflt);
					dv = coerce_value(c, dv, dflt->data_type,
									  sd->fields[di].type, NULL);
					LLVMValueRef gep = LLVMBuildStructGEP2(
						c->builder, s_type, alloca, (unsigned)di, "dflt");
					LLVMBuildStore(c->builder, dv, gep);
				}
			}
		}

		StructInitItem *item = n->data.struct_lit.items;
		for (int idx = 0; item; idx++, item = item->next) {
			if (item->spread_from)
				continue; // already applied as the seed
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

	case NODE_TERNARY: {
		// cond ? a : b -- both arms evaluate in their own block; a phi
		// merges the values. Same structure as short-circuit && / ||.
		LLVMValueRef func = c->current_func;
		LLVMValueRef cond =
			cond_to_bool(c, codegen_expr(c, n->data.ternary.cond));
		LLVMBasicBlockRef then_bb =
			kawa_append_block(func, "tern_then");
		LLVMBasicBlockRef else_bb =
			kawa_append_block(func, "tern_else");
		LLVMBasicBlockRef merge_bb =
			kawa_append_block(func, "tern_merge");
		LLVMBuildCondBr(c->builder, cond, then_bb, else_bb);

		LLVMPositionBuilderAtEnd(c->builder, then_bb);
		LLVMValueRef tv = codegen_expr(c, n->data.ternary.then_expr);
		LLVMBasicBlockRef then_end = LLVMGetInsertBlock(c->builder);
		LLVMBuildBr(c->builder, merge_bb);

		LLVMPositionBuilderAtEnd(c->builder, else_bb);
		LLVMValueRef ev = codegen_expr(c, n->data.ternary.else_expr);
		LLVMBasicBlockRef else_end = LLVMGetInsertBlock(c->builder);
		LLVMBuildBr(c->builder, merge_bb);

		LLVMPositionBuilderAtEnd(c->builder, merge_bb);
		// Matching struct-typed arms (str views, slices) are fine: LLVM
		// phis carry first-class aggregates. Mismatched shapes stay an
		// error.
		int arms_ok = LLVMTypeOf(tv) == LLVMTypeOf(ev);
		if (!arms_ok) {
			kerr(KAWA_E_TYPE, n, "ternary arms must be scalars of matching type");
			exit(1);
		}
		LLVMValueRef phi =
			LLVMBuildPhi(c->builder, LLVMTypeOf(tv), "tern_val");
		LLVMAddIncoming(phi, (LLVMValueRef[]){tv, ev},
						(LLVMBasicBlockRef[]){then_end, else_end}, 2);
		return phi;
	}

	case NODE_BINARY_OP:
		// Short-circuit logical ops need custom control flow -- the RHS
		// must not be evaluated unless the LHS demands it.
		if (n->data.bin_op.op == TOK_ANDAND || n->data.bin_op.op == TOK_OROR)
			return codegen_short_circuit(c, n);
		rehome_wide_literal(c, n);
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
			kawa_append_block(c->current_func, "set_grow");
		LLVMBasicBlockRef append_bb =
			kawa_append_block(c->current_func, "set_append");

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


	case NODE_SEND: {
		// ch <- v: if the ring is full, yield and re-poll; else store at
		// slot (head+count) & mask and bump count. Ready path: 1 load,
		// 1 store, 1 add, 1 mask.
		LLVMContextRef ctx = c->context;
		Type *chan_t = n->data.send.chan->data_type;
		if (!chan_t && n->data.send.chan->type == NODE_VAR_REF) {
			Scope *sc = scope_find(c,
								   n->data.send.chan->data.var_ref.name);
			chan_t = (sc && sc->node) ? sc->node->data_type : NULL;
			n->data.send.chan->data_type = chan_t;
		}
		if (!chan_t || chan_t->kind != TYPE_CHAN) {
			kerr(KAWA_E_TYPE, n, "send requires a chan<T>");
			exit(1);
		}
		LLVMTypeRef ct = get_llvm_type(c, chan_t);
		LLVMTypeRef elem_t = get_llvm_type(c, chan_t->inner);
		LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx);

		// Purity probe: when the sent expression is side-effect-free we
		// evaluate it AFTER the capacity check passes, so its SSA value
		// never spans a suspension point -- coro-split then keeps it in a
		// register instead of spilling through the coroutine frame on every
		// iteration of a send-heavy loop. Impure values (calls, chan ops,
		// assignments) keep the evaluate-first order: blocking must not
		// re-run their side effects.
		int val_pure = expr_is_pure(c, n->data.send.value);

		LLVMValueRef chan_addr =
			get_address(c, n->data.send.chan, NULL);

		LLVMValueRef cnt_p =
			LLVMBuildStructGEP2(c->builder, ct, chan_addr, 3, "cnt_p");
		LLVMValueRef cap_p =
			LLVMBuildStructGEP2(c->builder, ct, chan_addr, 1, "cap_p");
		LLVMValueRef head_p =
			LLVMBuildStructGEP2(c->builder, ct, chan_addr, 2, "head_p");
		LLVMValueRef mask_p =
			LLVMBuildStructGEP2(c->builder, ct, chan_addr, 4, "mask_p");
		LLVMValueRef buf_p =
			LLVMBuildStructGEP2(c->builder, ct, chan_addr, 0, "buf_p");

		LLVMValueRef val = NULL;
		if (!val_pure) {
			val = codegen_expr(c, n->data.send.value);
			val = coerce_value(c, val, n->data.send.value->data_type,
							   elem_t, chan_t->inner);
		}

		// check_bb is a true loop header: every coroutine resume re-enters
		// it, so capacity is re-read fresh after each suspension -- no
		// volatiles needed, the CFG carries the ordering.
		LLVMBasicBlockRef check_bb =
			kawa_append_block(c->current_func, "send_check");
		LLVMBasicBlockRef retry_bb =
			kawa_append_block(c->current_func, "send_full");
		LLVMBasicBlockRef do_send_bb =
			kawa_append_block(c->current_func, "send_put");
		LLVMBasicBlockRef done_bb =
			kawa_append_block(c->current_func, "send_done");

		LLVMBuildBr(c->builder, check_bb);
		LLVMPositionBuilderAtEnd(c->builder, check_bb);
		LLVMValueRef cur_cnt =
			LLVMBuildLoad2(c->builder, i64_t, cnt_p, "cnt");
		LLVMValueRef cur_cap =
			LLVMBuildLoad2(c->builder, i64_t, cap_p, "cap");
		LLVMValueRef full =
			LLVMBuildICmp(c->builder, LLVMIntUGE, cur_cnt, cur_cap,
						  "is_full");
		set_branch_weights(c, LLVMBuildCondBr(c->builder, full, retry_bb,
											  do_send_bb),
						   1, 99);

		// Full: cooperative block -- suspend; resumption loops back to
		// re-poll rather than falling into the store.
		LLVMPositionBuilderAtEnd(c->builder, retry_bb);
		if (c->in_coroutine && c->current_coro_hdl) {
			chan_yield(c);
			LLVMBuildBr(c->builder, check_bb);
		} else {
			// Outside any coroutine there is nothing to yield to: a full
			// ring here is a program bug. Trap like a bounds failure.
			LLVMValueRef msg = LLVMBuildGlobalStringPtr(
				c->builder, "send on full channel", "trap_msg");
			LLVMValueRef file_v = LLVMBuildGlobalStringPtr(
				c->builder,
				c->source_filename ? c->source_filename : "?",
				"trap_file");
			LLVMValueRef targs[3] = {
				msg, file_v,
				LLVMConstInt(LLVMInt32TypeInContext(ctx),
							 n->line > 0 ? n->line : 0, 1)};
			LLVMValueRef tf = get_or_declare_trap_fn(c);
			LLVMBuildCall2(c->builder,
						   LLVMGlobalGetValueType(tf), tf, targs, 3,
						   "");
			LLVMBuildUnreachable(c->builder);
		}

		LLVMPositionBuilderAtEnd(c->builder, do_send_bb);
		if (val_pure) {
			// Post-check evaluation: no suspension separates this value
			// from its store, so it lives entirely in registers.
			val = codegen_expr(c, n->data.send.value);
			val = coerce_value(c, val, n->data.send.value->data_type,
							   elem_t, chan_t->inner);
		}
		cur_cnt = LLVMBuildLoad2(c->builder, i64_t, cnt_p, "cnt2");
		LLVMValueRef head =
			LLVMBuildLoad2(c->builder, i64_t, head_p, "head");
		LLVMValueRef mask_v =
			LLVMBuildLoad2(c->builder, i64_t, mask_p, "mask");
		LLVMValueRef buf =
			LLVMBuildLoad2(c->builder,
						   LLVMPointerType(elem_t, 0), buf_p, "ring");
		// head is monotonic; the write slot is (head+count) & mask. Since
		// count never exceeds cap <= alloc, unread slots are never
		// overwritten -- FIFO holds with no wrap store on the send path.
		LLVMValueRef end = LLVMBuildAdd(c->builder, head, cur_cnt,
										"end");
		LLVMValueRef slot_idx = LLVMBuildAnd(c->builder, end, mask_v,
											 "slot");
		LLVMValueRef slot =
			LLVMBuildGEP2(c->builder, elem_t, buf, &slot_idx, 1,
						  "slot_p");
		LLVMBuildStore(c->builder, val, slot);
		LLVMBuildStore(c->builder,
					   LLVMBuildNUWAdd(c->builder, cur_cnt,
									   LLVMConstInt(i64_t, 1, 0),
									   "cnt_inc"),
					   cnt_p);
		LLVMBuildBr(c->builder, done_bb);
		LLVMPositionBuilderAtEnd(c->builder, done_bb);
		return val;
	}

	case NODE_RECV: {
		// <-ch: if empty, yield and re-poll; else load head, advance it
		// modulo cap, decrement count.
		LLVMContextRef ctx = c->context;
		Type *chan_t = n->data.recv.chan->data_type;
		if (!chan_t && n->data.recv.chan->type == NODE_VAR_REF) {
			Scope *sc = scope_find(c,
								   n->data.recv.chan->data.var_ref.name);
			chan_t = (sc && sc->node) ? sc->node->data_type : NULL;
			n->data.recv.chan->data_type = chan_t;
			if (chan_t && chan_t->inner)
				n->data_type = chan_t->inner;
		}
		if (!chan_t || chan_t->kind != TYPE_CHAN) {
			kerr(KAWA_E_TYPE, n, "receive requires a chan<T>");
			exit(1);
		}
		LLVMTypeRef ct = get_llvm_type(c, chan_t);
		LLVMTypeRef elem_t = get_llvm_type(c, chan_t->inner);
		LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx);

		LLVMValueRef chan_addr =
			get_address(c, n->data.recv.chan, NULL);

		LLVMValueRef cnt_p =
			LLVMBuildStructGEP2(c->builder, ct, chan_addr, 3, "rcnt_p");
		LLVMValueRef head_p =
			LLVMBuildStructGEP2(c->builder, ct, chan_addr, 2, "rhead_p");
		LLVMValueRef mask_p =
			LLVMBuildStructGEP2(c->builder, ct, chan_addr, 4, "rmask_p");
		LLVMValueRef buf_p =
			LLVMBuildStructGEP2(c->builder, ct, chan_addr, 0, "rbuf_p");

		// Same loop-header discipline as send: resume re-enters the check
		// with freshly loaded count/head, never a stale snapshot.
		LLVMBasicBlockRef check_bb =
			kawa_append_block(c->current_func, "recv_check");
		LLVMBasicBlockRef retry_bb =
			kawa_append_block(c->current_func, "recv_empty");
		LLVMBasicBlockRef do_recv_bb =
			kawa_append_block(c->current_func, "recv_get");
		LLVMBasicBlockRef done_bb =
			kawa_append_block(c->current_func, "recv_done");

		LLVMBuildBr(c->builder, check_bb);
		LLVMPositionBuilderAtEnd(c->builder, check_bb);
		LLVMValueRef cur_cnt =
			LLVMBuildLoad2(c->builder, i64_t, cnt_p, "cnt");
		set_branch_weights(
			c,
			LLVMBuildCondBr(c->builder,
							LLVMBuildICmp(c->builder, LLVMIntEQ, cur_cnt,
										  LLVMConstInt(i64_t, 0, 0),
										  "is_empty"),
							retry_bb, do_recv_bb),
			1, 99);

		LLVMPositionBuilderAtEnd(c->builder, retry_bb);
		if (c->in_coroutine && c->current_coro_hdl) {
			chan_yield(c);
			LLVMBuildBr(c->builder, check_bb);
		} else {
			// Same as send: nothing to schedule us -- trap.
			LLVMValueRef msg2 = LLVMBuildGlobalStringPtr(
				c->builder, "receive on empty channel", "trap_msg");
			LLVMValueRef file_v2 = LLVMBuildGlobalStringPtr(
				c->builder,
				c->source_filename ? c->source_filename : "?",
				"trap_file");
			LLVMValueRef targs2[3] = {
				msg2, file_v2,
				LLVMConstInt(LLVMInt32TypeInContext(ctx),
							 n->line > 0 ? n->line : 0, 1)};
			LLVMValueRef tf2 = get_or_declare_trap_fn(c);
			LLVMBuildCall2(c->builder,
						   LLVMGlobalGetValueType(tf2), tf2, targs2, 3,
						   "");
			LLVMBuildUnreachable(c->builder);
		}

		LLVMPositionBuilderAtEnd(c->builder, do_recv_bb);
		LLVMValueRef head =
			LLVMBuildLoad2(c->builder, i64_t, head_p, "head");
		LLVMValueRef mask_v =
			LLVMBuildLoad2(c->builder, i64_t, mask_p, "mask");
		LLVMValueRef buf =
			LLVMBuildLoad2(c->builder, LLVMPointerType(elem_t, 0), buf_p,
						   "ring");
		// Read slot is head & mask over the monotonic counter; the stored
		// head just advances -- no division anywhere on the data path.
		LLVMValueRef slot_idx = LLVMBuildAnd(c->builder, head, mask_v,
											 "slot");
		LLVMValueRef slot =
			LLVMBuildGEP2(c->builder, elem_t, buf, &slot_idx, 1,
						  "slot_p");
		LLVMValueRef out = LLVMBuildLoad2(c->builder, elem_t, slot,
										  "recv_val");
		LLVMValueRef new_head = LLVMBuildNUWAdd(
			c->builder, head, LLVMConstInt(i64_t, 1, 0), "head1");
		LLVMBuildStore(c->builder, new_head, head_p);
		LLVMValueRef cnt_now =
			LLVMBuildLoad2(c->builder, i64_t, cnt_p, "cnt3");
		LLVMBuildStore(
			c->builder,
			LLVMBuildNSWSub(c->builder, cnt_now,
							LLVMConstInt(i64_t, 1, 0), "cnt_dec"),
			cnt_p);
		LLVMBuildBr(c->builder, done_bb);
		LLVMPositionBuilderAtEnd(c->builder, done_bb);
		return out;
	}

	case NODE_SIP: {
		LLVMValueRef hdl = codegen_expr(c, n->data.sip.handle);
		LLVMValueRef is_done = LLVMBuildCall2(c->builder, c->coro_done_type,
											  c->coro_done, &hdl, 1, "is_done");
		LLVMBasicBlockRef resume_bb =
			kawa_append_block(c->current_func, "sip_resume");
		LLVMBasicBlockRef cont_bb =
			kawa_append_block(c->current_func, "sip_cont");
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
		// Bare variable refs carry no parser-side type; resolve from their
		// declaration so sign-aware casts (sitofp vs uitofp) pick right.
		if (!n->data.cast.val->data_type &&
			n->data.cast.val->type == NODE_VAR_REF) {
			Scope *sv =
				scope_find(c, n->data.cast.val->data.var_ref.name);
			if (sv && sv->node && sv->node->data_type)
				n->data.cast.val->data_type = sv->node->data_type;
		}
		LLVMTypeRef dest_type = get_llvm_type(c, n->data_type);
		return coerce_value(c, val, n->data.cast.val->data_type, dest_type,
							n->data_type);
	}

	case NODE_BREW:
		return codegen_brew(c, n);

	case NODE_ASM: {
		// Inline asm as a side-effecting barrier expression. No operand
		// plumbing in v1: the template runs verbatim (AT&T dialect).
		LLVMTypeRef asm_t = LLVMFunctionType(
			LLVMVoidTypeInContext(c->context), NULL, 0, 0);
		const char *cons =
			n->data.asm_block.constraints ? n->data.asm_block.constraints : "";
		LLVMValueRef asm_val = LLVMGetInlineAsm(
			asm_t, n->data.asm_block.asm_template,
			strlen(n->data.asm_block.asm_template), cons, strlen(cons),
			/*hasSideEffects*/ 1, /*isAlignStack*/ 0,
			LLVMInlineAsmDialectATT, /*CanThrow*/ 0);
		LLVMBuildCall2(c->builder, asm_t, asm_val, NULL, 0, "");
		return LLVMConstNull(LLVMInt32TypeInContext(c->context));
	}

	default:
		break;
	}

	kdiag_error_at(KAWA_E_SEMANTIC, c->source_filename ? c->source_filename
													   : "<kawa>",
				   NULL, n && n->line > 0 ? n->line : 0,
				   "unknown AST node type %d in codegen_expr", // internal
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
		kawa_append_block(func, is_and ? "and_rhs" : "or_rhs");
	LLVMBasicBlockRef merge_bb = kawa_append_block(func, "bool_merge");
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

// Exact prototypes for common libc functions used via stdc.*. A mismatched// declaration is UB -- e.g. declaring strcpy variadic miscompiles on arm64
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

LLVMValueRef declare_kawa_runtime_fn(KawaCompiler *c, const char *name) {
	if (strncmp(name, "__kawa_", 7) != 0)
		return NULL;
	if (LLVMGetNamedFunction(c->module, name))
		return LLVMGetNamedFunction(c->module, name);

	LLVMContextRef ctx = c->context;
	LLVMTypeRef void_t = LLVMVoidTypeInContext(ctx);
	LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
	LLVMTypeRef i8 = LLVMInt8TypeInContext(ctx);
	LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx);
	LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx);
	LLVMTypeRef f64 = LLVMDoubleTypeInContext(ctx);

	if (strcmp(name, "__kawa_flush") == 0 || strcmp(name, "__kawa_print_nl") == 0) {
		return LLVMAddFunction(c->module, name, LLVMFunctionType(void_t, NULL, 0, 0));
	}
	if (strcmp(name, "__kawa_print_str") == 0) {
		LLVMTypeRef args[] = {i8ptr, i64};
		return LLVMAddFunction(c->module, name, LLVMFunctionType(void_t, args, 2, 0));
	}
	if (strcmp(name, "__kawa_print_cstr") == 0) {
		LLVMTypeRef args[] = {i8ptr};
		return LLVMAddFunction(c->module, name, LLVMFunctionType(void_t, args, 1, 0));
	}
	if (strcmp(name, "__kawa_print_char") == 0) {
		LLVMTypeRef args[] = {i8};
		return LLVMAddFunction(c->module, name, LLVMFunctionType(void_t, args, 1, 0));
	}
	if (strcmp(name, "__kawa_print_bool") == 0) {
		LLVMTypeRef args[] = {i32};
		return LLVMAddFunction(c->module, name, LLVMFunctionType(void_t, args, 1, 0));
	}
	if (strcmp(name, "__kawa_print_i64") == 0 || strcmp(name, "__kawa_print_u64") == 0) {
		LLVMTypeRef args[] = {i64};
		return LLVMAddFunction(c->module, name, LLVMFunctionType(void_t, args, 1, 0));
	}
	if (strcmp(name, "__kawa_print_f64") == 0) {
		LLVMTypeRef args[] = {f64};
		return LLVMAddFunction(c->module, name, LLVMFunctionType(void_t, args, 1, 0));
	}
	if (strcmp(name, "__kawa_print_f64_prec") == 0) {
		LLVMTypeRef args[] = {f64, i32};
		return LLVMAddFunction(c->module, name, LLVMFunctionType(void_t, args, 2, 0));
	}
	if (strcmp(name, "__kawa_print_hex") == 0) {
		LLVMTypeRef args[] = {i64, i32};
		return LLVMAddFunction(c->module, name, LLVMFunctionType(void_t, args, 2, 0));
	}
	if (strcmp(name, "__kawa_print_pad_i64") == 0) {
		LLVMTypeRef args[] = {i64, i32, i8};
		return LLVMAddFunction(c->module, name, LLVMFunctionType(void_t, args, 3, 0));
	}
	return NULL;
}

// Declare a std.* function under its bare leaf name with its exact
// prototype. Returns the existing declaration if one is already present.
static LLVMValueRef declare_std_fn(KawaCompiler *c, const char *qualified) {
	const char *leaf = strrchr(qualified, '.');
	leaf = leaf ? leaf + 1 : qualified;
	if (LLVMGetNamedFunction(c->module, leaf))
		return LLVMGetNamedFunction(c->module, leaf);
	LLVMTypeRef t = kawa_std_fn_type(c, qualified);
	if (!t)
		return NULL;
	LLVMContextRef ctx = c->context;
	LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx);
	LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx);
	LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
	// std.process.arg_count/arg_at read __kawa_argc/__kawa_argv globals
	// captured by @main's prologue. Synthesized here on first use.
	if (strcmp(qualified, "std.process.arg_count") == 0 ||
		strcmp(qualified, "std.process.arg_at") == 0) {
		LLVMTypeRef i8t = LLVMInt8TypeInContext(ctx);
		const char *gname =
			strcmp(qualified, "std.process.arg_at") == 0 ? "__kawa_argv"
														 : "__kawa_argc";
		LLVMValueRef gv = LLVMGetNamedGlobal(c->module, gname);
		if (!gv) {
			gv = LLVMAddGlobal(
				c->module,
				strcmp(gname, "__kawa_argv") == 0
					? LLVMPointerType(LLVMPointerType(i8t, 0), 0)
					: i32,
				gname);
			LLVMSetInitializer(gv, LLVMConstNull(LLVMGlobalGetValueType(gv)));
			LLVMSetLinkage(gv, LLVMPrivateLinkage);
		}
		char fname[64];
		snprintf(fname, sizeof(fname), "__kawa_%s",
				 strcmp(qualified, "std.process.arg_at") == 0 ? "arg_at"
															  : "arg_count");
		LLVMValueRef f = LLVMGetNamedFunction(c->module, fname);
		if (f)
			return f;
		if (strcmp(qualified, "std.process.arg_at") == 0) {
			f = LLVMAddFunction(c->module, fname, t);
			LLVMBasicBlockRef bb = kawa_append_block(f, "entry");
			LLVMBuilderRef ab = LLVMCreateBuilderInContext(ctx);
			LLVMPositionBuilderAtEnd(ab, bb);
			LLVMValueRef argc_g = LLVMGetNamedGlobal(c->module, "__kawa_argc");
			if (!argc_g) {
				argc_g = LLVMAddGlobal(c->module, i32, "__kawa_argc");
				LLVMSetInitializer(argc_g, LLVMConstNull(i32));
				LLVMSetLinkage(argc_g, LLVMPrivateLinkage);
			}
			LLVMBuilderRef saved_builder = c->builder;
			LLVMValueRef saved_fn = c->current_func;
			c->builder = ab; c->current_func = f;
			LLVMValueRef count = LLVMBuildLoad2(ab, i32, argc_g, "argc");
			emit_check_or_trap(c, NULL, LLVMBuildICmp(ab, LLVMIntULT, LLVMGetParam(f, 0), count, "arg_ok"), "argument index out of bounds");
			c->builder = saved_builder; c->current_func = saved_fn;
			LLVMTypeRef argv_t =
				LLVMPointerType(LLVMPointerType(i8t, 0), 0);
			LLVMValueRef slot = LLVMBuildGEP2(
				ab, LLVMPointerType(i8t, 0),
				LLVMBuildLoad2(ab, argv_t, gv, "argv"),
				(LLVMValueRef[]){LLVMGetParam(f, 0)}, 1, "slot");
			LLVMValueRef s = LLVMBuildLoad2(ab, LLVMPointerType(i8t, 0),
											slot, "arg");
			LLVMValueRef lenfn = LLVMGetNamedFunction(c->module, "strlen");
			if (!lenfn) lenfn = LLVMAddFunction(c->module, "strlen", LLVMFunctionType(i64, &ptr, 1, 0));
			LLVMValueRef len = LLVMBuildCall2(ab, LLVMGlobalGetValueType(lenfn), lenfn, &s, 1, "arg_len");
			LLVMValueRef view = LLVMBuildInsertValue(ab, LLVMGetUndef(LLVMGetReturnType(t)), s, 0, "arg_data");
			view = LLVMBuildInsertValue(ab, view, len, 1, "arg_view");
			LLVMBuildRet(ab, view);
			LLVMDisposeBuilder(ab);
		} else {
			f = LLVMAddFunction(c->module, fname,
								LLVMFunctionType(i32, NULL, 0, 0));
			LLVMBasicBlockRef bb =
				LLVMAppendBasicBlockInContext(ctx, f, "entry");
			LLVMBuilderRef ab = LLVMCreateBuilderInContext(ctx);
			LLVMPositionBuilderAtEnd(ab, bb);
			LLVMValueRef n = LLVMBuildLoad2(ab, i32, gv, "argc");
			LLVMBuildRet(ab, n);
			LLVMDisposeBuilder(ab);
		}
		return f;
	}

	if (strcmp(qualified, "std.io.puts") == 0)
		return declare_puts_std(c);

	// eputs lowers onto write(2): declare it once and synthesize the full
	// call inside a tiny module-local wrapper so callers see a plain
	// i32(str)-shaped function. (stderr is unbuffered by C standard, so
	// raw write cannot reorder against printf output.)
	if (strcmp(qualified, "std.io.eputs") == 0) {
		int fd = 2;
		char wname[32];
		snprintf(wname, sizeof(wname), "__kawa_write_fd%d", fd);
		LLVMValueRef wfn = LLVMGetNamedFunction(c->module, wname);
		if (!wfn) {
			wfn = LLVMAddFunction(
				c->module, wname,
				LLVMFunctionType(LLVMInt64TypeInContext(c->context),
								 (LLVMTypeRef[]){ptr}, 1, 0));
			LLVMAppendBasicBlockInContext(c->context, wfn, "entry");
			LLVMBuilderRef wb =
				LLVMCreateBuilderInContext(c->context);
			LLVMPositionBuilderAtEnd(
				wb, LLVMGetEntryBasicBlock(wfn));
			LLVMValueRef sfn = LLVMGetNamedFunction(c->module, "strlen");
			if (!sfn)
				sfn = LLVMAddFunction(
					c->module, "strlen",
					LLVMFunctionType(LLVMInt64TypeInContext(c->context),
									 (LLVMTypeRef[]){ptr}, 1, 0));
			LLVMValueRef wlen = LLVMBuildCall2(
				wb,
				LLVMFunctionType(LLVMInt64TypeInContext(c->context),
								 (LLVMTypeRef[]){ptr}, 1, 0),
				sfn, (LLVMValueRef[]){LLVMGetParam(wfn, 0)}, 1, "len");
			LLVMBuildCall2(
				wb,
				LLVMFunctionType(LLVMInt64TypeInContext(c->context),
								 (LLVMTypeRef[]){i32, ptr, i64},
								 3, 0),
				LLVMGetNamedFunction(c->module, "write")
					? LLVMGetNamedFunction(c->module, "write")
					: LLVMAddFunction(
						  c->module, "write",
						  LLVMFunctionType(LLVMInt64TypeInContext(ctx),
										   (LLVMTypeRef[]){i32, ptr, i64},
										   3, 0)),
				(LLVMValueRef[]){LLVMConstInt(i32, (unsigned long long)fd,
											  0),
								 LLVMGetParam(wfn, 0), wlen},
				3, "");
			LLVMBuildRet(wb, wlen);
			LLVMDisposeBuilder(wb);
		}
		return LLVMGetNamedFunction(c->module, wname);
	}
	LLVMValueRef fn = LLVMAddFunction(c->module, leaf, t);
	// exit never returns: mark it noreturn so LLVM knows no fallthrough
	// code after the call can be reached.
	if (strcmp(qualified, "std.process.exit") == 0)
		LLVMAddTargetDependentFunctionAttr(fn, "noreturn", "");
	return fn;
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
	if (!t || t->kind != TYPE_SLICE || !t->inner)
		return 0;
	return t->inner->kind == TYPE_U8;
}

static LLVMValueRef build_str_view_field(KawaCompiler *c,
										 LLVMTypeRef view_t,
										 LLVMValueRef view, int field) {
	return LLVMBuildExtractValue(c->builder, view, (unsigned)field,
								 field == 0 ? "str_d" : "str_l");
}

#include <stdio.h>
// If `node` is a string literal, return its bytes (excluding the NUL the
// stored global carries). Lets == against a literal lower to compares
// against immediates instead of loading the literal from memory.
static const unsigned char *literal_str_bytes(const ASTNode *node,
											  size_t *out_len) {
	*out_len = 0;
	if (!node || node->type != NODE_STRING_LIT ||
		!node->data.str_lit.s_val)
		return NULL;
	*out_len = node->data.str_lit.len;
	return (const unsigned char *)node->data.str_lit.s_val;
}

// Equality of two {ptr,len} views for == and !=: lengths gate the compare,
// then one memcmp over the full length. Passing l_len (not a min() select)
// as the size lets the optimizer fold a constant length into an inline
// load-compare -- the three-way memcmp path cannot, which is why `w ==
// "quick"` in hot loops must come through here, not build_str_memcmp.
static LLVMValueRef build_str_eq(KawaCompiler *c, LLVMTypeRef view_t,
								 const ASTNode *ln, LLVMValueRef l,
								 const ASTNode *rn, LLVMValueRef r) {
	LLVMContextRef ctx = c->context;
	LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx);
	LLVMTypeRef i1_t = LLVMInt1TypeInContext(ctx);
	LLVMValueRef ld = build_str_view_field(c, view_t, l, 0);
	LLVMValueRef ll = build_str_view_field(c, view_t, l, 1);
	LLVMValueRef rd = build_str_view_field(c, view_t, r, 0);
	LLVMValueRef rl = build_str_view_field(c, view_t, r, 1);

	// Literal-vs-literal with both byte strings visible: fold entirely.
	size_t lbytes_len = 0, rbytes_len = 0;
	const unsigned char *lbytes = literal_str_bytes(ln, &lbytes_len);
	const unsigned char *rbytes = literal_str_bytes(rn, &rbytes_len);
	if (lbytes && rbytes) {
		int same = (lbytes_len == rbytes_len) &&
				   memcmp(lbytes, rbytes, lbytes_len) == 0;
		return LLVMConstInt(i1_t, same ? 1 : 0, 0);
	}

	// len_l == len_r && bytes equal over len. When both lengths are
	// compile-time constants that differ, fold straight to false without
	// touching memory.
	if (LLVMIsAConstantInt(ll) && LLVMIsAConstantInt(rl) &&
		LLVMConstIntGetSExtValue(ll) != LLVMConstIntGetSExtValue(rl))
		return LLVMConstInt(i1_t, 0, 0);

	long clen = -1;
	LLVMValueRef clen_v = LLVMIsAConstantInt(ll)	? ll
						  : LLVMIsAConstantInt(rl)	? rl
													: NULL;
	int const_len = clen_v && (clen = LLVMConstIntGetSExtValue(clen_v)) >= 1 &&
					clen <= 16;

	// Known literal bytes + constant length: the length test and the
	// chunk compares all go into the CURRENT block as one and-chain --
	// no extra branches for the optimizer to duplicate or reschedule.
	size_t klen = 0;
	const unsigned char *kb =
		lbytes ? lbytes : rbytes ? rbytes : NULL;
	if (const_len && kb && LLVMIsAConstantInt(ll) && LLVMIsAConstantInt(rl)) {
		LLVMValueRef var_ptr = lbytes ? rd : ld;
		LLVMValueRef lens_eq = NULL;
		if (!LLVMIsAConstantInt(ll) || !LLVMIsAConstantInt(rl)) {
			// One length is runtime: gate on it matching the literal.
			LLVMValueRef run_len = LLVMIsAConstantInt(ll) ? rl : ll;
			lens_eq = LLVMBuildICmp(c->builder, LLVMIntEQ, run_len,
									LLVMConstInt(i64_t, clen, 0),
									"str_lens");
		}
		LLVMValueRef eq_all = NULL;
		size_t off = 0;
		while (off < (size_t)clen) {
			size_t rem = (size_t)clen - off;
			size_t w = rem >= 8 ? 8 : rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
			unsigned bits = (unsigned)(w * 8);
			uint64_t imm = 0;
			memcpy(&imm, kb + off, w);
			LLVMValueRef lp = LLVMBuildGEP2(
				c->builder, LLVMInt8TypeInContext(ctx), var_ptr,
				(LLVMValueRef[]){LLVMConstInt(i64_t, off, 0)}, 1, "");
			LLVMValueRef lv = LLVMBuildLoad2(
				c->builder,
				LLVMIntTypeInContext(ctx, bits), lp,
				w == 1 ? "str_byte" : "str_chunk");
			LLVMSetAlignment(lv, 1);
			LLVMValueRef z = LLVMBuildICmp(
				c->builder, LLVMIntEQ, lv,
				LLVMConstInt(LLVMIntTypeInContext(ctx, bits), imm, 0),
				w == 1 ? "str_byte_eq" : "str_chunk_eq");
			eq_all = eq_all ? LLVMBuildAnd(c->builder, eq_all, z,
										   "str_and")
							: z;
			off += w;
		}
		if (!lens_eq)
			return eq_all;
		return LLVMBuildAnd(c->builder, lens_eq, eq_all, "str_eq");
	}

	LLVMBasicBlockRef entry_bb = LLVMGetInsertBlock(c->builder);
	LLVMBasicBlockRef len_ok =
		kawa_append_block(c->current_func, "str_len_ok");
	LLVMBasicBlockRef str_eq_done =
		kawa_append_block(c->current_func, "str_eq_done");
	LLVMValueRef lens_eq =
		LLVMBuildICmp(c->builder, LLVMIntEQ, ll, rl, "str_lens");
	LLVMValueRef nonempty = LLVMBuildICmp(c->builder, LLVMIntNE, ll, LLVMConstNull(i64_t), "str_nonempty");
	LLVMValueRef compare = LLVMBuildAnd(c->builder, lens_eq, nonempty, "str_compare");
	LLVMBuildCondBr(c->builder, compare, len_ok, str_eq_done);

	LLVMPositionBuilderAtEnd(c->builder, len_ok);
	// Remaining cases (both lengths runtime, or constant length > 16):
	// one length-gated memcmp call.
	LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
	LLVMTypeRef i32_t = LLVMInt32TypeInContext(ctx);
	LLVMTypeRef fn_t =
		LLVMFunctionType(i32_t, (LLVMTypeRef[]){i8ptr, i8ptr, i64_t}, 3, 0);
	LLVMValueRef bcmp = LLVMGetNamedFunction(c->module, "memcmp");
	if (!bcmp)
		bcmp = LLVMAddFunction(c->module, "memcmp", fn_t);
	LLVMValueRef args[3] = {ld, rd, ll};
	LLVMValueRef call =
		LLVMBuildCall2(c->builder, fn_t, bcmp, args, 3, "str_bcmp");
	LLVMValueRef eq_bytes = LLVMBuildICmp(
		c->builder, LLVMIntEQ, call, LLVMConstInt(i32_t, 0, 0),
		"str_bytes_eq");
	LLVMBuildBr(c->builder, str_eq_done);

	LLVMPositionBuilderAtEnd(c->builder, str_eq_done);
	LLVMValueRef incoming[2] = {
		eq_bytes, lens_eq};
	LLVMBasicBlockRef preds[2] = {len_ok, entry_bb};
	LLVMValueRef out = LLVMBuildPhi(c->builder, i1_t, "str_eq");
	LLVMAddIncoming(out, incoming, preds, 2);
	return out;
}
// length, ties broken by total length -- the same ordering strcmp gives
// without scanning for terminators. Constant operands fold at -O2.
static LLVMValueRef build_str_memcmp(KawaCompiler *c, LLVMTypeRef view_t,
									 LLVMValueRef l, LLVMValueRef r) {
	LLVMContextRef ctx = c->context;
	LLVMTypeRef i8ptr =
		LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
	LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx);
	LLVMTypeRef i32_t = LLVMInt32TypeInContext(ctx);
	LLVMTypeRef fn_t =
		LLVMFunctionType(i32_t, (LLVMTypeRef[]){i8ptr, i8ptr, i64_t}, 3, 0);
	LLVMValueRef fn = LLVMGetNamedFunction(c->module, "memcmp");
	if (!fn)
		fn = LLVMAddFunction(c->module, "memcmp", fn_t);
	LLVMValueRef ld =
		build_str_view_field(c, view_t, l, 0);
	LLVMValueRef ll =
		build_str_view_field(c, view_t, l, 1);
	LLVMValueRef rd =
		build_str_view_field(c, view_t, r, 0);
	LLVMValueRef rl =
		build_str_view_field(c, view_t, r, 1);
	LLVMValueRef ll_lt =
		LLVMBuildICmp(c->builder, LLVMIntULT, ll, rl, "ll_lt");
	LLVMValueRef min_len =
		LLVMBuildSelect(c->builder, ll_lt, ll, rl, "minlen");
	LLVMValueRef args[3] = {ld, rd, min_len};
	LLVMBasicBlockRef start = LLVMGetInsertBlock(c->builder);
	LLVMBasicBlockRef bytes = kawa_append_block(c->current_func, "str_compare_bytes");
	LLVMBasicBlockRef done = kawa_append_block(c->current_func, "str_order");
	LLVMValueRef nonempty = LLVMBuildICmp(c->builder, LLVMIntNE, min_len, LLVMConstNull(i64_t), "str_nonempty");
	LLVMBuildCondBr(c->builder, nonempty, bytes, done);
	LLVMPositionBuilderAtEnd(c->builder, bytes);
	LLVMValueRef byte_cmp = LLVMBuildCall2(c->builder, fn_t, fn, args, 3, "str_bcmp");
	LLVMBuildBr(c->builder, done);
	LLVMPositionBuilderAtEnd(c->builder, done);
	LLVMValueRef cmp = LLVMBuildPhi(c->builder, i32_t, "str_prefix_cmp");
	LLVMAddIncoming(cmp, (LLVMValueRef[]){LLVMConstNull(i32_t), byte_cmp},
		(LLVMBasicBlockRef[]){start, bytes}, 2);
	// memcmp result sign: convert to the strcmp-style three-way value.
	LLVMValueRef neg = LLVMBuildICmp(c->builder, LLVMIntSLT, cmp,
									 LLVMConstInt(i32_t, 0, 0), "lt0");
	LLVMValueRef pos = LLVMBuildICmp(c->builder, LLVMIntSGT, cmp,
									 LLVMConstInt(i32_t, 0, 0), "gt0");
	// Equal prefixes: the shorter string sorts first.
	LLVMValueRef tie_neg =
		LLVMBuildICmp(c->builder, LLVMIntSLT, ll, rl, "tie_neg");
	LLVMValueRef tie_val =
		LLVMBuildSelect(c->builder, tie_neg, LLVMConstInt(i32_t, -1, 1),
						LLVMConstInt(i32_t, 1, 0), "tie_val");
	LLVMValueRef tie_is_eq =
		LLVMBuildICmp(c->builder, LLVMIntEQ, ll, rl, "tie_is_eq");
	LLVMValueRef tie_result =
		LLVMBuildSelect(c->builder, tie_is_eq, LLVMConstInt(i32_t, 0, 0),
						tie_val, "tie_result");
	LLVMValueRef by_content =
		LLVMBuildSelect(c->builder, neg, LLVMConstInt(i32_t, -1, 1),
						LLVMBuildSelect(c->builder, pos,
										LLVMConstInt(i32_t, 1, 0),
										tie_result, "sel_pos"),
						"sel_neg");
	(void)i64_t;
	return by_content;
}

// Re-home a 32-bit-or-untyped literal operand to a wider partner's width and
// sign before emission (`~u64_var` desugars to x ^ (-1) with the ones literal
// carrying no parse-time type; emitting it first would truncate to i32).
// Must run BEFORE the operands are codegen'd.
static void rehome_wide_literal(KawaCompiler *c, ASTNode *n) {
	ASTNode *sides[2] = {n->data.bin_op.left, n->data.bin_op.right};
	for (int si = 0; si < 2; si++) {
		ASTNode *me = sides[si], *other = sides[si ^ 1];
		if (me->type != NODE_LITERAL || other->type == NODE_LITERAL)
			continue;
		// Stamp bare var refs so `other` always carries its declared type.
		if (!other->data_type && other->type == NODE_VAR_REF) {
			Scope *osv = scope_find(c, other->data.var_ref.name);
			if (osv && osv->node && osv->node->data_type)
				other->data_type = osv->node->data_type;
		}
		if (!other->data_type)
			continue;
		if (me->data_type && me->data_type->kind != TYPE_I32 &&
			me->data_type->kind != TYPE_U32)
			continue;
		LLVMTypeRef ot = get_llvm_type(c, other->data_type);
		if (LLVMGetTypeKind(ot) != LLVMIntegerTypeKind)
			continue;
		unsigned ow = LLVMGetIntTypeWidth(ot);
		if (ow <= 32)
			continue;
		int os_ = type_is_signed(c, other->data_type);
		TypeKind wk;
		if (ow == 64)
			wk = os_ ? TYPE_I64 : TYPE_U64;
		else if (ow == 16)
			wk = os_ ? TYPE_I16 : TYPE_U16;
		else
			wk = TYPE_U8;
		Type *wt = arena_alloc(c->arena, sizeof(Type));
		wt->kind = wk;
		me->data_type = wt;
		// Untyped binop (~x on a bare var ref): adopt the partner's type so
		// `let v = ~x;` infers the right width instead of defaulting i32.
		if (!n->data_type)
			n->data_type = wt;
	}
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
			LLVMValueRef cmp;
			if (LLVMGetTypeKind(LLVMTypeOf(l)) == LLVMStructTypeKind) {
				// Fat {ptr,len} views. Equality takes the specialized
				// length-gated path so constant-length compares fold into
				// inline load-compares; orderings keep the three-way
				// content compare.
				Type slice_t = {0};
				slice_t.kind = TYPE_SLICE;
				slice_t.inner = NULL;
				slice_t.inner = arena_alloc(c->arena, sizeof(Type));
				slice_t.inner->kind = TYPE_U8;
				LLVMTypeRef view_t = get_llvm_type(c, &slice_t);
				if (op == TOK_ISEQ)
					return build_str_eq(c, view_t, n->data.bin_op.left, l,
								n->data.bin_op.right, r);
				if (op == TOK_NOTEQ) {
					// != is the negation of the same equality; emit the
					// length-gated compare once and flip it.
					LLVMValueRef eq =
						build_str_eq(c, view_t,
									 n->data.bin_op.left, l,
									 n->data.bin_op.right, r);
					return LLVMBuildNot(c->builder, eq, "str_ne");
				}
				cmp = build_str_memcmp(c, view_t, l, r);
			} else {
				cmp = build_strcmp_call(c, l, r);
			}
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

	// String concat (IDEAS 3): a + b allocates len_a+len_b+1 bytes, copies
	// both views, and stores the NUL. One malloc, two memcpys -- no
	// per-byte loops. (The + never runs when both operands are literals;
	// the folder would have to constant-fold it first.)
	{
		Type *lt2 = n->data.bin_op.left->data_type;
		Type *rt2 = n->data.bin_op.right->data_type;
		if (!lt2 && n->data.bin_op.left->type == NODE_VAR_REF) {
			Scope *s2 = scope_find(
				c, n->data.bin_op.left->data.var_ref.name);
			lt2 = (s2 && s2->node) ? s2->node->data_type : NULL;
		}
		if (!rt2 && n->data.bin_op.right->type == NODE_VAR_REF) {
			Scope *s2 = scope_find(
				c, n->data.bin_op.right->data.var_ref.name);
			rt2 = (s2 && s2->node) ? s2->node->data_type : NULL;
		}
		if ((op == TOK_PLUS || op == TOK_PLUS_EQ) && lt2 && rt2 &&
			lt2->kind == TYPE_SLICE && rt2->kind == TYPE_SLICE &&
			lt2->inner &&
			(lt2->inner->kind == TYPE_U8 || lt2->inner->kind == TYPE_CHAR) &&
			rt2->inner &&
			(rt2->inner->kind == TYPE_U8 || rt2->inner->kind == TYPE_CHAR)) {
			LLVMContextRef ctx = c->context;
			LLVMTypeRef i64_t = LLVMInt64TypeInContext(ctx);
			LLVMTypeRef i8ptr =
				LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
			Type u8t2 = {0};
			u8t2.kind = TYPE_U8;
			Type slice_t2 = {0};
			slice_t2.kind = TYPE_SLICE;
			slice_t2.inner = &u8t2;
			LLVMTypeRef view_t = get_llvm_type(c, &slice_t2);

			LLVMValueRef ld =
				LLVMBuildExtractValue(c->builder, l, 0, "a_d");
			LLVMValueRef ll =
				LLVMBuildExtractValue(c->builder, l, 1, "a_l");
			LLVMValueRef rd =
				LLVMBuildExtractValue(c->builder, r, 0, "b_d");
			LLVMValueRef rl =
				LLVMBuildExtractValue(c->builder, r, 1, "b_l");
			LLVMValueRef total = LLVMBuildAdd(c->builder, ll, rl, "str_n");
			// total+1 for the NUL.
			LLVMValueRef alloc_n = LLVMBuildAdd(
				c->builder, total, LLVMConstInt(i64_t, 1, 0), "str_cap");
			LLVMTypeRef malloc_t = LLVMFunctionType(
				i8ptr, (LLVMTypeRef[]){i64_t}, 1, 0);
			LLVMValueRef malloc_f = LLVMGetNamedFunction(c->module,
														 "malloc");
			if (!malloc_f)
				malloc_f = LLVMAddFunction(c->module, "malloc", malloc_t);
			LLVMValueRef buf = LLVMBuildCall2(c->builder, malloc_t,
											  malloc_f, &alloc_n, 1,
											  "str_buf");

			// libc memcpy: same machine code as the intrinsic once the
			// optimizer recognizes the libfunc (lowered inline).
			LLVMTypeRef memcpy_t = LLVMFunctionType(
				i8ptr, (LLVMTypeRef[]){i8ptr, i8ptr, i64_t}, 3, 0);
			LLVMValueRef memcpy_f =
				LLVMGetNamedFunction(c->module, "memcpy");
			if (!memcpy_f || LLVMGetTypeKind(LLVMGlobalGetValueType(
								 memcpy_f)) != LLVMFunctionTypeKind ||
				LLVMCountParamTypes(LLVMGlobalGetValueType(memcpy_f)) != 3)
				memcpy_f = LLVMAddFunction(c->module, "memcpy",
										   memcpy_t);

			LLVMValueRef mc1_args[3] = {buf, ld, ll};
			LLVMBuildCall2(c->builder, memcpy_t, memcpy_f, mc1_args, 3,
						   "");
			// Second copy destination: buf + ll (GEP on the raw pointer).
			LLVMValueRef tail_dst = LLVMBuildGEP2(
				c->builder, LLVMInt8TypeInContext(ctx), buf, &ll, 1,
				"str_tail");
			LLVMValueRef mc2_args[3] = {tail_dst, rd, rl};
			LLVMBuildCall2(c->builder, memcpy_t, memcpy_f, mc2_args, 3,
						   "");
			// NUL terminator at buf[total].
			LLVMValueRef nul_dst = LLVMBuildGEP2(
				c->builder, LLVMInt8TypeInContext(ctx), buf, &total, 1,
				"str_nul_slot");
			LLVMBuildStore(c->builder,
						   LLVMConstInt(LLVMInt8TypeInContext(ctx), 0, 0),
						   nul_dst);

			LLVMValueRef view = LLVMGetUndef(view_t);
			view = LLVMBuildInsertValue(c->builder, view, buf, 0,
										"cat_ins_data");
			view = LLVMBuildInsertValue(c->builder, view, total, 1,
										"cat_ins_len");
			return view;
		}
	}

	int l_is_fp = is_fp_kind(LLVMGetTypeKind(l_ty));
	int r_is_fp = is_fp_kind(LLVMGetTypeKind(r_ty));

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

	// Mixed FP precision: promote the narrower side up (f16/bf16/f32 all
	// extend toward the widest operand). f16+bf16 meets at f32 -- bf16's
	// 8-bit mantissa can represent every f16 value exactly.
	if (l_is_fp && r_is_fp &&
		LLVMGetTypeKind(l_ty) != LLVMGetTypeKind(LLVMTypeOf(r))) {
		unsigned l_w = LLVMGetTypeKind(l_ty) == LLVMHalfTypeKind   ? 16
					   : LLVMGetTypeKind(l_ty) == LLVMBFloatTypeKind ? 16
																	 : LLVMGetTypeKind(l_ty) == LLVMFloatTypeKind ? 32
																												  : 64;
		unsigned r_w = LLVMGetTypeKind(LLVMTypeOf(r)) == LLVMHalfTypeKind
						   ? 16
					   : LLVMGetTypeKind(LLVMTypeOf(r)) == LLVMBFloatTypeKind
						   ? 16
					   : LLVMGetTypeKind(LLVMTypeOf(r)) == LLVMFloatTypeKind
						   ? 32
						   : 64;
		LLVMTypeRef meet =
			l_w >= r_w ? (l_w == 16 ? LLVMFloatTypeInContext(c->context)
									: l_w == 32 ? LLVMFloatTypeInContext(c->context)
												: LLVMDoubleTypeInContext(c->context))
					   : (r_w == 16 ? LLVMFloatTypeInContext(c->context)
									: r_w == 32 ? LLVMFloatTypeInContext(c->context)
												: LLVMDoubleTypeInContext(c->context));
		if (LLVMTypeOf(l) != meet)
			l = LLVMBuildFPExt(c->builder, l, meet, "promote_l_fp");
		if (LLVMTypeOf(r) != meet)
			r = LLVMBuildFPExt(c->builder, r, meet, "promote_r_fp");
		l_ty = meet;
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

	// A bare integer literal defaults to i32 (signed); when it is paired
	// with an unsigned operand of at least its width, the operation follows
	// the unsigned side -- `u32 h * 16777619` must wrap like C's
	// `uint32_t * int`, not take the signed no-wrap fast path (nsw makes
	// the deliberate wrap poison and the optimizer mangles the result).
	if ((l_signed != r_signed) &&
		LLVMGetTypeKind(l_ty) == LLVMIntegerTypeKind &&
		LLVMGetTypeKind(r_ty) == LLVMIntegerTypeKind) {
		unsigned lw = LLVMGetIntTypeWidth(l_ty);
		unsigned rw = LLVMGetIntTypeWidth(r_ty);
		if (l_signed && !r_signed && rw >= lw)
			l_signed = 0; // literal vs unsigned var: go unsigned
		else if (!l_signed && r_signed && lw >= rw)
			r_signed = 0;
	}

	if (LLVMGetTypeKind(l_ty) == LLVMIntegerTypeKind &&
		LLVMGetTypeKind(r_ty) == LLVMIntegerTypeKind && l_ty != r_ty) {
		unsigned lw = LLVMGetIntTypeWidth(l_ty);
		unsigned rw = LLVMGetIntTypeWidth(r_ty);
		// A 1-bit value is a bool result (comparison/logical): its truth
		// value is 1, so it zero-extends no matter what either side's
		// signedness says -- sext would turn `true` into -1.
		if (rw == 1 || (lw > rw)) {
			r = (rw != 1 &&
				 type_is_signed(c, n->data.bin_op.right->data_type))
					? LLVMBuildSExt(c->builder, r, l_ty, "widen_r")
					: LLVMBuildZExt(c->builder, r, l_ty, "widen_r");
			r_ty = l_ty;
		} else {
			l = (lw != 1 &&
				 type_is_signed(c, n->data.bin_op.left->data_type))
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
		// Saturating forms (qadd/qsub/qmul) come in as function calls,
		// not operators -- but a `q`-prefixed call on narrow types lowers
		// to the sat intrinsics. Plain operators keep C wrap/UB rules.
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
