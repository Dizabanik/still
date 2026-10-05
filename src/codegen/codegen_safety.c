#include "codegen_internal.h"
#include <llvm-c/Error.h>

// Analyze a private SSA copy: diagnostics must work at O0 too, without
// changing the user's debug IR. SROA exposes pointers inside structs/slices;
// mem2reg follows local assignments and merges them at branches/loops.
static int carries_pointer(LLVMTypeRef t) {
	switch (LLVMGetTypeKind(t)) {
	case LLVMPointerTypeKind:
		return 1;
	case LLVMArrayTypeKind:
		return carries_pointer(LLVMGetElementType(t));
	case LLVMStructTypeKind:
		for (unsigned i = 0; i < LLVMCountStructElementTypes(t); i++)
			if (carries_pointer(LLVMStructGetTypeAtIndex(t, i)))
				return 1;
	default:
		return 0;
	}
}

static LLVMValueRef storage_root(LLVMValueRef v) {
	while (LLVMIsAGetElementPtrInst(v) || LLVMIsABitCastInst(v) ||
		   LLVMIsAAddrSpaceCastInst(v))
		v = LLVMGetOperand(v, 0);
	return v;
}

// A local destination must be proved local on every incoming path. Cycles
// and unknown calls are deliberately not evidence of a local destination.
static int local_address(LLVMValueRef v, unsigned depth) {
	if (depth > 128)
		return 0;
	v = storage_root(v);
	if (LLVMIsAAllocaInst(v))
		return 1;
	if (LLVMIsAPHINode(v)) {
		for (unsigned i = 0; i < LLVMCountIncoming(v); i++)
			if (!local_address(LLVMGetIncomingValue(v, i), depth + 1))
				return 0;
		return 1;
	}
	if (LLVMIsASelectInst(v))
		return local_address(LLVMGetOperand(v, 1), depth + 1) &&
			   local_address(LLVMGetOperand(v, 2), depth + 1);
	return 0;
}
static int memory_copy(LLVMValueRef in) {
	if (!LLVMIsACallInst(in) || LLVMGetNumArgOperands(in)<2) return 0;
	LLVMValueRef fn=LLVMGetCalledValue(in);
	if (!LLVMIsAFunction(fn)) return 0;
	const char *name=LLVMGetValueName(fn);
	return ((!strcmp(name,"memcpy") || !strcmp(name,"memmove")) && !LLVMCountBasicBlocks(fn)) ||
		!strncmp(name,"llvm.memcpy.",12) || !strncmp(name,"llvm.memmove.",13);
}
/* An aggregate's scalar fields can carry encoded pointers (enum payload words
 * are i64 in the ABI). Track the selected field instead of discarding scalar
 * extracts or tainting every field of a slice/struct. */
#define FLOW_SELECT_MAX 32
typedef struct ResultBinding {
    LLVMValueRef parameter, argument, caller_origin;
    struct ResultBinding *previous;
} ResultBinding;
static ResultBinding *result_binding;
static int summarize_bits;
typedef struct {
    LLVMValueRef value, origin;
    ResultBinding *binding;
    unsigned count, indices[FLOW_SELECT_MAX];
} FlowPath;
typedef int (*ValueFlow)(LLVMValueRef,LLVMValueRef,FlowPath *,unsigned);
static int local_flow(LLVMValueRef,LLVMValueRef,FlowPath *,unsigned);
static int parameter_flow(LLVMValueRef,LLVMValueRef,FlowPath *,unsigned);
static int origin_flow(LLVMValueRef,LLVMValueRef,const unsigned *,unsigned,FlowPath *,unsigned);

static int enter_flow(LLVMValueRef value,LLVMValueRef origin,const unsigned *indices,
                      unsigned count,FlowPath *path,unsigned depth) {
    if (depth>=256 || count>FLOW_SELECT_MAX) return -1;
    for (unsigned i=0; i<depth; ++i)
        if (path[i].value==value && path[i].origin==origin && path[i].count==count &&
            (!count || !memcmp(path[i].indices,indices,count*sizeof(*indices))))
            return path[i].binding==result_binding ? 0 : 2;
    path[depth].value=value; path[depth].origin=origin; path[depth].count=count;
    path[depth].binding=result_binding;
    if (count) memcpy(path[depth].indices,indices,count*sizeof(*indices));
    return 1;
}

/* Follow contents separately from addresses: copying a numeric local buffer
 * out is safe, copying a pointer stored in that buffer may not be. Include
 * residual memcpy/memmove edges that SROA cannot turn into scalar stores. */
static int contents_flow(LLVMValueRef address,LLVMValueRef origin,LLVMValueRef fn,
        FlowPath *path,unsigned depth,ValueFlow flow) {
	if (depth>=256) return 1;
	LLVMValueRef root=storage_root(address);
    int entered=enter_flow(root,origin,NULL,0,path,depth);
    if (entered<=0) return entered<0;
    ++depth;
	if (LLVMIsAPHINode(root) || LLVMIsASelectInst(root)) {
		unsigned first=LLVMIsAPHINode(root) ? 0 : 1;
		for (unsigned i=first; i<(unsigned)LLVMGetNumOperands(root); ++i)
			if (contents_flow(LLVMGetOperand(root,i),origin,fn,path,depth,flow)) return 1;
	}
	if (!LLVMIsAAllocaInst(root))
		return origin && parameter_flow(address,origin,path,depth-1);
	for (LLVMBasicBlockRef bb=LLVMGetFirstBasicBlock(fn); bb; bb=LLVMGetNextBasicBlock(bb))
		for (LLVMValueRef in=LLVMGetFirstInstruction(bb); in; in=LLVMGetNextInstruction(in)) {
			if (LLVMIsAStoreInst(in) && storage_root(LLVMGetOperand(in,1))==root &&
				flow(LLVMGetOperand(in,0),origin,path,depth)) return 1;
			if (memory_copy(in) && storage_root(LLVMGetArgOperand(in,0))==root &&
				contents_flow(LLVMGetArgOperand(in,1),origin,fn,path,depth,flow)) return 1;
		}
	return 0;
}

// Existential provenance: any incoming local address makes a returned view
// unsafe. A path stack terminates SSA cycles without losing the other inputs.
static int origin_flow(LLVMValueRef v,LLVMValueRef origin,const unsigned *indices,
                        unsigned count,FlowPath *path,unsigned depth) {
    if (origin && v==origin) {
        for (ResultBinding *b=result_binding; b; b=b->previous) {
            if (b->parameter!=origin) continue;
            ResultBinding *saved=result_binding;
            result_binding=b->previous;
            int result=origin_flow(b->argument,b->caller_origin,indices,count,path,depth);
            result_binding=saved;
            return result;
        }
        if (summarize_bits) return 1;
        LLVMTypeRef selected=LLVMTypeOf(v);
        for (unsigned i=0; i<count; ++i) {
            LLVMTypeKind kind=LLVMGetTypeKind(selected);
            if (kind==LLVMStructTypeKind) {
                const char *name=LLVMGetStructName(selected);
                if (name && !strncmp(name,"enum.",5) && indices[i]!=0) return 1;
                if (indices[i]>=LLVMCountStructElementTypes(selected)) return 1;
                selected=LLVMStructGetTypeAtIndex(selected,indices[i]);
            } else if (kind==LLVMArrayTypeKind) selected=LLVMGetElementType(selected);
            else return 1;
        }
        return !count || carries_pointer(selected);
    }
    if (!origin && LLVMIsAAllocaInst(v)) return 1;
    if (!LLVMIsAInstruction(v)) return 0;
    int entered=enter_flow(v,origin,indices,count,path,depth);
    if (entered<=0) return entered<0;
    if (entered==2) {
        /* A recursive call may permute aggregate fields. Join its parameter
         * dependencies instead of mistaking a new argument mapping for an SSA
         * cycle. Scalar readers still have no return address dependency. */
        ResultBinding *saved=result_binding;
        int saved_summary=summarize_bits;
        result_binding=NULL;
        summarize_bits=1;
        FlowPath *conservative=malloc(sizeof(*conservative)*256);
        if (!conservative) { result_binding=saved; summarize_bits=saved_summary; return 1; }
        int result=origin_flow(v,origin,indices,count,conservative,0);
        free(conservative); result_binding=saved; summarize_bits=saved_summary;
        return result;
    }
    ++depth;
    if (LLVMIsAExtractValueInst(v)) {
        unsigned prefix=LLVMGetNumIndices(v), selected[FLOW_SELECT_MAX];
        if (prefix+count>FLOW_SELECT_MAX) return 1;
        memcpy(selected,LLVMGetIndices(v),prefix*sizeof(*selected));
        if (count) memcpy(selected+prefix,indices,count*sizeof(*selected));
        return origin_flow(LLVMGetOperand(v,0),origin,selected,prefix+count,path,depth);
    }
    if (count && LLVMIsAInsertValueInst(v)) {
        unsigned inserted_count=LLVMGetNumIndices(v);
        const unsigned *inserted=LLVMGetIndices(v);
        unsigned shared=count<inserted_count ? count : inserted_count;
        if (memcmp(indices,inserted,shared*sizeof(*indices)))
            return origin_flow(LLVMGetOperand(v,0),origin,indices,count,path,depth);
        if (count>=inserted_count)
            return origin_flow(LLVMGetOperand(v,1),origin,indices+inserted_count,
                               count-inserted_count,path,depth);
        return origin_flow(LLVMGetOperand(v,1),origin,NULL,0,path,depth) ||
               origin_flow(LLVMGetOperand(v,0),origin,indices,count,path,depth);
    }
    if (LLVMIsALoadInst(v)) {
		// Residual address-taken aggregate storage (not promotable by SROA).
		LLVMValueRef root = storage_root(LLVMGetOperand(v, 0));
		LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInstructionParent(v));
        if (!LLVMIsAAllocaInst(root))
            return origin && carries_pointer(LLVMTypeOf(v)) &&
                   parameter_flow(LLVMGetOperand(v,0),origin,path,depth);
        return contents_flow(root,origin,fn,path,depth,origin ? parameter_flow : local_flow);
	}
	if (LLVMIsACallInst(v)) {
        LLVMValueRef fn=LLVMGetCalledValue(v);
        const char *name=LLVMIsAFunction(fn) ? LLVMGetValueName(fn) : "";
        /* Runtime results never borrow a caller's slot address. Descriptor
         * metadata and allocation addresses have independent lifetimes. */
        if (LLVMIsAFunction(fn) &&
            !LLVMGetStringAttributeAtIndex(fn,LLVMAttributeFunctionIndex,"kawa.source",11) &&
            !strncmp(name,"__kawa_mem_",11)) return 0;
        int defined=LLVMIsAFunction(fn) && LLVMCountBasicBlocks(fn);
        if (!defined && !carries_pointer(LLVMTypeOf(v))) return 0;
        for (unsigned i=0; i<LLVMGetNumArgOperands(v); ++i) {
            if (!origin_flow(LLVMGetArgOperand(v,i),origin,NULL,0,path,depth)) continue;
            if (!defined || i>=LLVMCountParams(fn)) return 1;
            /* Inspect returns even for scalar results: ptrtoint is not an
             * address-lifetime boundary. The path includes the parameter so
             * recursive/mutually recursive helpers terminate independently. */
            LLVMValueRef param=LLVMGetParam(fn,i);
            ResultBinding binding={param,LLVMGetArgOperand(v,i),origin,result_binding};
            ResultBinding *saved=result_binding;
            if (!summarize_bits && (result_binding || !origin || origin!=param)) result_binding=&binding;
            int result=0;
            for (LLVMBasicBlockRef bb=LLVMGetFirstBasicBlock(fn); bb; bb=LLVMGetNextBasicBlock(bb))
                for (LLVMValueRef in=LLVMGetFirstInstruction(bb); in; in=LLVMGetNextInstruction(in))
                    if (LLVMIsAReturnInst(in) && LLVMGetNumOperands(in) &&
                        origin_flow(LLVMGetOperand(in,0),param,indices,count,path,depth)) result=1;
            result_binding=saved;
            if (result) return 1;
        }
        return 0;
    }
    if (LLVMIsAICmpInst(v) || LLVMIsAFCmpInst(v)) return 0;
    if (LLVMIsAPHINode(v)) {
        for (unsigned i=0; i<LLVMCountIncoming(v); ++i)
            if (origin_flow(LLVMGetIncomingValue(v,i),origin,indices,count,path,depth)) return 1;
        return 0;
    }
    for (int i = 0; i < LLVMGetNumOperands(v); i++)
        if (!(LLVMIsASelectInst(v) && i==0))
        if (origin_flow(LLVMGetOperand(v,i),origin,indices,count,path,depth))
            return 1;
    return 0;
}
static int contains_local(LLVMValueRef value,FlowPath *path,unsigned depth) {
    return origin_flow(value,NULL,NULL,0,path,depth);
}
static int local_flow(LLVMValueRef value,LLVMValueRef unused,FlowPath *path,unsigned depth) {
    return origin_flow(value,NULL,NULL,0,path,depth);
}

static int permitted_pure_call(LLVMValueRef call) {
	LLVMValueRef callee = LLVMGetCalledValue(call);
	if (!LLVMIsAFunction(callee))
		return 0;
	if (LLVMGetStringAttributeAtIndex(callee, LLVMAttributeFunctionIndex,
									  "kawa.pure", 9))
		return 1;
	const char *name = LLVMGetValueName(callee);
	// Memory intrinsics may only write local storage. Arithmetic/debug/lifetime
	// intrinsics have no observable writes. Trap remains an allowed failure.
	if (strncmp(name, "llvm.memcpy.", 12) == 0 ||
		strncmp(name, "llvm.memmove.", 13) == 0 ||
		strncmp(name, "llvm.memset.", 12) == 0)
		return local_address(LLVMGetArgOperand(call, 0), 0);
	const char *prefixes[] = {
		"llvm.lifetime.", "llvm.dbg.",	"llvm.sadd.",  "llvm.uadd.",
		"llvm.ssub.",	  "llvm.usub.", "llvm.smul.",  "llvm.umul.",
		"llvm.fabs.",	  "llvm.sqrt.", "llvm.ctpop.", "llvm.ctlz.",
		"llvm.cttz.",	  "llvm.fshl.", "llvm.fshr.",  NULL};
	for (int i = 0; prefixes[i]; i++)
		if (strncmp(name, prefixes[i], strlen(prefixes[i])) == 0)
			return 1;
	return strcmp(name, "kawa_trap") == 0 ||
		   (LLVMCountBasicBlocks(callee) == 0 &&
			(strcmp(name, "memcmp") == 0 || strcmp(name, "strlen") == 0));
}

/* Infer retention from bodies, never from an unchecked extern annotation.
 * Returning a parameter is distinct from retaining it: an identity helper may
 * return a local view to its caller, but the caller still cannot export it. */
static int nonretaining_leaf(const char *name) {
	const char *known[]={"memcmp","memcpy","memmove","memset","strlen","strcmp",
		"strncmp","strchr","strrchr","printf","fprintf","sprintf","snprintf",
		"puts","putchar","fwrite","fread","read","write","abort","free",NULL};
	for (unsigned i=0; known[i]; ++i) if (!strcmp(name,known[i])) return 1;
	return !strncmp(name,"llvm.",5) || !strcmp(name,"kawa_trap");
}
static int parameter_flow(LLVMValueRef value, LLVMValueRef param, FlowPath *path, unsigned depth) {
    return origin_flow(value,param,NULL,0,path,depth);
}
static int ast_carries_address(KawaCompiler *c,Type *type,unsigned depth) {
    if (!type) return 0;
    if (depth>=64) return 1;
    type=kawa_resolve_type(c,type);
    switch (type->kind) {
    case TYPE_PTR: case TYPE_AMP: case TYPE_SLICE: case TYPE_OWNER:
    case TYPE_REF: case TYPE_ARENA: case TYPE_CHAN: case TYPE_HANDLE: return 1;
    case TYPE_ARRAY: case TYPE_SET: return ast_carries_address(c,type->inner,depth+1);
    case TYPE_STRUCT: {
        StructDef *def=find_struct_def_pub(c,get_llvm_type(c,type));
        for (int i=0; def && i<def->field_count; ++i)
            if (ast_carries_address(c,def->fields[i].ast_type,depth+1)) return 1;
        return 0;
    }
    case TYPE_ENUM: {
        ASTNode *en=find_enum_decl(c,type->name);
        for (EnumVariant *v=en ? en->data.enum_decl.variants : NULL; v; v=v->next)
            for (int i=0; i<v->payload_count; ++i)
                if (ast_carries_address(c,v->payload_types[i],depth+1)) return 1;
        return 0;
    }
    default: return 0;
    }
}
static int address_parameter(KawaCompiler *c,LLVMValueRef fn,unsigned index) {
    LLVMValueRef original=LLVMGetNamedFunction(c->module,LLVMGetValueName(fn));
    for (FunctionSignature *sig=c->function_signatures; sig; sig=sig->next)
        if (sig->function==original)
            return ast_carries_address(c,sig->parameters[index],0);
    return carries_pointer(LLVMTypeOf(LLVMGetParam(fn,index)));
}
typedef struct { LLVMValueRef fn; unsigned parameter; } CapturePath;
static int retention_trace(LLVMValueRef fn, unsigned parameter, int include_return,
		CapturePath *active, unsigned depth, char *trace, size_t capacity) {
	const char *name=LLVMIsAFunction(fn) ? LLVMGetValueName(fn) : "<indirect call>";
	if (!LLVMIsAFunction(fn) || depth==128) { snprintf(trace,capacity,"%s",name); return 1; }
	/* Compiler-owned runtime slots are borrowed for the call. Descriptor
	 * pointers copied out of a slot refer to process-lived metadata, never
	 * the caller's slot. Source functions cannot acquire this exception. */
	if (!LLVMGetStringAttributeAtIndex(fn,LLVMAttributeFunctionIndex,"kawa.source",11)) {
		const char *runtime[]={"__kawa_mem_alloc","__kawa_mem_drop","__kawa_mem_clone",
			"__kawa_mem_resize","__kawa_mem_capacity","__kawa_mem_address","__kawa_mem_slice",
			"__kawa_mem_pin","__kawa_mem_try_pin","__kawa_mem_unpin","__kawa_mem_arena",
            "__kawa_mem_arena_alloc","__kawa_mem_remove","__kawa_mem_store_owner",
            "__kawa_mem_take","__kawa_mem_write_address","__kawa_mem_replace","__kawa_mem_view",
            "__kawa_mem_value_drop","__kawa_mem_value_take","__kawa_mem_value_store",
            "__kawa_mem_value_clone","__kawa_mem_value_clear",NULL};
		for (unsigned i=0; runtime[i]; ++i) if (!strcmp(name,runtime[i])) return 0;
	}
	if (!LLVMCountBasicBlocks(fn) && nonretaining_leaf(name)) return 0;
	if (!LLVMCountBasicBlocks(fn) || parameter>=LLVMCountParams(fn)) {
		snprintf(trace,capacity,"%s (retention effect is unknown)",name); return 1;
	}
	for (unsigned i=0; i<depth; ++i)
		if (active[i].fn==fn && active[i].parameter==parameter) return 0;
	active[depth++]=(CapturePath){fn,parameter};
    LLVMValueRef param=LLVMGetParam(fn,parameter);
    FlowPath path[256];
	for (LLVMBasicBlockRef bb=LLVMGetFirstBasicBlock(fn); bb; bb=LLVMGetNextBasicBlock(bb)) {
		for (LLVMValueRef in=LLVMGetFirstInstruction(bb); in; in=LLVMGetNextInstruction(in)) {
			LLVMValueRef sink=NULL;
			if (LLVMIsAStoreInst(in) && !local_address(LLVMGetOperand(in,1),0)) sink=LLVMGetOperand(in,0);
			if (include_return && LLVMIsAReturnInst(in) && LLVMGetNumOperands(in)) sink=LLVMGetOperand(in,0);
			if (sink && parameter_flow(sink,param,path,0)) {
				snprintf(trace,capacity,"%s -> %s parameter %u",name,LLVMIsAReturnInst(in) ? "returns" : "retains",parameter+1); return 1;
			}
			if (memory_copy(in) && !local_address(LLVMGetArgOperand(in,0),0) &&
				contents_flow(LLVMGetArgOperand(in,1),param,fn,path,0,parameter_flow)) {
				snprintf(trace,capacity,"%s -> retains copied contents of parameter %u",name,parameter+1); return 1;
			}
			if (!LLVMIsACallInst(in) && !LLVMIsAInvokeInst(in)) continue;
			for (unsigned i=0; i<LLVMGetNumArgOperands(in); ++i) {
				if (!parameter_flow(LLVMGetArgOperand(in,i),param,path,0)) continue;
				char child[2048];
				if (retention_trace(LLVMGetCalledValue(in),i,0,active,depth,child,sizeof(child))) {
					snprintf(trace,capacity,"%s -> %s",name,child); return 1;
				}
			}
		}
	}
	return 0;
}

void kawa_verify_safety(KawaCompiler *c, LLVMTargetMachineRef machine) {
    kawa_verify_effects(c);
    LLVMModuleRef copy = LLVMCloneModule(c->module);
    /* Keep every source body available, including unused always-inline
     * accessors, so normalization cannot erase an invalid source contract. */
    for (LLVMValueRef fn=LLVMGetFirstFunction(copy); fn; fn=LLVMGetNextFunction(fn)) {
        if (LLVMGetStringAttributeAtIndex(fn,LLVMAttributeFunctionIndex,"kawa.source",11))
            LLVMSetLinkage(fn,LLVMExternalLinkage);
    }
	LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
	LLVMErrorRef err =
        LLVMRunPasses(copy, "always-inline,function(sroa,mem2reg)", machine, opts);
	LLVMDisposePassBuilderOptions(opts);
	if (err) {
		char *message = LLVMGetErrorMessage(err);
		kdiag_error_at(KAWA_E_SEMANTIC, c->source_filename, NULL, 0,
					   "safety analysis failed: %s", message);
		LLVMDisposeErrorMessage(message);
		exit(1);
	}
	for (LLVMValueRef fn = LLVMGetFirstFunction(copy); fn;
		 fn = LLVMGetNextFunction(fn)) {
		LLVMAttributeRef source = LLVMGetStringAttributeAtIndex(
			fn, LLVMAttributeFunctionIndex, "kawa.source", 11);
		if (!source)
			continue;
		unsigned len;
		const char *line_text = LLVMGetStringAttributeValue(source, &len);
		char buf[32] = {0};
		memcpy(buf, line_text, len < 31 ? len : 31);
		int line = atoi(buf);
		int pure = LLVMGetStringAttributeAtIndex(fn, LLVMAttributeFunctionIndex,
												 "kawa.pure", 9) != NULL;
		if (LLVMGetStringAttributeAtIndex(fn,LLVMAttributeFunctionIndex,"kawa.nocapture",14)) {
			for (unsigned i=0; i<LLVMCountParams(fn); ++i) {
                if (!address_parameter(c,fn,i)) continue;
				CapturePath active[128]; char trace[2048];
				if (retention_trace(fn,i,1,active,0,trace,sizeof(trace))) {
					kdiag_error_at(KAWA_E_EFFECT,c->source_filename,NULL,line,"nocapture contract failed: %s",trace); exit(1);
				}
			}
		}
		for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb;
			 bb = LLVMGetNextBasicBlock(bb)) {
			for (LLVMValueRef in = LLVMGetFirstInstruction(bb); in;
				 in = LLVMGetNextInstruction(in)) {
				if (pure &&
					((LLVMIsAStoreInst(in) &&
					  !local_address(LLVMGetOperand(in, 1), 0)) ||
					 (LLVMIsACallInst(in) && !permitted_pure_call(in)) ||
					 LLVMIsAAtomicRMWInst(in) || LLVMIsAAtomicCmpXchgInst(in) ||
					 LLVMIsAFenceInst(in) ||
					 (LLVMIsALoadInst(in) && LLVMGetVolatile(in)))) {
					kdiag_error_at(KAWA_E_SEMANTIC, c->source_filename, NULL,
								   line,
								   "pure function `%s` has an external write "
								   "or an unverified call effect",
								   LLVMGetValueName(fn));
					exit(1);
				}
				LLVMValueRef value = NULL;
				if (LLVMIsAReturnInst(in) && LLVMGetNumOperands(in))
					value = LLVMGetOperand(in, 0);
				if (LLVMIsAStoreInst(in) &&
					!local_address(LLVMGetOperand(in, 1), 0))
					value = LLVMGetOperand(in, 0);
                FlowPath path[256];
				if (memory_copy(in) && !local_address(LLVMGetArgOperand(in,0),0) &&
					contents_flow(LLVMGetArgOperand(in,1),NULL,fn,path,0,local_flow)) {
					kdiag_error_at(KAWA_E_SEMANTIC,c->source_filename,NULL,line,
						"local address may escape through copied memory in function %s",LLVMGetValueName(fn)); exit(1);
				}
				if (LLVMIsACallInst(in) || LLVMIsAInvokeInst(in)) {
					for (unsigned i=0; i<LLVMGetNumArgOperands(in); ++i) {
						LLVMValueRef arg=LLVMGetArgOperand(in,i);
						if (!contains_local(arg,path,0)) continue;
						CapturePath active[128]; char trace[2048];
						if (retention_trace(LLVMGetCalledValue(in),i,0,active,0,trace,sizeof(trace))) {
							kdiag_note("the address originates in this function's stack; callees are checked transitively");
							kdiag_error_at(KAWA_E_SEMANTIC,c->source_filename,NULL,line,"local address may escape through %s",trace); exit(1);
						}
					}
				}
				if (value && contains_local(value, path, 0)) {
					kdiag_error_at(KAWA_E_SEMANTIC, c->source_filename, NULL,
								   line,
								   "local address may escape its lifetime in "
								   "function `%s`",
								   LLVMGetValueName(fn));
					exit(1);
				}
			}
		}
	}
	LLVMDisposeModule(copy);
}
