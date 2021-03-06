/* ----------------------------------------------------------------------------
Copyright (c) 2019, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

/* ----------------------------------------------------------------------------
This implements a layer between the raw OS memory (VirtualAlloc/mmap/sbrk/..)
and the segment and huge object allocation by mimalloc. In contrast to the
rest of mimalloc, this uses thread-shared "regions" that are accessed using
atomic operations. We need this layer because of:
1. on `sbrk` like systems (like WebAssembly) we need our own memory maps in order
   to reuse memory 
2. It turns out that for large objects, between 1MiB and 32MiB (?), the cost of
   an OS allocation/free is still too expensive relative to the accesses in that
   object :-( (`mallloc-large` tests this). This means we need a cheaper way to 
   reuse memory.
3. This layer can help with a NUMA aware allocation in the future.

Possible issues:
- (2) can potentially be addressed too with a small cache per thread which is much 
  simpler. Generally though that requires shrinking of huge pages, and may overuse
  memory per thread. (and is not compatible with `sbrk`). 
- Since the current regions are per-process, we need atomic operations to 
  claim blocks which may be contended
- In the worst case, we need to search the whole region map (16KiB for 256GiB)
  linearly. At what point will direct OS calls be faster? Is there a way to 
  do this better without adding too much complexity?
-----------------------------------------------------------------------------*/
#include "mimalloc.h"
#include "mimalloc-internal.h"
#include "mimalloc-atomic.h"

#include <string.h>  // memset

// Internal OS interface
size_t  _mi_os_large_page_size();
bool  _mi_os_protect(void* addr, size_t size);
bool  _mi_os_unprotect(void* addr, size_t size);
bool  _mi_os_commit(void* p, size_t size, mi_stats_t* stats);
bool  _mi_os_decommit(void* p, size_t size, mi_stats_t* stats);
bool  _mi_os_reset(void* p, size_t size, mi_stats_t* stats);
bool  _mi_os_unreset(void* p, size_t size, mi_stats_t* stats);
void* _mi_os_alloc_aligned(size_t size, size_t alignment, bool commit, mi_os_tld_t* tld);


// Constants
#if (MI_INTPTR_SIZE==8)
#define MI_HEAP_REGION_MAX_SIZE    (256 * (1ULL << 30))  // 256GiB => 16KiB for the region map
#elif (MI_INTPTR_SIZE==4)
#define MI_HEAP_REGION_MAX_SIZE    (3 * (1UL << 30))    // 3GiB => 196 bytes for the region map
#else
#error "define the maximum heap space allowed for regions on this platform"
#endif

#define MI_SEGMENT_ALIGN          MI_SEGMENT_SIZE

#define MI_REGION_MAP_BITS        (MI_INTPTR_SIZE * 8)
#define MI_REGION_SIZE            (MI_SEGMENT_SIZE * MI_REGION_MAP_BITS)
#define MI_REGION_MAX_ALLOC_SIZE  ((MI_REGION_MAP_BITS/4)*MI_SEGMENT_SIZE)  // 64MiB
#define MI_REGION_MAX             (MI_HEAP_REGION_MAX_SIZE / MI_REGION_SIZE)
#define MI_REGION_MAP_FULL        UINTPTR_MAX


// A region owns a chunk of REGION_SIZE (256MiB) (virtual) memory with
// a bit map with one bit per MI_SEGMENT_SIZE (4MiB) block.
typedef struct mem_region_s {
  volatile uintptr_t map;    // in-use bit per MI_SEGMENT_SIZE block
  volatile void*     start;  // start of virtual memory area
} mem_region_t;


// The region map; 16KiB for a 256GiB HEAP_REGION_MAX
// TODO: in the future, maintain a map per NUMA node for numa aware allocation
static mem_region_t regions[MI_REGION_MAX];

static volatile size_t regions_count = 0;        // allocated regions
static volatile uintptr_t region_next_idx = 0;


/* ----------------------------------------------------------------------------
Utility functions
-----------------------------------------------------------------------------*/

// Blocks (of 4MiB) needed for the given size.
static size_t mi_region_block_count(size_t size) {
  mi_assert_internal(size <= MI_REGION_MAX_ALLOC_SIZE);
  return (size + MI_SEGMENT_SIZE - 1) / MI_SEGMENT_SIZE;
}

// The bit mask for a given number of blocks at a specified bit index.
static uintptr_t mi_region_block_mask(size_t blocks, size_t bitidx) {
  mi_assert_internal(blocks + bitidx <= MI_REGION_MAP_BITS);
  return ((((uintptr_t)1 << blocks) - 1) << bitidx);
}

// Return a rounded commit/reset size such that we don't fragment large OS pages into small ones.
static size_t mi_good_commit_size(size_t size) {
  if (size > (SIZE_MAX - _mi_os_large_page_size())) return size;
  return _mi_align_up(size, _mi_os_large_page_size());  
}

/* ----------------------------------------------------------------------------
Commit from a region
-----------------------------------------------------------------------------*/

// Commit the `blocks` in `region` at `idx` and `bitidx` of a given `size`. 
// Returns `false` on an error (OOM); `true` otherwise. `p` and `id` are only written
// if the blocks were successfully claimed so ensure they are initialized to NULL/SIZE_MAX before the call. 
// (not being able to claim is not considered an error so check for `p != NULL` afterwards).
static bool mi_region_commit_blocks(mem_region_t* region, size_t idx, size_t bitidx, size_t blocks, size_t size, bool commit, void** p, size_t* id, mi_os_tld_t* tld) {
  size_t mask = mi_region_block_mask(blocks,bitidx);
  mi_assert_internal(mask != 0);
  mi_assert_internal((mask & mi_atomic_read(&region->map)) == mask);

  // ensure the region is reserved
  void* start = mi_atomic_read_ptr(&region->start);
  if (start == NULL) {    
    start = _mi_os_alloc_aligned(MI_REGION_SIZE, MI_SEGMENT_ALIGN, mi_option_is_enabled(mi_option_eager_region_commit), tld);
    if (start == NULL) {
      // failure to allocate from the OS! unclaim the blocks and fail
      size_t map;
      do {
        map = mi_atomic_read(&region->map);
      } while (!mi_atomic_compare_exchange(&region->map, map & ~mask, map));
      return false;
    }
    // set the newly allocated region
    if (mi_atomic_compare_exchange_ptr(&region->start, start, NULL)) {
      // update the region count
      mi_atomic_increment(&regions_count);
    }    
    else {
      // failed, another thread allocated just before us, free our allocated memory
      // TODO: should we keep the allocated memory and assign it to some other region?
      _mi_os_free(start, MI_REGION_SIZE, tld->stats);
      start = mi_atomic_read_ptr(&region->start);
    }    
  }

  // Commit the blocks to memory
  mi_assert_internal(start == mi_atomic_read_ptr(&region->start));
  mi_assert_internal(start != NULL);
  void* blocks_start = (uint8_t*)start + (bitidx * MI_SEGMENT_SIZE);
  if (commit && !mi_option_is_enabled(mi_option_eager_region_commit)) {
    _mi_os_commit(blocks_start, mi_good_commit_size(size), tld->stats);  // only commit needed size (unless using large OS pages)
  }

  // and return the allocation
  mi_atomic_write(&region_next_idx,idx);  // next search from here
  *p  = blocks_start;
  *id = (idx*MI_REGION_MAP_BITS) + bitidx;
  return true;
}

// Allocate `blocks` in a `region` at `idx` of a given `size`. 
// Returns `false` on an error (OOM); `true` otherwise. `p` and `id` are only written
// if the blocks were successfully claimed so ensure they are initialized to NULL/SIZE_MAX before the call. 
// (not being able to claim is not considered an error so check for `p != NULL` afterwards).
static bool mi_region_alloc_blocks(mem_region_t* region, size_t idx, size_t blocks, size_t size, bool commit, void** p, size_t* id, mi_os_tld_t* tld) {
  mi_assert_internal(p != NULL && id != NULL);
  mi_assert_internal(blocks < MI_REGION_MAP_BITS);

  const uintptr_t mask = mi_region_block_mask(blocks,0);
  const size_t bitidx_max = MI_REGION_MAP_BITS - blocks;
  size_t bitidx = 0;
  uintptr_t map;
  uintptr_t newmap;
  do {   // while no atomic claim success and not all bits seen
    // find the first free range of bits
    map = mi_atomic_read(&region->map);
    size_t m = map;
    do {
      // skip ones
      while ((m&1) == 1) { bitidx++; m>>=1; }
      // count zeros
      mi_assert_internal((m&1)==0);
      size_t zeros = 1;
      m >>= 1;
      while(zeros < blocks && (m&1)==0) { zeros++; m>>=1; }
      if (zeros == blocks) break; // found a range that fits
      bitidx += zeros;    
    }
    while(bitidx <= bitidx_max);
    if (bitidx > bitidx_max) {
      return true;  // no error, but could not find a range either
    }

    // try to claim it
    mi_assert_internal( (mask << bitidx) >> bitidx == mask ); // no overflow?
    mi_assert_internal( (map & (mask << bitidx)) == 0);         // fits in zero range
    newmap = map | (mask << bitidx);
    mi_assert_internal((newmap^map) >> bitidx == mask); 
  }
  while(!mi_atomic_compare_exchange(&region->map, newmap, map)); 

  // success, we claimed the blocks atomically
  // now commit the block memory -- this can still fail
  return mi_region_commit_blocks(region, idx, bitidx, blocks, size, commit, p, id, tld);
}

// Try to allocate `blocks` in a `region` at `idx` of a given `size`. Does a quick check before trying to claim.
// Returns `false` on an error (OOM); `true` otherwise. `p` and `id` are only written
// if the blocks were successfully claimed so ensure they are initialized to NULL/0 before the call. 
// (not being able to claim is not considered an error so check for `p != NULL` afterwards).
static bool mi_region_try_alloc_blocks(size_t idx, size_t blocks, size_t size, bool commit, void** p, size_t* id, mi_os_tld_t* tld)
{
  // check if there are available blocks in the region..
  mi_assert_internal(idx < MI_REGION_MAX);
  mem_region_t* region = &regions[idx];
  uintptr_t m = mi_atomic_read(&region->map);
  if (m != MI_REGION_MAP_FULL) {  // some bits are zero
    return mi_region_alloc_blocks(region, idx, blocks, size, commit, p, id, tld);
  }
  else {
    return true;  // no error, but no success either
  }
}

/* ----------------------------------------------------------------------------
 Allocation
-----------------------------------------------------------------------------*/

// Allocate `size` memory aligned at `alignment`. Return non NULL on success, with a given memory `id`.
// (`id` is abstract, but `id = idx*MI_REGION_MAP_BITS + bitidx`)
void* _mi_mem_alloc_aligned(size_t size, size_t alignment, bool commit, size_t* id, mi_os_tld_t* tld)
{
  mi_assert_internal(id != NULL && tld != NULL);
  mi_assert_internal(size > 0);
  *id = SIZE_MAX;

  // use direct OS allocation for huge blocks or alignment (with `id = SIZE_MAX`)
  if (size > MI_REGION_MAX_ALLOC_SIZE || alignment > MI_SEGMENT_ALIGN) {
    return _mi_os_alloc_aligned(mi_good_commit_size(size), alignment, true, tld);  // round up size
  }

  // always round size to OS page size multiple (so commit/decommit go over the entire range)
  // TODO: use large OS page size here?
  size = _mi_align_up(size, _mi_os_page_size());

  // calculate the number of needed blocks
  size_t blocks = mi_region_block_count(size);
  mi_assert_internal(blocks > 0 && blocks <= 8*MI_INTPTR_SIZE);

  // find a range of free blocks
  void* p = NULL;
  size_t count = mi_atomic_read(&regions_count);
  size_t idx = mi_atomic_read(&region_next_idx);
  for (size_t visited = 0; visited < count; visited++, idx++) {
    if (!mi_region_try_alloc_blocks(idx%count, blocks, size, commit, &p, id, tld)) return NULL; // error
    if (p != NULL) break;    
  }

  if (p == NULL) {
    // no free range in existing regions -- try to extend beyond the count
    for (idx = count; idx < MI_REGION_MAX; idx++) {
      if (!mi_region_try_alloc_blocks(idx, blocks, size, commit, &p, id, tld)) return NULL; // error
      if (p != NULL) break;
    }
  }

  if (p == NULL) {
    // we could not find a place to allocate, fall back to the os directly
    p = _mi_os_alloc_aligned(size, alignment, commit, tld);
  }

  mi_assert_internal( p == NULL || (uintptr_t)p % alignment == 0);
  return p;
}


// Allocate `size` memory. Return non NULL on success, with a given memory `id`.
void* _mi_mem_alloc(size_t size, bool commit, size_t* id, mi_os_tld_t* tld) {
  return _mi_mem_alloc_aligned(size,0,commit,id,tld);
}

/* ----------------------------------------------------------------------------
Free
-----------------------------------------------------------------------------*/

// Free previously allocated memory with a given id.
void _mi_mem_free(void* p, size_t size, size_t id, mi_stats_t* stats) {
  mi_assert_internal(size > 0 && stats != NULL);
  if (p==NULL) return;
  if (size==0) return;
  if (id == SIZE_MAX) {
   // was a direct OS allocation, pass through
    _mi_os_free(p, size, stats); 
  }
  else {
    // allocated in a region 
    mi_assert_internal(size <= MI_REGION_MAX_ALLOC_SIZE); if (size > MI_REGION_MAX_ALLOC_SIZE) return;
    // we can align the size up to page size (as we allocate that way too)
    // this ensures we fully commit/decommit/reset
    size = _mi_align_up(size, _mi_os_page_size());
    size_t idx = (id / MI_REGION_MAP_BITS);
    size_t bitidx = (id % MI_REGION_MAP_BITS);
    size_t blocks = mi_region_block_count(size);
    size_t mask = mi_region_block_mask(blocks, bitidx);
    mi_assert_internal(idx < MI_REGION_MAX); if (idx >= MI_REGION_MAX) return; // or `abort`?
    mem_region_t* region = &regions[idx];
    mi_assert_internal((mi_atomic_read(&region->map) & mask) == mask ); // claimed?
    void* start = mi_atomic_read_ptr(&region->start);
    mi_assert_internal(start != NULL); 
    void* blocks_start = (uint8_t*)start + (bitidx * MI_SEGMENT_SIZE);
    mi_assert_internal(blocks_start == p); // not a pointer in our area?
    mi_assert_internal(bitidx + blocks <= MI_REGION_MAP_BITS);
    if (blocks_start != p || bitidx + blocks > MI_REGION_MAP_BITS) return; // or `abort`?

    // decommit (or reset) the blocks to reduce the working set.
    // TODO: implement delayed decommit/reset as these calls are too expensive 
    // if the memory is reused soon.
    // reset: 10x slowdown on malloc-large, decommit: 17x slowdown on malloc-large
    if (mi_option_is_enabled(mi_option_eager_region_commit)) {
      _mi_os_reset(p, size, stats);      // 10x slowdown on malloc-large
    }
    else {      
      _mi_os_decommit(p, size, stats);  // 17x slowdown on malloc-large
    }

    // TODO: should we free empty regions? 
    // this frees up virtual address space which
    // might be useful on 32-bit systems?
    
    // and unclaim
    uintptr_t map;
    uintptr_t newmap;
    do {
      map = mi_atomic_read(&region->map);
      newmap = map & ~mask;
    } while (!mi_atomic_compare_exchange(&region->map, newmap, map));
  }
}

/* ----------------------------------------------------------------------------
  Other
-----------------------------------------------------------------------------*/

bool _mi_mem_commit(void* p, size_t size, mi_stats_t* stats) {
  return _mi_os_commit(p, size, stats);
}

bool _mi_mem_decommit(void* p, size_t size, mi_stats_t* stats) {
  return _mi_os_decommit(p, size, stats);
}

bool _mi_mem_reset(void* p, size_t size, mi_stats_t* stats) {
  return _mi_os_reset(p, size, stats);
}

bool _mi_mem_unreset(void* p, size_t size, mi_stats_t* stats) {
  return _mi_os_unreset(p, size, stats);
}

bool _mi_mem_protect(void* p, size_t size) {
  return _mi_os_protect(p, size);
}

bool _mi_mem_unprotect(void* p, size_t size) {
  return _mi_os_unprotect(p, size);
}
