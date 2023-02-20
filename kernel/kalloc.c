// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

//for increasing and decreasing the ref_count we must
//always acquire it first and then release it

struct{
  uint reference_counter[PHYSTOP/PGSIZE]; //max number of pages
  struct spinlock lock;
}ref_count;

void
ref_counter_incr(uint64 pa){
  ref_count.reference_counter[pa/PGSIZE] += 1;
  return;
}

//returns -1 if the counter becomes 0
//else returns 0 if the counter is still >= 1
int
ref_counter_decr(uint64 pa){
  //each pa differs by PGSIZE (line:86)
  //so we can hash each pa in our table by dividing with PGSIZE
  if(ref_count.reference_counter[pa/PGSIZE] > 1){
    ref_count.reference_counter[pa/PGSIZE] -= 1;
    return 0;
  }
  else{
    ref_count.reference_counter[pa/PGSIZE] = 0;
    return -1;
  }
}

uint
ref_counter_get(uint64 pa){
  return ref_count.reference_counter[pa/PGSIZE];
}

void 
ref_counter_set(uint64 pa, uint64 n){
  ref_count.reference_counter[pa/PGSIZE] = n;
}

void
ref_acquire(){
  acquire(&ref_count.lock);
}

void
ref_release(){
  release(&ref_count.lock);
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
  initlock(&ref_count.lock,"ref_count");  //we init the lock of ref_Count
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  //if the ref_count is greater than 1 we should not free the page
  //but instead just decrease the ref counter by 1
  ref_acquire();
  if(ref_counter_decr((uint64)pa) == 0){
    ref_release();
    return;
  }
  ref_release();

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk

  //when we create a new page
  //we initialize the ref counter of the page with 1
  if(r){
    //we check if the current cpu is holding the lock
    //and we set the ref counter accordingly
    if(holding(&ref_count.lock)){
      ref_counter_set((uint64)r,1);
    }
    //if it doesnt hold it we must acquire it first and then release it
    else{
      ref_acquire();
      ref_counter_set((uint64)r,1);
      ref_release();
    }
  }
  return (void*)r;
}
