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

// Existential provenance: any incoming local address makes a returned view
// unsafe. A path stack terminates SSA cycles without losing the other inputs.
static int contains_local(LLVMValueRef v, LLVMValueRef *path, unsigned depth) {
	if (LLVMIsAAllocaInst(v))
		return 1;
	if (!LLVMIsAInstruction(v))
		return 0;
	if (depth == 256)
		return 1; // fail closed on unusually deep pointer graphs
	for (unsigned i = 0; i < depth; i++)
		if (path[i] == v)
			return 0;
	path[depth++] = v;
	if (LLVMIsALoadInst(v)) {
		// Residual address-taken aggregate storage (not promotable by SROA).
		LLVMValueRef root = storage_root(LLVMGetOperand(v, 0));
		if (!LLVMIsAAllocaInst(root))
			return 0;
		LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInstructionParent(v));
		for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb;
			 bb = LLVMGetNextBasicBlock(bb))
			for (LLVMValueRef st = LLVMGetFirstInstruction(bb); st;
				 st = LLVMGetNextInstruction(st))
				if (LLVMIsAStoreInst(st) &&
					storage_root(LLVMGetOperand(st, 1)) == root &&
					contains_local(LLVMGetOperand(st, 0), path, depth))
					return 1;
		return 0;
	}
	if (LLVMIsACallInst(v)) {
		// A pointer-returning call may return one of its pointer arguments.
		// Scalar results (e.g. strlen) do not carry an address.
		if (!carries_pointer(LLVMTypeOf(v)))
			return 0;
		for (unsigned i = 0; i < LLVMGetNumArgOperands(v); i++)
			if (contains_local(LLVMGetArgOperand(v, i), path, depth))
				return 1;
		return 0;
	}
	if (LLVMIsAExtractValueInst(v) && !carries_pointer(LLVMTypeOf(v)))
		return 0;
	for (int i = 0; i < LLVMGetNumOperands(v); i++)
		if (contains_local(LLVMGetOperand(v, i), path, depth))
			return 1;
	return 0;
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

void kawa_verify_safety(KawaCompiler *c, LLVMTargetMachineRef machine) {
	LLVMModuleRef copy = LLVMCloneModule(c->module);
	LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
	LLVMErrorRef err =
		LLVMRunPasses(copy, "function(sroa,mem2reg)", machine, opts);
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
				LLVMValueRef path[256];
				if (value && carries_pointer(LLVMTypeOf(value)) &&
					contains_local(value, path, 0)) {
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
