#include "codegen_internal.h"
#include <math.h>

void kawa_check_conversion(KawaCompiler *c, LLVMValueRef value, Type *source,
                           LLVMTypeRef dst, Type *dest_ast) {
    if (!source || !dest_ast || LLVMGetTypeKind(dst)!=LLVMIntegerTypeKind) return;
    LLVMTypeRef src=LLVMTypeOf(value);
    unsigned dw=LLVMGetIntTypeWidth(dst);
    if (dw==1) return; // bool has a truth-value conversion
    int ds=type_is_signed(c,dest_ast);
    LLVMValueRef ok=NULL;
    if (is_fp_kind(LLVMGetTypeKind(src))) {
        double minimum=ds ? -ldexp(1.0,(int)dw-1) : 0.0;
        double end=ldexp(1.0,(int)dw-(ds ? 1 : 0));
        ok=LLVMBuildAnd(c->builder,
            LLVMBuildFCmp(c->builder,LLVMRealOGE,value,LLVMConstReal(src,minimum),"conversion_lower"),
            LLVMBuildFCmp(c->builder,LLVMRealOLT,value,LLVMConstReal(src,end),"conversion_upper"),"conversion_fits");
    } else if (LLVMGetTypeKind(src)==LLVMIntegerTypeKind) {
        unsigned sw=LLVMGetIntTypeWidth(src);
        int ss=type_is_signed(c,source);
        ok=LLVMConstInt(LLVMInt1TypeInContext(c->context),1,0);
        if (ss && (!ds || dw<sw)) {
            uint64_t minimum=ds ? ~((1ULL<<(dw-1))-1) : 0;
            ok=LLVMBuildICmp(c->builder,LLVMIntSGE,value,LLVMConstInt(src,minimum,1),"conversion_lower");
        }
        if (dw<sw || (ds && !ss && dw<=sw)) {
            uint64_t maximum=ds ? (1ULL<<(dw-1))-1 : (1ULL<<dw)-1;
            LLVMValueRef upper=LLVMBuildICmp(c->builder,ss ? LLVMIntSLE : LLVMIntULE,value,
                                            LLVMConstInt(src,maximum,0),"conversion_upper");
            ok=LLVMBuildAnd(c->builder,ok,upper,"conversion_fits");
        }
    }
    if (ok) {
        if (!LLVMGetInsertBlock(c->builder)) {
            if (!LLVMIsAConstantInt(ok) || !LLVMConstIntGetZExtValue(ok)) {
				kdiag_error_at(KAWA_E_TYPE,c->source_filename,NULL,c->source_line,"constant numeric conversion out of range"); exit(1);
            }
        } else emit_check_or_trap(c,NULL,ok,"numeric conversion out of range");
    }
}

// policy: 0 checked, 1 wrapping, 2 saturating. Explicit operations preserve
// the first operand's width; ordinary operators retain integer promotion.
static LLVMValueRef overflow(KawaCompiler *c, LLVMValueRef a, LLVMValueRef b,
                              int op, int sign) {
    LLVMTypeRef type = LLVMTypeOf(a);
    const char *base = op == TOK_PLUS ? "add" : op == TOK_MINUS ? "sub" : "mul";
    char name[64];
    snprintf(name, sizeof(name), "llvm.%c%s.with.overflow.i%u", sign ? 's' : 'u', base,
              LLVMGetIntTypeWidth(type));
    LLVMValueRef fn = LLVMGetNamedFunction(c->module, name);
    if (!fn) {
        LLVMTypeRef fields[] = {type, LLVMInt1TypeInContext(c->context)};
        fn = LLVMAddFunction(c->module, name, LLVMFunctionType(
            LLVMStructTypeInContext(c->context, fields, 2, 0), (LLVMTypeRef[]){type,type}, 2, 0));
    }
    return LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(fn), fn, (LLVMValueRef[]){a,b}, 2, "arithmetic");
}
LLVMValueRef kawa_integer_op(KawaCompiler *c, ASTNode *n, int op, LLVMValueRef a,
                              LLVMValueRef b, int sign, int policy) {
    LLVMTypeRef type = LLVMTypeOf(a);
    unsigned width = LLVMGetIntTypeWidth(type);
    if (op == TOK_SLASH || op == TOK_PERCENT) {
        LLVMValueRef ok = LLVMBuildICmp(c->builder, LLVMIntNE, b, LLVMConstNull(type), "nonzero_divisor");
        if (sign) {
            LLVMValueRef minimum = LLVMConstInt(type, 1ULL << (width - 1), 0);
            LLVMValueRef bad = LLVMBuildAnd(c->builder,
                LLVMBuildICmp(c->builder, LLVMIntEQ, a, minimum, "minimum"),
                LLVMBuildICmp(c->builder, LLVMIntEQ, b, LLVMConstAllOnes(type), "minus_one"), "division_overflow");
            ok = LLVMBuildAnd(c->builder, ok, LLVMBuildNot(c->builder, bad, "valid_division"), "division_ok");
        }
        emit_check_or_trap(c, n, ok, "invalid integer division");
        if (op == TOK_SLASH) return sign ? LLVMBuildSDiv(c->builder,a,b,"div") : LLVMBuildUDiv(c->builder,a,b,"div");
        return sign ? LLVMBuildSRem(c->builder,a,b,"rem") : LLVMBuildURem(c->builder,a,b,"rem");
    }
    if (op == TOK_SHL || op == TOK_SHR) {
        emit_check_or_trap(c, n, LLVMBuildICmp(c->builder, LLVMIntULT,b,LLVMConstInt(type,width,0),"valid_shift"),
                           "integer shift count out of range");
        if (op == TOK_SHL) return LLVMBuildShl(c->builder,a,b,"shift");
        return sign ? LLVMBuildAShr(c->builder,a,b,"shift") : LLVMBuildLShr(c->builder,a,b,"shift");
    }
    if (policy == 1) {
        if (op == TOK_PLUS) return LLVMBuildAdd(c->builder,a,b,"wrap_add");
        if (op == TOK_MINUS) return LLVMBuildSub(c->builder,a,b,"wrap_sub");
        return LLVMBuildMul(c->builder,a,b,"wrap_mul");
    }
    LLVMValueRef pair = overflow(c,a,b,op,sign);
    LLVMValueRef value = LLVMBuildExtractValue(c->builder,pair,0,"arithmetic_value");
    LLVMValueRef failed = LLVMBuildExtractValue(c->builder,pair,1,"overflow");
    if (!policy) {
        emit_check_or_trap(c,n,LLVMBuildNot(c->builder,failed,"no_overflow"),"integer arithmetic overflow");
        return value;
    }
    LLVMValueRef clamp;
    if (sign) {
        LLVMValueRef negative = LLVMBuildICmp(c->builder,LLVMIntSLT,a,LLVMConstNull(type),"negative");
        if (op == TOK_STAR)
            negative = LLVMBuildXor(c->builder,negative,
                LLVMBuildICmp(c->builder,LLVMIntSLT,b,LLVMConstNull(type),"negative_rhs"),"negative_product");
        clamp = LLVMBuildSelect(c->builder,negative,LLVMConstInt(type,1ULL<<(width-1),0),
                                 LLVMConstInt(type,(1ULL<<(width-1))-1,0),"signed_clamp");
    } else clamp = op == TOK_MINUS ? LLVMConstNull(type) : LLVMConstAllOnes(type);
    return LLVMBuildSelect(c->builder,failed,clamp,value,"saturated");
}
LLVMValueRef kawa_numeric_builtin(KawaCompiler *c, ASTNode *n, const char *name) {
    if (!strncmp(name,"lossy_",6)) {
        const char *types[]={"i8","u8","i16","u16","i32","u32","i64","u64",NULL};
        TypeKind kinds[]={TYPE_I8,TYPE_U8,TYPE_I16,TYPE_U16,TYPE_I32,TYPE_U32,TYPE_I64,TYPE_U64};
        unsigned index=0; while (types[index] && strcmp(types[index],name+6)) ++index;
        if (!types[index]) return NULL;
        ASTNode *arg=n->data.call.args;
        if (!arg || arg->next) { kerr(KAWA_E_ARITY,n,"%s expects one numeric argument",name); exit(1); }
        Type *destination=arena_alloc(c->arena,sizeof(*destination)); destination->kind=kinds[index];
        n->data_type=destination;
        LLVMValueRef value=codegen_expr(c,arg);
        LLVMTypeRef dst=get_llvm_type(c,destination),src=LLVMTypeOf(value);
        if (is_fp_kind(LLVMGetTypeKind(src))) {
            const char *fp=LLVMGetTypeKind(src)==LLVMHalfTypeKind ? "f16" :
                LLVMGetTypeKind(src)==LLVMBFloatTypeKind ? "bf16" : LLVMGetTypeKind(src)==LLVMFloatTypeKind ? "f32" : "f64";
            char intrinsic[80];
            snprintf(intrinsic,sizeof(intrinsic),"llvm.fpto%ci.sat.i%u.%s",
                      type_is_signed(c,destination) ? 's' : 'u',LLVMGetIntTypeWidth(dst),fp);
            LLVMValueRef fn=LLVMGetNamedFunction(c->module,intrinsic);
            if (!fn) fn=LLVMAddFunction(c->module,intrinsic,LLVMFunctionType(dst,&src,1,0));
            return LLVMBuildCall2(c->builder,LLVMGlobalGetValueType(fn),fn,&value,1,"lossy_float");
        }
        if (LLVMGetTypeKind(src)!=LLVMIntegerTypeKind) { kerr(KAWA_E_TYPE,n,"lossy conversion requires a number"); exit(1); }
        return LLVMBuildIntCast2(c->builder,value,dst,type_is_signed(c,kawa_expr_type(c,arg)),"lossy_integer");
    }
    int policy;
    const char *operation;
    if (!strncmp(name,"checked_",8)) { policy=0; operation=name+8; }
    else if (!strncmp(name,"wrap_",5)) { policy=1; operation=name+5; }
    else if (!strncmp(name,"sat_",4)) { policy=2; operation=name+4; }
    else return NULL;
    int op = !strcmp(operation,"add") ? TOK_PLUS : !strcmp(operation,"sub") ? TOK_MINUS :
             !strcmp(operation,"mul") ? TOK_STAR : 0;
    if (!op) return NULL;
    ASTNode *a=n->data.call.args;
    if (!a || !a->next || a->next->next) {
        kerr(KAWA_E_ARITY,n,"%s expects two integer arguments",name); exit(1);
    }
    LLVMValueRef left=codegen_expr(c,a);
    LLVMValueRef right=codegen_expr(c,a->next);
    Type *type=kawa_expr_type(c,a);
    if (!type || LLVMGetTypeKind(LLVMTypeOf(left))!=LLVMIntegerTypeKind ||
        LLVMGetTypeKind(LLVMTypeOf(right))!=LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(left))<8) {
        kerr(KAWA_E_TYPE,n,"explicit arithmetic requires i8/i16/i32/i64 or unsigned integers"); exit(1);
    }
    n->data_type=type;
    right=coerce_value(c,right,kawa_expr_type(c,a->next),LLVMTypeOf(left),type);
    return kawa_integer_op(c,n,op,left,right,type_is_signed(c,type),policy);
}
