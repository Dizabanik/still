#include "codegen_internal.h"
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

/* Static IR facts, never estimates of executed operations. A source tag may
 * disappear during inlining/merging without eliminating the underlying check.
 * Preserve that distinction in the report and retain both function snapshots. */
typedef struct {
    unsigned instructions,branches,calls;
    const char *condition;
} SiteFacts;
typedef struct StillOptimizationSite {
    unsigned id;
    const char *function,*kind,*detail,*omission,*file;
    int line,column,length;
    SiteFacts before,after;
    struct StillOptimizationSite *next;
} StillOptimizationSite;
typedef struct CallFacts {
    const char *callee;
    unsigned count;
    struct CallFacts *next;
} CallFacts;
typedef struct FunctionFacts {
    const char *name,*role;
    unsigned blocks,instructions,conditional_branches;
    CallFacts *calls;
    struct FunctionFacts *next;
} FunctionFacts;
struct StillOptimizationSnapshot { FunctionFacts *functions; };

LLVMMetadataRef still_report_site(StillCompiler *c,ASTNode *node,const char *kind,
                                 const char *detail,const char *omission) {
    if (!c->optimization_report) return NULL;
    StillOptimizationSite *site=arena_alloc(c->arena,sizeof(*site));
    site->id=++c->optimization_site_count;
    site->function=arena_strdup(c->arena,c->current_func ? LLVMGetValueName(c->current_func) : "<global>");
    site->kind=kind; site->detail=detail; site->omission=omission;
    site->file=c->source_filename;
    int expanded=node && node->line>0 ? node->line : c->source_line;
    if (node && node->span_length>0) {
        site->column=node->column;
        site->length=node->span_length;
    }
    still_diag_location(expanded,&site->file,&site->line);
    site->file=arena_strdup(c->arena,site->file ? site->file : "<generated>");
    site->next=c->optimization_sites; c->optimization_sites=site;
    LLVMMetadataRef id=LLVMValueAsMetadata(
        LLVMConstInt(LLVMInt64TypeInContext(c->context),site->id,0));
    return LLVMMDNodeInContext2(c->context,&id,1);
}
void still_report_attach(StillCompiler *c,LLVMValueRef instruction,LLVMMetadataRef site) {
    if (!site || !instruction) return;
    unsigned kind=LLVMGetMDKindIDInContext(c->context,"wky.site", sizeof("wky.site") - 1);
    LLVMSetMetadata(instruction,kind,LLVMMetadataAsValue(c->context,site));
}
static StillOptimizationSite **site_index(StillCompiler *c) {
    StillOptimizationSite **index=arena_alloc(c->arena,
        (c->optimization_site_count+1)*sizeof(*index));
    for (StillOptimizationSite *s=c->optimization_sites; s; s=s->next) index[s->id]=s;
    return index;
}
static void observe(StillCompiler *c,LLVMValueRef instruction,
                    StillOptimizationSite **index,int after) {
    LLVMValueRef node=LLVMGetMetadata(instruction,
        LLVMGetMDKindIDInContext(c->context,"wky.site", sizeof("wky.site") - 1));
    if (!node || LLVMGetMDNodeNumOperands(node)!=1) return;
    LLVMValueRef operand=NULL;
    LLVMGetMDNodeOperands(node,&operand);
    if (!LLVMIsAConstantInt(operand)) return;
    uint64_t id=LLVMConstIntGetZExtValue(operand);
    if (!id || id>c->optimization_site_count) return;
    SiteFacts *facts=after ? &index[id]->after : &index[id]->before;
    ++facts->instructions;
    if (LLVMIsACallInst(instruction) || LLVMIsAInvokeInst(instruction)) ++facts->calls;
    if (LLVMIsABranchInst(instruction) && LLVMIsConditional(instruction)) {
        ++facts->branches;
        if (!facts->condition) {
            char *condition=LLVMPrintValueToString(LLVMGetCondition(instruction));
            facts->condition=arena_strdup(c->arena,condition);
            LLVMDisposeMessage(condition);
        }
    }
}
static void add_call(StillCompiler *c,FunctionFacts *facts,LLVMValueRef instruction) {
    LLVMValueRef callee=LLVMGetCalledValue(instruction);
    const char *name=LLVMIsAFunction(callee) ? LLVMGetValueName(callee) : "<indirect>";
    CallFacts **tail=&facts->calls;
    while (*tail) {
        if (!strcmp((*tail)->callee,name)) { ++(*tail)->count; return; }
        tail=&(*tail)->next;
    }
    CallFacts *call=arena_alloc(c->arena,sizeof(*call));
    call->callee=arena_strdup(c->arena,name); call->count=1; *tail=call;
}
static StillOptimizationSnapshot *snapshot(StillCompiler *c,int after) {
    StillOptimizationSnapshot *result=arena_alloc(c->arena,sizeof(*result));
    FunctionFacts **tail=&result->functions;
    StillOptimizationSite **index=site_index(c);
    for (LLVMValueRef fn=LLVMGetFirstFunction(c->module); fn; fn=LLVMGetNextFunction(fn)) {
        if (!LLVMCountBasicBlocks(fn)) continue;
        FunctionFacts *facts=arena_alloc(c->arena,sizeof(*facts));
        facts->name=arena_strdup(c->arena,LLVMGetValueName(fn));
        facts->role=LLVMGetStringAttributeAtIndex(fn,LLVMAttributeFunctionIndex,"wky.source", sizeof("wky.source") - 1) ?
            "source" : LLVMGetStringAttributeAtIndex(fn,LLVMAttributeFunctionIndex,"target-cpu",10) ?
            "embedded_runtime" : "generated";
        *tail=facts; tail=&facts->next;
        for (LLVMBasicBlockRef bb=LLVMGetFirstBasicBlock(fn); bb; bb=LLVMGetNextBasicBlock(bb)) {
            ++facts->blocks;
            for (LLVMValueRef in=LLVMGetFirstInstruction(bb); in; in=LLVMGetNextInstruction(in)) {
                ++facts->instructions;
                if (LLVMIsABranchInst(in) && LLVMIsConditional(in)) ++facts->conditional_branches;
                if (LLVMIsACallInst(in) || LLVMIsAInvokeInst(in)) add_call(c,facts,in);
                observe(c,in,index,after);
            }
        }
    }
    return result;
}
StillOptimizationSnapshot *still_report_snapshot(StillCompiler *c) {
    return c->optimization_report ? snapshot(c,0) : NULL;
}
static void write_function(FILE *out,FunctionFacts *facts) {
    if (!facts) { fputs("null",out); return; }
    fprintf(out,"{\"blocks\":%u,\"instructions\":%u,\"conditional_branches\":%u,\"calls\":[",
        facts->blocks,facts->instructions,facts->conditional_branches);
    for (CallFacts *call=facts->calls; call; call=call->next) {
        if (call!=facts->calls) fputc(',',out);
        fputs("{\"callee\":",out); still_diag_json_string(out,call->callee);
        fprintf(out,",\"count\":%u}",call->count);
    }
    fputs("]}",out);
}
static void write_site_facts(FILE *out,SiteFacts *facts) {
    fprintf(out,"{\"tagged_instructions\":%u,\"conditional_branches\":%u,\"calls\":%u,\"condition_ir\":",
        facts->instructions,facts->branches,facts->calls);
    if (facts->condition) still_diag_json_string(out,facts->condition);
    else fputs("null",out);
    fputc('}',out);
}
static FunctionFacts *find_function(StillOptimizationSnapshot *snapshot,const char *name) {
    for (FunctionFacts *fn=snapshot->functions; fn; fn=fn->next)
        if (!strcmp(fn->name,name)) return fn;
    return NULL;
}
static void write_function_pair(FILE *out,FunctionFacts *before,FunctionFacts *after) {
    fputs("{\"name\":",out); still_diag_json_string(out,before ? before->name : after->name);
    fputs(",\"role\":",out); still_diag_json_string(out,before ? before->role : after->role);
    fputs(",\"before\":",out); write_function(out,before);
    fputs(",\"after\":",out); write_function(out,after); fputc('}',out);
}
static const char *path_key(StillCompiler *c,const char *path) {
    char canonical[PATH_MAX];
    if (realpath(path,canonical)) return arena_strdup(c->arena,canonical);
    char *copy=arena_strdup(c->arena,path), *slash=strrchr(copy,'/');
    const char *directory=".", *base=copy;
    if (slash) {
        base=slash+1; *slash='\0';
        directory=*copy ? copy : "/";
    }
    if (!realpath(directory,canonical)) return NULL;
    size_t length=strlen(canonical)+strlen(base)+2;
    char *key=arena_alloc(c->arena,length);
    snprintf(key,length,"%s/%s",canonical,base);
    return key;
}
static void check_collision(StillCompiler *c,const char *report_key,
                             const struct stat *report_stat,int exists,const char *path) {
    if (!path) return;
    const char *key=path_key(c,path);
    struct stat candidate;
    int collision=(key && report_key && !strcmp(key,report_key)) ||
        (exists && !stat(path,&candidate) && report_stat->st_dev==candidate.st_dev &&
         report_stat->st_ino==candidate.st_ino);
    if (collision) {
        still_diag_error_at(STILL_E_SEMANTIC,c->source_filename,NULL,0,
            "optimization report conflicts with input or build artifact '%s'",path); exit(1);
    }
}
void still_report_write(StillCompiler *c,StillOptimizationSnapshot *before,const char *pipeline) {
    if (!c->optimization_report) return;
    const char *key=path_key(c,c->optimization_report);
    struct stat report_stat;
    int exists=!stat(c->optimization_report,&report_stat);
    check_collision(c,key,&report_stat,exists,c->source_filename);
    for (ASTNode *node=c->program_root; node; node=node->next)
        check_collision(c,key,&report_stat,exists,node->module_name);
    const char *artifacts[]={"output.bc","output.ll","output.o",c->executable_path};
    for (unsigned i=0; i<sizeof(artifacts)/sizeof(*artifacts); ++i)
        check_collision(c,key,&report_stat,exists,artifacts[i]);
    StillOptimizationSnapshot *after=snapshot(c,1);
    size_t temporary_size=strlen(c->optimization_report)+8;
    char *temporary=arena_alloc(c->arena,temporary_size);
    snprintf(temporary,temporary_size,"%s.XXXXXX",c->optimization_report);
    int fd=mkstemp(temporary);
    FILE *out=fd<0 ? NULL : fdopen(fd,"w");
    if (!out) {
        int error=errno;
        if (fd>=0) { close(fd); unlink(temporary); }
        still_diag_error_at(STILL_E_SEMANTIC,c->source_filename,NULL,0,
            "cannot write optimization report: %s",strerror(error)); exit(1);
    }
    fprintf(out,"{\n  \"schema_version\":1,\n  \"optimization_level\":%d,\n  \"target\":",c->opt_level);
    still_diag_json_string(out,LLVMGetTarget(c->module));
    fputs(",\n  \"pipeline\":",out); still_diag_json_string(out,pipeline);
    fprintf(out,",\n  \"memory_metrics_enabled\":%s,\n  \"runtime_counts\":null,\n  ",
        c->memory_metrics ? "true" : "false");
    fputs("\"measurement\":\"Static IR instruction sites, including embedded runtime bodies; not executed-operation counts.\",",out);
    fputs("\n  \"limitations\":\"Source tags cover managed operations and guards emitted through common helpers; legacy guards may lack tags. Tags can be lost during inlining, merging, cloning or removal. An unobserved tag or missing direct call is not proof that a check or allocation was eliminated. Function facts are exact for the emitted IR.\",\n  \"functions\":[\n    ",out);
    int first=1;
    for (FunctionFacts *fn=before->functions; fn; fn=fn->next) {
        if (!first) fputs(",\n    ",out); first=0;
        write_function_pair(out,fn,find_function(after,fn->name));
    }
    for (FunctionFacts *fn=after->functions; fn; fn=fn->next) {
        if (find_function(before,fn->name)) continue;
        if (!first) fputs(",\n    ",out); first=0;
        write_function_pair(out,NULL,fn);
    }
    fputs("\n  ],\n  \"sites\":[\n    ",out);
    StillOptimizationSite **index=site_index(c);
    for (unsigned id=1; id<=c->optimization_site_count; ++id) {
        if (id!=1) fputs(",\n    ",out);
        StillOptimizationSite *s=index[id];
        fprintf(out,"{\"id\":%u,\"function\":",id); still_diag_json_string(out,s->function);
        fputs(",\"kind\":",out); still_diag_json_string(out,s->kind);
        fputs(",\"detail\":",out); still_diag_json_string(out,s->detail);
        fputs(",\"location\":{\"file\":",out); still_diag_json_string(out,s->file);
        fprintf(out,",\"line\":%d,\"column\":",s->line);
        if (s->column) fprintf(out,"%d,\"length\":%d",s->column,s->length);
        else fputs("null,\"length\":null",out);
        fputs(",\"column_unit\":\"byte\"},\"before\":",out); write_site_facts(out,&s->before);
        fputs(",\"after\":",out); write_site_facts(out,&s->after);
        const char *status=s->omission ? "omitted_during_lowering" :
            s->after.instructions ? "observed_in_final_ir" : "unobserved_in_final_ir";
        const char *reason=s->omission ? s->omission : s->after.branches ?
            "A conditional check remains. condition_ir shows the predicate LLVM retained." : s->after.calls ?
            "At least one tagged operation remains as a call in final IR." :
            s->after.instructions ? "At least one tagged instruction remains without an associated branch or call." :
            "No tagged instruction remains. Inlining, merging, removal or metadata loss can cause this; no runtime elimination is inferred.";
        fputs(",\"status\":",out); still_diag_json_string(out,status);
        fputs(",\"reason\":",out); still_diag_json_string(out,reason); fputc('}',out);
    }
    fputs("\n  ]\n}\n",out);
    int failed=ferror(out);
    if (fclose(out)) failed=1;
    if (failed || rename(temporary,c->optimization_report)) {
        unlink(temporary);
        still_diag_error_at(STILL_E_SEMANTIC,c->source_filename,NULL,0,"writing optimization report failed"); exit(1);
    }
}
