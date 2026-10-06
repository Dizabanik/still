/* Equal-policy comparison: the same checked descriptor runtime is inlined. */
#ifndef WKY_BENCH_SEMANTICS_COMMON_H
#define WKY_BENCH_SEMANTICS_COMMON_H
#include "../../src/runtime/wky_memory.c"
#include <inttypes.h>
#include <errno.h>
static uint64_t input(const char *text) {
 char *end; errno=0; uint64_t n=strtoull(text,&end,10);
 if(errno || end==text || *end || *text=='-') exit(2);
 return n;
}
static void report(uint64_t checksum,uint64_t counted) {
 printf("%"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64"\n",
 checksum,counted,__wky_mem_metric(0),__wky_mem_metric(1),__wky_mem_metric(2),
 __wky_mem_metric(3),__wky_mem_metric(5),__wky_mem_metric(7),__wky_mem_metric(4));
}
static uint64_t *word(WkyRef value) {
 return __wky_mem_address(value.descriptor,value.generation,value.offset,value.extent,0,8);
}
#endif
