#include "semantics_common.h"
typedef struct { uint64_t tag; union {WkyRef data; uint64_t words[4];} payload; } Value;
_Static_assert(sizeof(Value)==40,"tagged owner ABI");
static const WkyOwnedField fields[]={{offsetof(Value,payload),1,sizeof(WkyRef),NULL,0,1}};
static const WkyValueLayout layout={sizeof(Value),1,fields};
static uint64_t consume(Value value,int increment) {
 uint64_t out;
 if(value.tag) out=value.payload.words[2];
 else { WkyRef buffer=value.payload.data; value.payload.data=(WkyRef){0};
  if(increment) {uint64_t *p=word(buffer); *p+=1;}
  out=*word(buffer); __wky_mem_drop(&buffer);
 }
 __wky_mem_value_drop(&value,&layout); return out;
}
int main(int argc,char **argv) {
 if(argc!=4) return 2;
 uint64_t rounds=input(argv[1]),seed=input(argv[2]),period=input(argv[3]);
 if(rounds>100000000 || seed>65535 || !period || period>256) return 2;
 uint64_t checksum=0,counted=0;
 for(uint64_t i=0;i<rounds;++i) {
  uint64_t x=seed+i; Value source={0},copy={0},next={0};
  if(x%period==0) { next.tag=1; next.payload.words[0]=next.payload.words[1]=next.payload.words[3]=UINT64_MAX; next.payload.words[2]=x; }
  else {if(!__wky_mem_alloc(&next.payload.data,1,8)) abort(); *word(next.payload.data)=x; ++counted;}
  __wky_mem_value_store(&source,&next,&layout,NULL);
  if(!__wky_mem_value_clone(&copy,&source,&layout,NULL)) abort();
  Value moved;
  __wky_mem_value_take(&moved,&copy,&layout,NULL); uint64_t newer=consume(moved,1);
  __wky_mem_value_take(&moved,&source,&layout,NULL); uint64_t original=consume(moved,0);
  checksum+=original+3*newer;
  __wky_mem_value_drop(&copy,&layout); __wky_mem_value_drop(&source,&layout);
 }
 report(checksum,counted);
}
