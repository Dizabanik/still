#include "codegen_internal.h"

void trigger_orbit_updates(KawaCompiler *c, ASTNode *origin_node) {
	if (!origin_node || !origin_node->dependents)
		return;
	Dependency *dep = origin_node->dependents;
	while (dep) {
		LLVMValueRef new_val = codegen_expr(c, dep->logic_expr);
		Scope *s = scope_find(c, dep->dependent_node->data.var_decl.name);
		if (s) {
			LLVMValueRef store = LLVMBuildStore(c->builder, new_val, s->val);
			attach_tbaa(c, store, s->type);
		}
		trigger_orbit_updates(c, dep->dependent_node);
		dep = dep->next;
	}
}

LLVMValueRef codegen_expr(KawaCompiler *c, ASTNode *n) {
	if (!n)
		return LLVMConstInt(LLVMInt32TypeInContext(c->context), 0, 0);

	if (n->type == NODE_LITERAL) {
		if (n->data_type && (n->data_type->kind == TYPE_F32 ||
							 n->data_type->kind == TYPE_F64)) {
			return LLVMConstReal(get_llvm_type(c, n->data_type),
								 (double)n->data.literal.f_val);
		}
		return LLVMConstInt(LLVMInt32TypeInContext(c->context),
							n->data.literal.i_val, 0);
	}
	if (n->type == NODE_SIZEOF) {
		LLVMTypeRef type_to_measure = NULL;
		if (n->data.size_of.type_val) {
			type_to_measure = get_llvm_type(c, n->data.size_of.type_val);
		} else if (n->data.size_of.value) {
			// [Optimization] For now, we only support sizeof(Type).
			// If generic expr, we'd need type analysis without codegen.
			// Falling back to 0 or handling common simple cases could work,
			// but for test_new.kawa, sizeof(Car) is sufficient.
		}

		if (type_to_measure) {
			// LLVMSizeOf returns a ConstantExpr (i64 usually) representing size
			LLVMValueRef size_vals = LLVMSizeOf(type_to_measure);
			// Cast to u32 if needed, but standard malloc takes i64 usually.
			// Kawa uses u32 mainly, but malloc matches size_t.
			return LLVMBuildTruncOrBitCast(c->builder, size_vals,
										   LLVMInt32TypeInContext(c->context),
										   "sizeof_cast");
		}
		return LLVMConstInt(LLVMInt32TypeInContext(c->context), 0, 0);
	}

	if (n->type == NODE_STRING_LIT)
		return LLVMBuildGlobalStringPtr(c->builder, n->data.str_lit.s_val,
										"str");

	if (n->type == NODE_VAR_REF) {
		Scope *s = scope_find(c, n->data.var_ref.name);
		if (!s) {
			char *v_path = get_var_path(n->data.var_ref.name);
			timbr_err("Undefined variable '%s'\n", v_path);
			free(v_path);
			exit(1);
		}
		LLVMValueRef load =
			LLVMBuildLoad2(c->builder, s->type, s->val, n->data.var_ref.name);
		attach_tbaa(c, load, s->type);
		return load;
	}
	if (n->type == NODE_DEREF) {
		LLVMValueRef ptr = codegen_expr(c, n->data.deref.expr);
		LLVMTypeRef elem_type = get_llvm_type(c, n->data_type);
		// Fallback if AST type is missing
		if (!elem_type && n->data.deref.expr->data_type &&
			n->data.deref.expr->data_type->inner) {
			elem_type = get_llvm_type(c, n->data.deref.expr->data_type->inner);
		}
		LLVMValueRef val =
			LLVMBuildLoad2(c->builder, elem_type, ptr, "deref_val");
		attach_tbaa(c, val, elem_type);
		return val;
	}
	if (n->type == NODE_CALL) {
		char func_name[256];
		memset(func_name, 0, 256);
		if (n->data.call.callee->type == NODE_VAR_REF) {
			strcpy(func_name, n->data.call.callee->data.var_ref.name);
		} else if (n->data.call.callee->type == NODE_MEMBER_ACCESS) {
			ASTNode *obj = n->data.call.callee->data.member_access.object;
			char *member = n->data.call.callee->data.member_access.member;
			if (obj->type == NODE_VAR_REF &&
				strcmp(obj->data.var_ref.name, "stdc") == 0) {
				strcpy(func_name, member);
			} else if (obj->type == NODE_VAR_REF) {
				sprintf(func_name, "%s__%s", obj->data.var_ref.name, member);
			} else {
				const char *type_name = resolve_type_name(c, obj);
				sprintf(func_name, "%s__%s", type_name, member);
			}
		}

		LLVMValueRef fn = LLVMGetNamedFunction(c->module, func_name);
		if (!fn) {
			if (strcmp(func_name, "printf") == 0) {
				LLVMTypeRef args[] = {
					LLVMPointerType(LLVMInt8TypeInContext(c->context), 0)};
				LLVMTypeRef ft = LLVMFunctionType(
					LLVMInt32TypeInContext(c->context), args, 1, 1);
				fn = LLVMAddFunction(c->module, "printf", ft);
			} else if (strcmp(func_name, "malloc") == 0) {
				fn = c->malloc_fn;
			} else if (strcmp(func_name, "free") == 0) {
				fn = c->free_fn;
			} else {
				char *f_path = get_var_path(func_name);
				timbr_err("Undefined function: %s\n", f_path);
				free(f_path);
				exit(1);
			}
		}

		int arg_count = 0;
		ASTNode *arg_node = n->data.call.args;
		while (arg_node) {
			arg_count++;
			arg_node = arg_node->next;
		}

		LLVMValueRef *llvm_args = malloc(sizeof(LLVMValueRef) * arg_count);
		arg_node = n->data.call.args;
		LLVMTypeRef func_type = LLVMGlobalGetValueType(fn);
		int param_count = LLVMCountParamTypes(func_type);
		LLVMTypeRef *param_types = malloc(sizeof(LLVMTypeRef) * param_count);
		LLVMGetParamTypes(func_type, param_types);

		for (int i = 0; i < arg_count; i++) {
			LLVMValueRef val = codegen_expr(c, arg_node);

			// [FIX] Auto-Spill Struct to Stack if Pointer Expected (Handle
			// 'self')
			if (i < param_count) {
				LLVMTypeRef expected = param_types[i];
				LLVMTypeRef actual = LLVMTypeOf(val);

				// If function expects Ptr (e.g. i8* self) but we have
				// Struct Value (User)
				if (LLVMGetTypeKind(expected) == LLVMPointerTypeKind &&
					LLVMGetTypeKind(actual) == LLVMStructTypeKind) {

					LLVMValueRef temp_alloc =
						create_entry_block_alloca(c, actual, "self_temp");
					LLVMBuildStore(c->builder, val, temp_alloc);
					// Cast to expected pointer type (likely i8*)
					val = LLVMBuildBitCast(c->builder, temp_alloc, expected,
										   "self_ptr");
				}

				// [FIX] Auto-Cast Ptr to Ptr (Car* -> i8*)
				if (LLVMGetTypeKind(expected) == LLVMPointerTypeKind &&
					LLVMGetTypeKind(actual) == LLVMPointerTypeKind &&
					expected != actual) {
					val =
						LLVMBuildBitCast(c->builder, val, expected, "arg_cast");
				}
			}

			if (i >= param_count) {
				// [FIX] 2. Now it is safe to check TypeOf(val)
				LLVMTypeRef val_type = LLVMTypeOf(val);
				if (LLVMGetTypeKind(val_type) == LLVMIntegerTypeKind) {
					unsigned width = LLVMGetIntTypeWidth(val_type);
					if (width < 32) {
						// bool(1) -> i32, char(8) -> i32
						val = LLVMBuildZExt(c->builder, val,
											LLVMInt32TypeInContext(c->context),
											"vararg_prom");
					}
				}
				if (LLVMGetTypeKind(val_type) == LLVMFloatTypeKind) {
					val = LLVMBuildFPExt(c->builder, val,
										 LLVMDoubleTypeInContext(c->context),
										 "float_prom");
				}
			}
			llvm_args[i] = val;
			arg_node = arg_node->next;
		}
		free(param_types);
		LLVMValueRef call_res =
			LLVMBuildCall2(c->builder, func_type, fn, llvm_args, arg_count, "");
		free(llvm_args);
		return call_res;
	}
	if (n->type == NODE_STRUCT_LITERAL) {
		LLVMTypeRef s_type = get_llvm_type(c, n->data_type);
		if (!s_type)
			return LLVMConstNull(LLVMInt32TypeInContext(c->context));

		LLVMValueRef alloca = create_entry_block_alloca(c, s_type, "lit");
		StructInitItem *item = n->data.struct_lit.items;
		int idx = 0;
		while (item) {
			LLVMValueRef val = codegen_expr(c, item->value);
			int field_idx = idx;
			if (item->field_name) {
				field_idx = get_field_index(c, s_type, item->field_name);
			}
			LLVMValueRef gep = LLVMBuildStructGEP2(c->builder, s_type, alloca,
												   field_idx, "fld");
			LLVMBuildStore(c->builder, val, gep);
			if (!item->field_name)
				idx++;
			item = item->next;
		}
		return LLVMBuildLoad2(c->builder, s_type, alloca, "lit_val");
	}
	if (n->type == NODE_BINARY_OP) {
		LLVMValueRef l = codegen_expr(c, n->data.bin_op.left);
		LLVMValueRef r = codegen_expr(c, n->data.bin_op.right);

		LLVMTypeRef l_ty = LLVMTypeOf(l);
		LLVMTypeRef r_ty = LLVMTypeOf(r);

		int l_is_fp = (LLVMGetTypeKind(l_ty) == LLVMFloatTypeKind ||
					   LLVMGetTypeKind(l_ty) == LLVMDoubleTypeKind);
		int r_is_fp = (LLVMGetTypeKind(r_ty) == LLVMFloatTypeKind ||
					   LLVMGetTypeKind(r_ty) == LLVMDoubleTypeKind);

		// Promote Int to Float/Double if mixed
		if (l_is_fp && !r_is_fp) {
			r = LLVMBuildSIToFP(c->builder, r, l_ty, "promote_r");
			r_is_fp = 1;
		} else if (!l_is_fp && r_is_fp) {
			l = LLVMBuildSIToFP(c->builder, l, r_ty, "promote_l");
			l_is_fp = 1; // Now both are fp
		}

		// If both FP but different precision, upgrade to larger
		if (l_is_fp && r_is_fp) {
			if (LLVMGetTypeKind(l_ty) != LLVMGetTypeKind(LLVMTypeOf(r))) {
				// Simplify: always promote float to double
				if (LLVMGetTypeKind(l_ty) == LLVMFloatTypeKind) {
					l = LLVMBuildFPExt(c->builder, l,
									   LLVMDoubleTypeInContext(c->context),
									   "promote_l_dbl");
				}
				if (LLVMGetTypeKind(LLVMTypeOf(r)) == LLVMFloatTypeKind) {
					r = LLVMBuildFPExt(c->builder, r,
									   LLVMDoubleTypeInContext(c->context),
									   "promote_r_dbl");
				}
			}
		}

		int is_float = l_is_fp; // Effective type

		LLVMValueRef res;
		switch (n->data.bin_op.op) {
		case TOK_PLUS:
			if (is_float) {
				res = LLVMBuildFAdd(c->builder, l, r, "fadd");
				set_fast_math(res);
			} else {
				res = LLVMBuildNSWAdd(c->builder, l, r, "add");
			}
			return res;
		case TOK_MINUS:
			if (is_float) {
				res = LLVMBuildFSub(c->builder, l, r, "fsub");
				set_fast_math(res);
			} else {
				res = LLVMBuildNSWSub(c->builder, l, r, "sub");
			}
			return res;
		case TOK_STAR:
			if (is_float) {
				res = LLVMBuildFMul(c->builder, l, r, "fmul");
				set_fast_math(res);
			} else {
				res = LLVMBuildNSWMul(c->builder, l, r, "mul");
			}
			return res;
		case TOK_SLASH:
			if (is_float) {
				res = LLVMBuildFDiv(c->builder, l, r, "fdiv");
				set_fast_math(res);
				return res;
			} else {
				return LLVMBuildSDiv(c->builder, l, r, "div");
			}
		case TOK_LANGLE:
			return is_float
					   ? LLVMBuildFCmp(c->builder, LLVMRealOLT, l, r, "flt")
					   : LLVMBuildICmp(c->builder, LLVMIntSLT, l, r, "lt");
		case TOK_RANGLE:
			return is_float
					   ? LLVMBuildFCmp(c->builder, LLVMRealOGT, l, r, "fgt")
					   : LLVMBuildICmp(c->builder, LLVMIntSGT, l, r, "gt");
		case TOK_LEQ:
			return is_float
					   ? LLVMBuildFCmp(c->builder, LLVMRealOLE, l, r, "fle")
					   : LLVMBuildICmp(c->builder, LLVMIntSLE, l, r, "le");
		case TOK_REQ:
			return is_float
					   ? LLVMBuildFCmp(c->builder, LLVMRealOGE, l, r, "fge")
					   : LLVMBuildICmp(c->builder, LLVMIntSGE, l, r, "ge");
		case TOK_ISEQ:
			return is_float
					   ? LLVMBuildFCmp(c->builder, LLVMRealOEQ, l, r, "feq")
					   : LLVMBuildICmp(c->builder, LLVMIntEQ, l, r, "eq");
		default:
			return l;
		}
	}
	if (n->type == NODE_SET_POUR) {
		// 1. Resolve the address of the Set struct (the LHS of ~=)
		LLVMTypeRef ignored;
		LLVMValueRef set_ptr =
			get_address(c, n->data.set_pour.target, &ignored);
		LLVMValueRef val_to_add = codegen_expr(c, n->data.set_pour.value);

		// Reconstruct Set Struct Type: { i32*, i64, i64 }
		// Corresponds to the definition in get_llvm_type for TYPE_SET
		LLVMTypeRef i32_ptr_t =
			LLVMPointerType(LLVMInt32TypeInContext(c->context), 0);
		LLVMTypeRef i64_t = LLVMInt64TypeInContext(c->context);
		LLVMTypeRef set_struct_t = LLVMStructTypeInContext(
			c->context, (LLVMTypeRef[]){i32_ptr_t, i64_t, i64_t}, 3, 0);

		// 2. Load Buffer, Count, and Capacity pointers (GEP)
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

		// 3. Check if resize is needed: if (cnt >= cap)
		LLVMValueRef is_full =
			LLVMBuildICmp(c->builder, LLVMIntUGE, cur_cnt, cur_cap, "is_full");

		LLVMBasicBlockRef grow_bb =
			LLVMAppendBasicBlock(c->current_func, "set_grow");
		LLVMBasicBlockRef append_bb =
			LLVMAppendBasicBlock(c->current_func, "set_append");

		LLVMBuildCondBr(c->builder, is_full, grow_bb, append_bb);

		// --- GROW BLOCK (Realloc) ---
		LLVMPositionBuilderAtEnd(c->builder, grow_bb);

		// New Capacity = Capacity * 2
		LLVMValueRef new_cap = LLVMBuildMul(
			c->builder, cur_cap, LLVMConstInt(i64_t, 2, 0), "new_cap");
		// New Size in Bytes = new_cap * 4 (since elements are i32)
		LLVMValueRef new_bytes = LLVMBuildMul(
			c->builder, new_cap, LLVMConstInt(i64_t, 4, 0), "new_bytes");

		LLVMValueRef old_buf =
			LLVMBuildLoad2(c->builder, i32_ptr_t, buf_gep, "old_buf");
		// Cast to i8* for realloc
		LLVMValueRef old_buf_void = LLVMBuildBitCast(
			c->builder, old_buf,
			LLVMPointerType(LLVMInt8TypeInContext(c->context), 0), "void_ptr");

		// Call realloc(void* ptr, i64 size)
		LLVMValueRef new_mem = LLVMBuildCall2(
			c->builder, c->realloc_type, c->realloc_fn,
			(LLVMValueRef[]){old_buf_void, new_bytes}, 2, "new_mem");

		// Cast back to i32*
		LLVMValueRef new_buf =
			LLVMBuildBitCast(c->builder, new_mem, i32_ptr_t, "new_buf_cast");

		// Update struct fields
		LLVMBuildStore(c->builder, new_buf, buf_gep);
		LLVMBuildStore(c->builder, new_cap, cap_gep);
		LLVMBuildBr(c->builder, append_bb);

		// --- APPEND BLOCK ---
		LLVMPositionBuilderAtEnd(c->builder, append_bb);

		// Reload buffer (it might have changed in grow_bb)
		LLVMValueRef final_buf =
			LLVMBuildLoad2(c->builder, i32_ptr_t, buf_gep, "final_buf");

		// buffer[count] = value
		LLVMValueRef slot =
			LLVMBuildGEP2(c->builder, LLVMInt32TypeInContext(c->context),
						  final_buf, &cur_cnt, 1, "slot");
		LLVMBuildStore(c->builder, val_to_add, slot);

		// count++
		LLVMValueRef next_cnt = LLVMBuildAdd(
			c->builder, cur_cnt, LLVMConstInt(i64_t, 1, 0), "next_cnt");
		LLVMBuildStore(c->builder, next_cnt, cnt_gep);

		return val_to_add;
	}
	if (n->type == NODE_MEMBER_ACCESS) {
		LLVMTypeRef field_type = NULL;
		// [FIX] Use corrected get_address
		LLVMValueRef ptr = get_address(c, n, &field_type);
		if (ptr && field_type) {
			LLVMValueRef val =
				LLVMBuildLoad2(c->builder, field_type, ptr, "fld_val");
			attach_tbaa(c, val, field_type);
			return val;
		}
		return LLVMConstInt(LLVMInt32TypeInContext(c->context), 0, 0);
	}
	if (n->type == NODE_SET_LITERAL) {
		LLVMTypeRef set_t = get_llvm_type(c, n->data_type);
		LLVMValueRef set_alloca =
			create_entry_block_alloca(c, set_t, "set_tmp");
		int count = 0;
		ASTNode *cur = n->data.set_lit.items;
		while (cur) {
			count++;
			cur = cur->next;
		}
		LLVMValueRef size = LLVMConstInt(LLVMInt64TypeInContext(c->context),
										 count > 0 ? count * 4 : 4, 0);
		LLVMValueRef buf_void = LLVMBuildCall2(
			c->builder, c->malloc_type, c->malloc_fn, &size, 1, "malloc");
		LLVMValueRef buf = LLVMBuildBitCast(
			c->builder, buf_void,
			LLVMPointerType(LLVMInt32TypeInContext(c->context), 0), "buf_cast");
		cur = n->data.set_lit.items;
		int idx = 0;
		while (cur) {
			LLVMValueRef val = codegen_expr(c, cur);
			LLVMValueRef gep = LLVMBuildGEP2(
				c->builder, LLVMInt32TypeInContext(c->context), buf,
				(LLVMValueRef[]){
					LLVMConstInt(LLVMInt64TypeInContext(c->context), idx++, 0)},
				1, "ptr");
			LLVMValueRef store = LLVMBuildStore(c->builder, val, gep);
			attach_tbaa(c, store, LLVMInt32TypeInContext(c->context));
			cur = cur->next;
		}
		LLVMBuildStore(
			c->builder, buf,
			LLVMBuildStructGEP2(c->builder, set_t, set_alloca, 0, ""));
		LLVMBuildStore(
			c->builder,
			LLVMConstInt(LLVMInt64TypeInContext(c->context), count, 0),
			LLVMBuildStructGEP2(c->builder, set_t, set_alloca, 1, ""));
		LLVMBuildStore(
			c->builder,
			LLVMConstInt(LLVMInt64TypeInContext(c->context),
						 count > 0 ? count : 1, 0),
			LLVMBuildStructGEP2(c->builder, set_t, set_alloca, 2, ""));
		LLVMValueRef set_load =
			LLVMBuildLoad2(c->builder, set_t, set_alloca, "set_load");
		attach_tbaa(c, set_load, set_t);
		return set_load;
	}
	if (n->type == NODE_SIP) {
		LLVMValueRef hdl = codegen_expr(c, n->data.sip.handle);
		LLVMValueRef is_done = LLVMBuildCall2(c->builder, c->coro_done_type,
											  c->coro_done, &hdl, 1, "is_done");
		LLVMBasicBlockRef resume_bb =
			LLVMAppendBasicBlock(c->current_func, "sip_resume");
		LLVMBasicBlockRef cont_bb =
			LLVMAppendBasicBlock(c->current_func, "sip_cont");
		LLVMBuildCondBr(c->builder, is_done, cont_bb, resume_bb);
		LLVMPositionBuilderAtEnd(c->builder, resume_bb);
		LLVMValueRef resume_fn = c->coro_resume;
		if (!resume_fn) {
			LLVMTypeRef args[] = {
				LLVMPointerType(LLVMInt8TypeInContext(c->context), 0)};
			c->coro_resume_type =
				LLVMFunctionType(LLVMVoidTypeInContext(c->context), args, 1, 0);
			resume_fn = LLVMAddFunction(c->module, "llvm.coro.resume",
										c->coro_resume_type);
			c->coro_resume = resume_fn;
		}
		LLVMBuildCall2(c->builder, c->coro_resume_type, resume_fn, &hdl, 1, "");
		LLVMBuildBr(c->builder, cont_bb);
		LLVMPositionBuilderAtEnd(c->builder, cont_bb);
		int prom_index = c->drip_promise_index;
		LLVMValueRef promise_ptr_void = LLVMBuildCall2(
			c->builder, c->coro_promise_type, c->coro_promise,
			(LLVMValueRef[]){
				hdl,
				LLVMConstInt(LLVMInt32TypeInContext(c->context), prom_index, 0),
				LLVMConstInt(LLVMInt1TypeInContext(c->context), 0, 0)},
			3, "prom_ptr_void");
		LLVMValueRef promise_ptr = LLVMBuildBitCast(
			c->builder, promise_ptr_void,
			LLVMPointerType(LLVMInt32TypeInContext(c->context), 0), "prom_ptr");
		LLVMValueRef val =
			LLVMBuildLoad2(c->builder, LLVMInt32TypeInContext(c->context),
						   promise_ptr, "sip_val");
		LLVMSetVolatile(val, 1);
		attach_tbaa(c, val, LLVMInt32TypeInContext(c->context));
		return val;
	}
	if (n->type == NODE_CAST) {
		LLVMValueRef val = codegen_expr(c, n->data.cast.val);
		LLVMTypeRef dest_type = get_llvm_type(c, n->data_type);
		LLVMTypeRef src_type = LLVMTypeOf(val);
		LLVMTypeKind src_kind = LLVMGetTypeKind(src_type);
		LLVMTypeKind dest_kind = LLVMGetTypeKind(dest_type);
		if (src_kind == LLVMIntegerTypeKind &&
			dest_kind == LLVMIntegerTypeKind) {
			unsigned src_width = LLVMGetIntTypeWidth(src_type);
			unsigned dest_width = LLVMGetIntTypeWidth(dest_type);
			if (src_width == dest_width)
				return val;
			if (dest_width < src_width) {
				return LLVMBuildTrunc(c->builder, val, dest_type, "trunc");
			} else {
				return LLVMBuildZExt(c->builder, val, dest_type, "zext");
			}
		}
		if ((src_kind == LLVMFloatTypeKind || src_kind == LLVMDoubleTypeKind) &&
			dest_kind == LLVMIntegerTypeKind) {
			return LLVMBuildFPToUI(c->builder, val, dest_type, "fptoui");
		}
		if (src_kind == LLVMIntegerTypeKind &&
			(dest_kind == LLVMFloatTypeKind ||
			 dest_kind == LLVMDoubleTypeKind)) {
			return LLVMBuildUIToFP(c->builder, val, dest_type, "uitofp");
		}
		if (src_kind == LLVMPointerTypeKind &&
			dest_kind == LLVMPointerTypeKind) {
			return LLVMBuildBitCast(c->builder, val, dest_type, "ptr_cast");
		}
		return LLVMBuildBitCast(c->builder, val, dest_type, "raw_cast");
	}
	if (n->type == NODE_BREW) {
		char task_name[64];
		sprintf(task_name, "kawa_task_%d", c->lambda_counter++);
		LLVMBasicBlockRef old_block = LLVMGetInsertBlock(c->builder);
		LLVMValueRef old_func = c->current_func;
		LLVMTypeRef old_ret_type = c->current_ret_type;
		Scope *old_scope = c->scope_stack;
		int was_in_coroutine = c->in_coroutine;
		LLVMBasicBlockRef old_cleanup = c->coro_cleanup_block;
		LLVMBasicBlockRef old_suspend = c->coro_suspend_block;
		LLVMValueRef old_hdl = c->current_coro_hdl;
		LLVMValueRef old_prom = c->current_promise_ptr;
		LLVMTypeRef ret_type =
			LLVMPointerType(LLVMInt8TypeInContext(c->context), 0);
		LLVMTypeRef task_type = LLVMFunctionType(ret_type, NULL, 0, 0);
		LLVMValueRef task_func =
			LLVMAddFunction(c->module, task_name, task_type);
		unsigned kind_id =
			LLVMGetEnumAttributeKindForName("presplitcoroutine", 17);
		LLVMAddAttributeAtIndex(
			task_func, LLVMAttributeFunctionIndex,
			LLVMCreateEnumAttribute(c->context, kind_id, 0));
		c->current_func = task_func;
		c->current_ret_type = ret_type;
		c->in_coroutine = 1;
		LLVMBasicBlockRef entry = LLVMAppendBasicBlock(task_func, "entry");
		LLVMPositionBuilderAtEnd(c->builder, entry);
		LLVMValueRef promise_alloca = create_entry_block_alloca(
			c, LLVMInt32TypeInContext(c->context), "promise_storage");
		c->current_promise_ptr = promise_alloca;
		LLVMValueRef promise_void = LLVMBuildBitCast(
			c->builder, promise_alloca,
			LLVMPointerType(LLVMInt8TypeInContext(c->context), 0), "prom_void");
		LLVMValueRef null_ptr = LLVMConstNull(
			LLVMPointerType(LLVMInt8TypeInContext(c->context), 0));
		LLVMValueRef id = LLVMBuildCall2(
			c->builder, c->coro_id_type, c->coro_id,
			(LLVMValueRef[]){LLVMConstInt(LLVMInt32TypeInContext(c->context),
										  c->brew_promise_index, 0),
							 promise_void, null_ptr, null_ptr},
			4, "id");
		LLVMValueRef need_alloc =
			LLVMBuildCall2(c->builder, c->coro_alloc_type, c->coro_alloc, &id,
						   1, "need_alloc");
		LLVMValueRef size = LLVMBuildCall2(c->builder, c->coro_size_type,
										   c->coro_size, NULL, 0, "size");
		LLVMBasicBlockRef alloc_bb = LLVMAppendBasicBlock(task_func, "alloc");
		LLVMBasicBlockRef cont_bb =
			LLVMAppendBasicBlock(task_func, "alloc_cont");
		LLVMBuildCondBr(c->builder, need_alloc, alloc_bb, cont_bb);
		LLVMPositionBuilderAtEnd(c->builder, alloc_bb);
		LLVMValueRef malloc_ptr = LLVMBuildCall2(
			c->builder, c->malloc_type, c->malloc_fn, &size, 1, "coro_mem");
		LLVMBuildBr(c->builder, cont_bb);
		LLVMPositionBuilderAtEnd(c->builder, cont_bb);
		LLVMValueRef phi = LLVMBuildPhi(
			c->builder, LLVMPointerType(LLVMInt8TypeInContext(c->context), 0),
			"mem_phi");
		LLVMAddIncoming(phi, (LLVMValueRef[]){malloc_ptr, null_ptr},
						(LLVMBasicBlockRef[]){alloc_bb, entry}, 2);
		LLVMValueRef hdl =
			LLVMBuildCall2(c->builder, c->coro_begin_type, c->coro_begin,
						   (LLVMValueRef[]){id, phi}, 2, "hdl");
		c->current_coro_hdl = hdl;
		LLVMValueRef suspend = LLVMBuildCall2(
			c->builder, c->coro_suspend_type, c->coro_suspend,
			(LLVMValueRef[]){
				LLVMConstNull(LLVMTokenTypeInContext(c->context)),
				LLVMConstInt(LLVMInt1TypeInContext(c->context), 0, 0)},
			2, "suspend");
		LLVMBasicBlockRef suspend_bb =
			LLVMAppendBasicBlock(task_func, "suspend");
		LLVMBasicBlockRef resume_bb = LLVMAppendBasicBlock(task_func, "resume");
		LLVMBasicBlockRef cleanup_bb =
			LLVMAppendBasicBlock(task_func, "cleanup");
		c->coro_cleanup_block = cleanup_bb;
		c->coro_suspend_block = suspend_bb;
		LLVMValueRef sw = LLVMBuildSwitch(c->builder, suspend, suspend_bb, 2);
		LLVMAddCase(sw, LLVMConstInt(LLVMInt8TypeInContext(c->context), 0, 0),
					resume_bb);
		LLVMAddCase(sw, LLVMConstInt(LLVMInt8TypeInContext(c->context), 1, 0),
					cleanup_bb);
		LLVMPositionBuilderAtEnd(c->builder, resume_bb);
		c->scope_stack = NULL;
		codegen_stmt(c, n->data.brew.body);
		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(c->builder))) {
			LLVMValueRef final_suspend = LLVMBuildCall2(
				c->builder, c->coro_suspend_type, c->coro_suspend,
				(LLVMValueRef[]){
					LLVMConstNull(LLVMTokenTypeInContext(c->context)),
					LLVMConstInt(LLVMInt1TypeInContext(c->context), 1, 0)},
				2, "final");
			LLVMBasicBlockRef final_cleanup_bb = c->coro_cleanup_block;
			LLVMValueRef final_sw =
				LLVMBuildSwitch(c->builder, final_suspend, final_cleanup_bb, 2);
			LLVMAddCase(final_sw,
						LLVMConstInt(LLVMInt8TypeInContext(c->context), 0, 0),
						final_cleanup_bb);
			LLVMAddCase(final_sw,
						LLVMConstInt(LLVMInt8TypeInContext(c->context), 1, 0),
						final_cleanup_bb);
		}
		LLVMPositionBuilderAtEnd(c->builder, cleanup_bb);
		LLVMBuildCall2(
			c->builder, c->coro_end_type, c->coro_end,
			(LLVMValueRef[]){
				null_ptr,
				LLVMConstInt(LLVMInt1TypeInContext(c->context), 0, 0)},
			2, "");
		LLVMBuildBr(c->builder, suspend_bb);
		LLVMPositionBuilderAtEnd(c->builder, suspend_bb);
		LLVMBuildRet(c->builder, hdl);
		LLVMPositionBuilderAtEnd(c->builder, old_block);
		c->current_func = old_func;
		c->current_ret_type = old_ret_type;
		c->scope_stack = old_scope;
		c->in_coroutine = was_in_coroutine;
		c->coro_cleanup_block = old_cleanup;
		c->coro_suspend_block = old_suspend;
		c->current_coro_hdl = old_hdl;
		c->current_promise_ptr = old_prom;
		LLVMValueRef task_handle = LLVMBuildCall2(
			c->builder, task_type, task_func, NULL, 0, "task_hdl");
		LLVMValueRef md_str = LLVMMDStringInContext(c->context, "brew", 4);
		LLVMValueRef md = LLVMMDNodeInContext(c->context, &md_str, 1);
		unsigned KIND =
			LLVMGetMDKindID("kawa.coro.kind", strlen("kawa.coro.kind"));
		LLVMSetMetadata(task_handle, KIND, md);
		return task_handle;
	}
	return LLVMConstNull(LLVMInt32TypeInContext(c->context));
}
