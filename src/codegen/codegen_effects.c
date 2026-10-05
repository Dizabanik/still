#include "codegen_internal.h"

/* Check before optimization. Recursive calls form SCCs; an active-path revisit
 * adds no new effect, but every other outgoing edge is still examined. */
static int noalloc_leaf(const char *name) {
    const char *safe[] = {"free", "memcmp", "memcpy", "memmove", "memset", "strlen",
        "kawa_trap", "__kawa_mem_address", "__kawa_mem_slice", "__kawa_mem_pin",
        "__kawa_mem_try_pin", "__kawa_mem_unpin", "__kawa_mem_drop", "__kawa_mem_remove",
        "__kawa_mem_metric", "__kawa_mem_budget", "__kawa_mem_capacity",
        "__kawa_mem_store_owner", "__kawa_mem_take", "__kawa_mem_write_address",
        "__kawa_mem_replace", "__kawa_mem_view", NULL};
    for (unsigned i=0; safe[i]; ++i) if (!strcmp(name,safe[i])) return 1;
    // Memory/arithmetic/lifetime/debug intrinsics do not acquire heap storage.
    return !strncmp(name,"llvm.",5) && strncmp(name,"llvm.coro.",10);
}
static int allocation_trace(LLVMValueRef fn, LLVMValueRef *path, unsigned depth,
                             char *trace, size_t capacity) {
    const char *name = LLVMIsAFunction(fn) ? LLVMGetValueName(fn) : "<indirect call>";
    if (depth == 128 || !LLVMIsAFunction(fn)) {
        snprintf(trace,capacity,"%s",name); return 1;
    }
    for (unsigned i=0;i<depth;++i) if (path[i]==fn) return 0;
    if (!LLVMGetStringAttributeAtIndex(fn,LLVMAttributeFunctionIndex,"kawa.source",11) &&
        noalloc_leaf(name)) return 0;
    if (!LLVMCountBasicBlocks(fn)) {
        snprintf(trace,capacity,"%s (allocation effect is unknown)",name); return 1;
    }
    path[depth++] = fn;
    for (LLVMBasicBlockRef bb=LLVMGetFirstBasicBlock(fn);bb;bb=LLVMGetNextBasicBlock(bb)) {
        for (LLVMValueRef in=LLVMGetFirstInstruction(bb);in;in=LLVMGetNextInstruction(in)) {
            if (!LLVMIsACallInst(in) && !LLVMIsAInvokeInst(in)) continue;
            char child[2048];
            if (allocation_trace(LLVMGetCalledValue(in),path,depth,child,sizeof(child))) {
                snprintf(trace,capacity,"%s -> %s",name,child); return 1;
            }
        }
    }
    return 0;
}
void kawa_verify_effects(KawaCompiler *c) {
    for (LLVMValueRef fn=LLVMGetFirstFunction(c->module);fn;fn=LLVMGetNextFunction(fn)) {
        if (!LLVMGetStringAttributeAtIndex(fn,LLVMAttributeFunctionIndex,"kawa.noalloc",12)) continue;
        LLVMValueRef path[128]; char trace[2048];
        if (allocation_trace(fn,path,0,trace,sizeof(trace))) {
            LLVMAttributeRef source=LLVMGetStringAttributeAtIndex(fn,LLVMAttributeFunctionIndex,"kawa.source",11);
            unsigned size=0;
            const char *value=source ? LLVMGetStringAttributeValue(source,&size) : NULL;
            char line[32]={0};
            if (value) memcpy(line,value,size<31 ? size : 31);
            kdiag_error_at(KAWA_E_EFFECT,c->source_filename,NULL,atoi(line),
                          "noalloc contract failed: %s",trace);
            exit(1);
        }
    }
}
