#include "semantics_common.h"
typedef struct {WkyRef buffer; uint64_t cap,head,count,mask; _Bool closed;} Channel;
_Static_assert(sizeof(Channel)==72,"channel ABI");
static WkyRef *entry(Channel *pipe,uint64_t index) {
 return __wky_mem_address(pipe->buffer.descriptor,pipe->buffer.generation,0,pipe->buffer.extent,index,sizeof(WkyRef));
}
static void send(Channel *pipe,WkyRef value) {
 if(!pipe->buffer.descriptor || pipe->closed || pipe->count>=pipe->cap) abort();
 __wky_mem_store_owner(entry(pipe,(pipe->head+pipe->count)&pipe->mask),&value,&pipe->buffer); ++pipe->count;
 __wky_mem_drop(&value);
}
static WkyRef receive(Channel *pipe) {
 if(!pipe->buffer.descriptor || !pipe->count) abort();
 WkyRef value; __wky_mem_take(&value,entry(pipe,pipe->head&pipe->mask),&pipe->buffer);
 ++pipe->head; --pipe->count; return value;
}
int main(int argc,char **argv) {
 if(argc!=4) return 2;
 uint64_t rounds=input(argv[1]),seed=input(argv[2]),capacity=input(argv[3]);
 if(rounds>100000000 || seed>65535 || !capacity || capacity>256) return 2;
 uint64_t checksum=0,counted=0,slots=1; while(slots<capacity) slots*=2;
 Channel pipe={.cap=capacity,.mask=slots-1}; if(!__wky_mem_alloc(&pipe.buffer,slots,sizeof(WkyRef))) abort();
 for(uint64_t base=0;base<rounds;base+=capacity) {
  uint64_t batch=rounds-base<capacity ? rounds-base : capacity;
  for(uint64_t i=0;i<batch;++i) {WkyRef item; if(!__wky_mem_alloc(&item,1,8)) abort();
   *word(item)=seed+base+i; WkyRef moved=item; item=(WkyRef){0}; send(&pipe,moved); __wky_mem_drop(&item);}
  for(uint64_t i=0;i<batch;++i) {WkyRef item=receive(&pipe); checksum+=*word(item)*((base+i)%97+7); ++counted; __wky_mem_drop(&item);}
 }
 __wky_mem_drop(&pipe.buffer); report(checksum,counted);
}
