/* Adapter for the original palloc/pfree interface, backed by the native UEFI
 * ownership allocator. No second pool, physical-address truncation or guessed
 * top-of-RAM. Headers validate exact ownership and retain the real reserved size. */
#include "scos.h"
#define HEAP_MAGIC UINT64_C(0x53434f5348454150)
struct allocation {u64 magic;size_t requested,pages;uintptr_t self,owner;struct allocation *next,*prev;u64 pad;};
static struct allocation *live;
static uintptr_t current_owner;
_Static_assert(sizeof(struct allocation)==64,"16-byte aligned payload");
static u64 total_kb;
static u32 allocations,releases;
void mm_init(void){total_kb=(memory_free_pages()+memory_reserved_pages())*4;}
void *palloc_owned(size_t bytes,uintptr_t owner){
    if(bytes>SIZE_MAX-sizeof(struct allocation)-4095)return NULL;
    size_t pages=(bytes+sizeof(struct allocation)+4095)/4096;
    uintptr_t base=pages_allocate(pages);
    if(!base)return NULL;
    struct allocation *h=(void*)base;
    *h=(struct allocation){.magic=HEAP_MAGIC,.requested=bytes,.pages=pages,.self=base,.owner=owner,.next=live};
    if(live)live->prev=h;
    live=h;
    allocations++;
    return h+1;
}
static struct allocation *header(const void *p){
    struct allocation *h=(struct allocation*)p-1;
    if(h->magic!=HEAP_MAGIC||h->self!=(uintptr_t)h||((uintptr_t)h&4095))panic("invalid desktop allocation");
    return h;
}
void pfree(void *p,size_t bytes){
    if(!p)return;
    struct allocation *h=header(p);
    if(bytes!=h->requested)panic("desktop allocation size mismatch");
    size_t pages=h->pages;h->magic=0;releases++;
    if(h->prev)h->prev->next=h->next;else live=h->next;
    if(h->next)h->next->prev=h->prev;
    pages_release((uintptr_t)h,pages);
}
size_t allocation_bytes(const void *p){return p?header(p)->pages*4096:0;}
u64 mm_total_kb(void){return total_kb;}
u64 mm_free_kb(void){return memory_free_pages()*4;}
void mm_stats(u64 *total,u64 *free){*total=mm_total_kb();*free=mm_free_kb();}
void mm_ops(u32 *a,u32 *f){if(a)*a=allocations;if(f)*f=releases;}

uintptr_t heap_set_owner(uintptr_t owner){uintptr_t old=current_owner;current_owner=owner;return old;}
void *palloc(size_t bytes){return palloc_owned(bytes,current_owner);}
u64 heap_owner_bytes(uintptr_t owner){u64 bytes=0;for(struct allocation *h=live;h;h=h->next)if(h->owner==owner)bytes+=h->pages*4096;return bytes;}
