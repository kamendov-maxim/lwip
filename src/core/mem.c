/**
 * @file
 * Dynamic memory manager
 *
 * This is a lightweight replacement for the standard C library malloc().
 *
 * If you want to use the standard C library malloc() instead, define
 * MEM_LIBC_MALLOC to 1 in your lwipopts.h
 *
 * To let mem_malloc() use pools (prevents fragmentation and is much faster than
 * a heap but might waste some memory), define MEM_USE_POOLS to 1, define
 * MEM_USE_CUSTOM_POOLS to 1 and create a file "lwippools.h" that includes a list
 * of pools like this (more pools can be added between _START and _END):
 *
 * Define three pools with sizes 256, 512, and 1512 bytes
 * LWIP_MALLOC_MEMPOOL_START
 * LWIP_MALLOC_MEMPOOL(20, 256)
 * LWIP_MALLOC_MEMPOOL(10, 512)
 * LWIP_MALLOC_MEMPOOL(5, 1512)
 * LWIP_MALLOC_MEMPOOL_END
 */

/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *         Simon Goldschmidt
 *
 */

#include "lwip/opt.h"

#if !MEM_LIBC_MALLOC /* don't build if not configured for use in lwipopts.h */

#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/sys.h"
#include "lwip/stats.h"

#include <string.h>

#if MEM_USE_POOLS
/* lwIP head implemented with different sized pools */

/**
 * This structure is used to save the pool one element came from.
 */
struct mem_helper
{
   memp_t poolnr;
};

/**
 * Allocate memory: determine the smallest pool that is big enough
 * to contain an element of 'size' and get an element from that pool.
 *
 * @param size the size in bytes of the memory needed
 * @return a pointer to the allocated memory or NULL if the pool is empty
 */
void *
mem_malloc(mem_size_t size)
{
  struct mem_helper *element;
  memp_t poolnr;

  for (poolnr = MEMP_POOL_FIRST; poolnr <= MEMP_POOL_LAST; poolnr++) {
    /* is this pool big enough to hold an element of the required size
       plus a struct mem_helper that saves the pool this element came from? */
    if ((size + sizeof(struct mem_helper)) <= memp_sizes[poolnr]) {
      break;
    }
  }
  if (poolnr > MEMP_POOL_LAST) {
    LWIP_ASSERT("mem_malloc(): no pool is that big!", 0);
    return NULL;
  }
  element = (struct mem_helper*)memp_malloc(poolnr);
  if (element == NULL) {
    /* No need to DEBUGF or ASSERT: This error is already
       taken care of in memp.c */
    /** @todo: we could try a bigger pool if this one is empty! */
    return NULL;
  }

  /* save the pool number this element came from */
  element->poolnr = poolnr;
  /* and return a pointer to the memory directly after the struct mem_helper */
  element++;

  return element;
}

/**
 * Free memory previously allocated by mem_malloc. Loads the pool number
 * and calls memp_free with that pool number to put the element back into
 * its pool
 *
 * @param rmem the memory element to free
 */
void
mem_free(void *rmem)
{
  struct mem_helper *hmem = (struct mem_helper*)rmem;

  LWIP_ASSERT("rmem != NULL", (rmem != NULL));
  LWIP_ASSERT("rmem == MEM_ALIGN(rmem)", (rmem == LWIP_MEM_ALIGN(rmem)));

  /* get the original struct mem_helper */
  hmem--;

  LWIP_ASSERT("hmem != NULL", (hmem != NULL));
  LWIP_ASSERT("hmem == MEM_ALIGN(hmem)", (hmem == LWIP_MEM_ALIGN(hmem)));
  LWIP_ASSERT("hmem->poolnr < MEMP_MAX", (hmem->poolnr < MEMP_MAX));

  /* and put it in the pool we saved earlier */
  memp_free(hmem->poolnr, hmem);
}

#else /* MEM_USE_POOLS */
/* lwIP replacement for your libc malloc() */

/**
 * The heap is made up as a list of structs of this type.
 * This does not have to be aligned since for getting its size,
 * we only use the macro SIZEOF_STRUCT_MEM, which automatically alignes.
 */
struct mem {
  /** index (-> ram[next]) of the next struct */
  mem_size_t next;
  /** index (-> ram[next]) of the next struct */
  mem_size_t prev;
  /** 1: this area is used; 0: this area is unused */
  u8_t used;
};

/** All allocated blocks will be MIN_SIZE bytes big, at least!
 * MIN_SIZE can be overridden to suit your needs. Smaller values save space,
 * larger values could prevent too small blocks to fragment the RAM too much. */
#ifndef MIN_SIZE
#define MIN_SIZE             12
#endif /* MIN_SIZE */
/* some alignment macros: we define them here for better source code layout */
#define MIN_SIZE_ALIGNED     LWIP_MEM_ALIGN_SIZE(MIN_SIZE)
#define SIZEOF_STRUCT_MEM    LWIP_MEM_ALIGN_SIZE(sizeof(struct mem))
#define MEM_SIZE_ALIGNED     LWIP_MEM_ALIGN_SIZE(MEM_SIZE)

/** the heap. we need one struct mem at the end and some room for alignment */
static u8_t ram_heap[MEM_SIZE_ALIGNED + (2*SIZEOF_STRUCT_MEM) + MEM_ALIGNMENT];
/** pointer to the heap (ram_heap): for alignment, ram is now a pointer instead of an array */
static u8_t *ram;
/** the last entry, always unused! */
static struct mem *ram_end;
/** pointer to the lowest free block, this is used for faster search */
static struct mem *lfree;

/** concurrent access protection */
static sys_sem_t mem_sem;

#if LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT

static volatile u8_t mem_free_count;

/* Allow mem_free from other (e.g. interrupt) context */
#define LWIP_MEM_FREE_DECL_PROTECT()  SYS_ARCH_DECL_PROTECT(lev_free)
#define LWIP_MEM_FREE_PROTECT()       SYS_ARCH_PROTECT(lev_free)
#define LWIP_MEM_FREE_UNPROTECT()     SYS_ARCH_UNPROTECT(lev_free)
#define LWIP_MEM_ALLOC_DECL_PROTECT() SYS_ARCH_DECL_PROTECT(lev_alloc)
#define LWIP_MEM_ALLOC_PROTECT()      SYS_ARCH_PROTECT(lev_alloc)
#define LWIP_MEM_ALLOC_UNPROTECT()    SYS_ARCH_UNPROTECT(lev_alloc)

#else /* LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT */

/* Protect the heap only by using a semaphore */
#define LWIP_MEM_FREE_DECL_PROTECT()
#define LWIP_MEM_FREE_PROTECT()    sys_arch_sem_wait(mem_sem, 0)
#define LWIP_MEM_FREE_UNPROTECT()  sys_sem_signal(mem_sem)
/* mem_malloc is protected using semaphore AND LWIP_MEM_ALLOC_PROTECT */
#define LWIP_MEM_ALLOC_DECL_PROTECT()
#define LWIP_MEM_ALLOC_PROTECT()
#define LWIP_MEM_ALLOC_UNPROTECT()

#endif /* LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT */


/**
 * "Plug holes" by combining adjacent empty struct mems.
 * After this function is through, there should not exist
 * one empty struct mem pointing to another empty struct mem.
 *
 * @param mem this points to a struct mem which just has been freed
 * @internal this function is only called by mem_free() and mem_realloc()
 *
 * This assumes access to the heap is protected by the calling function
 * already.
 */
static void
plug_holes(struct mem *mem)
{
  struct mem *nmem;
  struct mem *pmem;

  LWIP_ASSERT("plug_holes: mem >= ram", (u8_t *)mem >= ram);
  LWIP_ASSERT("plug_holes: mem < ram_end", (u8_t *)mem < (u8_t *)ram_end);
  LWIP_ASSERT("plug_holes: mem->used == 0", mem->used == 0);

  /* plug hole forward */
  LWIP_ASSERT("plug_holes: mem->next <= MEM_SIZE_ALIGNED", mem->next <= MEM_SIZE_ALIGNED);

  nmem = (struct mem *)&ram[mem->next];
  if (mem != nmem && nmem->used == 0 && (u8_t *)nmem != (u8_t *)ram_end) {
    /* if mem->next is unused and not end of ram, combine mem and mem->next */
    if (lfree == nmem) {
      lfree = mem;
    }
    mem->next = nmem->next;
    ((struct mem *)&ram[nmem->next])->prev = (u8_t *)mem - ram;
  }

  /* plug hole backward */
  pmem = (struct mem *)&ram[mem->prev];
  if (pmem != mem && pmem->used == 0) {
    /* if mem->prev is unused, combine mem and mem->prev */
    if (lfree == mem) {
      lfree = pmem;
    }
    pmem->next = mem->next;
    ((struct mem *)&ram[mem->next])->prev = (u8_t *)pmem - ram;
  }
}

/**
 * Zero the heap and initialize start, end and lowest-free
 */
void
mem_init(void)
{
  struct mem *mem;

  LWIP_ASSERT("Sanity check alignment",
    (SIZEOF_STRUCT_MEM & (MEM_ALIGNMENT-1)) == 0);

  /* align the heap */
  ram = LWIP_MEM_ALIGN(ram_heap);
  /* initialize the start of the heap */
  mem = (struct mem *)ram;
  mem->next = MEM_SIZE_ALIGNED;
  mem->prev = 0;
  mem->used = 0;
  /* initialize the end of the heap */
  ram_end = (struct mem *)&ram[MEM_SIZE_ALIGNED];
  ram_end->used = 1;
  ram_end->next = MEM_SIZE_ALIGNED;
  ram_end->prev = MEM_SIZE_ALIGNED;

  mem_sem = sys_sem_new(1);

  /* initialize the lowest-free pointer to the start of the heap */
  lfree = (struct mem *)ram;

  MEM_STATS_AVAIL(avail, MEM_SIZE_ALIGNED);
}

/**
 * Put a struct mem back on the heap
 *
 * @param rmem is the data portion of a struct mem as returned by a previous
 *             call to mem_malloc()
 */
void
mem_free(void *rmem)
{
  struct mem *mem;
  LWIP_MEM_FREE_DECL_PROTECT();

  if (rmem == NULL) {
    LWIP_DEBUGF(MEM_DEBUG | LWIP_DBG_TRACE | 2, ("mem_free(p == NULL) was called.\n"));
    return;
  }
  LWIP_ASSERT("mem_free: sanity check alignment", (((mem_ptr_t)rmem) & (MEM_ALIGNMENT-1)) == 0);

  LWIP_ASSERT("mem_free: legal memory", (u8_t *)rmem >= (u8_t *)ram &&
    (u8_t *)rmem < (u8_t *)ram_end);

  if ((u8_t *)rmem < (u8_t *)ram || (u8_t *)rmem >= (u8_t *)ram_end) {
    SYS_ARCH_DECL_PROTECT(lev);
    LWIP_DEBUGF(MEM_DEBUG | 3, ("mem_free: illegal memory\n"));
    /* protect mem stats from concurrent access */
    SYS_ARCH_PROTECT(lev);
    MEM_STATS_INC(illegal);
    SYS_ARCH_UNPROTECT(lev);
    return;
  }
  /* protect the heap from concurrent access */
  LWIP_MEM_FREE_PROTECT();
  /* Get the corresponding struct mem ... */
  mem = (struct mem *)((u8_t *)rmem - SIZEOF_STRUCT_MEM);
  /* ... which has to be in a used state ... */
  LWIP_ASSERT("mem_free: mem->used", mem->used);
  /* ... and is now unused. */
  mem->used = 0;

  if (mem < lfree) {
    /* the newly freed struct is now the lowest */
    lfree = mem;
  }

  MEM_STATS_DEC_USED(used, mem->next - ((u8_t *)mem - ram));

  /* finally, see if prev or next are free also */
  plug_holes(mem);
#if LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT
  mem_free_count = 1;
#endif /* LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT */
  LWIP_MEM_FREE_UNPROTECT();
}

/**
 * In contrast to its name, mem_realloc can only shrink memory, not expand it.
 * Since the only use (for now) is in pbuf_realloc (which also can only shrink),
 * this shouldn't be a problem!
 *
 * @param rmem pointer to memory allocated by mem_malloc the is to be shrinked
 * @param newsize required size after shrinking (needs to be smaller than or
 *                equal to the previous size)
 * @return for compatibility reasons: is always == rmem, at the moment
 *         or NULL if newsize is > old size, in which case rmem is NOT touched
 *         or freed!
 */
void *
mem_realloc(void *rmem, mem_size_t newsize)
{
  mem_size_t size;
  mem_size_t ptr, ptr2;
  struct mem *mem, *mem2;
  /* use the FREE_PROTECT here: it protects with sem OR SYS_ARCH_PROTECT */
  LWIP_MEM_FREE_DECL_PROTECT();

  /* Expand the size of the allocated memory region so that we can
     adjust for alignment. */
  newsize = LWIP_MEM_ALIGN_SIZE(newsize);

  if(newsize < MIN_SIZE_ALIGNED) {
    /* every data block must be at least MIN_SIZE_ALIGNED long */
    newsize = MIN_SIZE_ALIGNED;
  }

  if (newsize > MEM_SIZE_ALIGNED) {
    return NULL;
  }

  LWIP_ASSERT("mem_realloc: legal memory", (u8_t *)rmem >= (u8_t *)ram &&
   (u8_t *)rmem < (u8_t *)ram_end);

  if ((u8_t *)rmem < (u8_t *)ram || (u8_t *)rmem >= (u8_t *)ram_end) {
    SYS_ARCH_DECL_PROTECT(lev);
    LWIP_DEBUGF(MEM_DEBUG | 3, ("mem_realloc: illegal memory\n"));
    /* protect mem stats from concurrent access */
    SYS_ARCH_PROTECT(lev);
    MEM_STATS_INC(illegal);
    SYS_ARCH_UNPROTECT(lev);
    return rmem;
  }
  /* Get the corresponding struct mem ... */
  mem = (struct mem *)((u8_t *)rmem - SIZEOF_STRUCT_MEM);
  /* ... and its offset pointer */
  ptr = (u8_t *)mem - ram;

  size = mem->next - ptr - SIZEOF_STRUCT_MEM;
  LWIP_ASSERT("mem_realloc can only shrink memory", newsize <= size);
  if (newsize > size) {
    /* not supported */
    return NULL;
  }
  if (newsize == size) {
    /* No change in size, simply return */
    return rmem;
  }

  /* protect the heap from concurrent access */
  LWIP_MEM_FREE_PROTECT();

  MEM_STATS_DEC_USED(used, (size - newsize));

  mem2 = (struct mem *)&ram[mem->next];
  if(mem2->used == 0) {
    /* The next struct is unused, we can simply move it at little */
    mem_size_t next;
    /* remember the old next pointer */
    next = mem2->next;
    /* create new struct mem which is moved directly after the shrinked mem */
    ptr2 = ptr + SIZEOF_STRUCT_MEM + newsize;
    if (lfree == mem2) {
      lfree = (struct mem *)&ram[ptr2];
    }
    mem2 = (struct mem *)&ram[ptr2];
    mem2->used = 0;
    /* restore the next pointer */
    mem2->next = next;
    /* link it back to mem */
    mem2->prev = ptr;
    /* link mem to it */
    mem->next = ptr2;
    /* last thing to restore linked list: as we have moved mem2,
     * let 'mem2->next->prev' point to mem2 again. but only if mem2->next is not
     * the end of the heap */
    if (mem2->next != MEM_SIZE_ALIGNED) {
      ((struct mem *)&ram[mem2->next])->prev = ptr2;
    }
    /* no need to plug holes, we've already done that */
  } else if (newsize + SIZEOF_STRUCT_MEM + MIN_SIZE_ALIGNED <= size) {
    /* Next struct is used but there's room for another struct mem with
     * at least MIN_SIZE_ALIGNED of data.
     * Old size ('size') must be big enough to contain at least 'newsize' plus a struct mem
     * ('SIZEOF_STRUCT_MEM') with some data ('MIN_SIZE_ALIGNED').
     * @todo we could leave out MIN_SIZE_ALIGNED. We would create an empty
     *       region that couldn't hold data, but when mem->next gets freed,
     *       the 2 regions would be combined, resulting in more free memory */
    ptr2 = ptr + SIZEOF_STRUCT_MEM + newsize;
    mem2 = (struct mem *)&ram[ptr2];
    if (mem2 < lfree) {
      lfree = mem2;
    }
    mem2->used = 0;
    mem2->next = mem->next;
    mem2->prev = ptr;
    mem->next = ptr2;
    if (mem2->next != MEM_SIZE_ALIGNED) {
      ((struct mem *)&ram[mem2->next])->prev = ptr2;
    }
    /* the original mem->next is used, so no need to plug holes! */
  }
  /* else {
    next struct mem is used but size between mem and mem2 is not big enough
    to create another struct mem
    -> don't do anyhting. 
    -> the remaining space stays unused since it is too small
  } */
#if LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT
  mem_free_count = 1;
#endif /* LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT */
  LWIP_MEM_FREE_UNPROTECT();
  return rmem;
}

/**
 * Adam's mem_malloc() plus solution for bug #17922
 * Allocate a block of memory with a minimum of 'size' bytes.
 *
 * @param size is the minimum size of the requested block in bytes.
 * @return pointer to allocated memory or NULL if no free memory was found.
 *
 * Note that the returned value will always be aligned (as defined by MEM_ALIGNMENT).
 */
void *
mem_malloc(mem_size_t size)
{
  mem_size_t ptr, ptr2;
  struct mem *mem, *mem2;
#if LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT
  u8_t local_mem_free_count = 0;
#endif /* LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT */
  LWIP_MEM_ALLOC_DECL_PROTECT();

  if (size == 0) {
    return NULL;
  }

  /* Expand the size of the allocated memory region so that we can
     adjust for alignment. */
  size = LWIP_MEM_ALIGN_SIZE(size);

  if(size < MIN_SIZE_ALIGNED) {
    /* every data block must be at least MIN_SIZE_ALIGNED long */
    size = MIN_SIZE_ALIGNED;
  }

  if (size > MEM_SIZE_ALIGNED) {
    return NULL;
  }

  /* protect the heap from concurrent access */
  sys_arch_sem_wait(mem_sem, 0);
  LWIP_MEM_ALLOC_PROTECT();
#if LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT
  /* run as long as a mem_free disturbed mem_malloc */
  do {
    local_mem_free_count = 0;
#endif /* LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT */

    /* Scan through the heap searching for a free block that is big enough,
     * beginning with the lowest free block.
     */
    for (ptr = (u8_t *)lfree - ram; ptr < MEM_SIZE_ALIGNED - size;
         ptr = ((struct mem *)&ram[ptr])->next) {
      mem = (struct mem *)&ram[ptr];
#if LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT
      mem_free_count = 0;
      LWIP_MEM_ALLOC_UNPROTECT();
      /* allow mem_free to run */
      LWIP_MEM_ALLOC_PROTECT();
      if (mem_free_count != 0) {
        local_mem_free_count = mem_free_count;
      }
      mem_free_count = 0;
#endif /* LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT */

      if ((!mem->used) &&
          (mem->next - (ptr + SIZEOF_STRUCT_MEM)) >= size) {
        /* mem is not used and at least perfect fit is possible:
         * mem->next - (ptr + SIZEOF_STRUCT_MEM) gives us the 'user data size' of mem */

        if (mem->next - (ptr + SIZEOF_STRUCT_MEM) >= (size + SIZEOF_STRUCT_MEM + MIN_SIZE_ALIGNED)) {
          /* (in addition to the above, we test if another struct mem (SIZEOF_STRUCT_MEM) containing
           * at least MIN_SIZE_ALIGNED of data also fits in the 'user data space' of 'mem')
           * -> split large block, create empty remainder,
           * remainder must be large enough to contain MIN_SIZE_ALIGNED data: if
           * mem->next - (ptr + (2*SIZEOF_STRUCT_MEM)) == size,
           * struct mem would fit in but no data between mem2 and mem2->next
           * @todo we could leave out MIN_SIZE_ALIGNED. We would create an empty
           *       region that couldn't hold data, but when mem->next gets freed,
           *       the 2 regions would be combined, resulting in more free memory
           */
          ptr2 = ptr + SIZEOF_STRUCT_MEM + size;
          /* create mem2 struct */
          mem2 = (struct mem *)&ram[ptr2];
          mem2->used = 0;
          mem2->next = mem->next;
          mem2->prev = ptr;
          /* and insert it between mem and mem->next */
          mem->next = ptr2;
          mem->used = 1;

          if (mem2->next != MEM_SIZE_ALIGNED) {
            ((struct mem *)&ram[mem2->next])->prev = ptr2;
          }
          MEM_STATS_INC_USED(used, (size + SIZEOF_STRUCT_MEM));
        } else {
          /* (a mem2 struct does no fit into the user data space of mem and mem->next will always
           * be used at this point: if not we have 2 unused structs in a row, plug_holes should have
           * take care of this).
           * -> near fit or excact fit: do not split, no mem2 creation
           * also can't move mem->next directly behind mem, since mem->next
           * will always be used at this point!
           */
          mem->used = 1;
          MEM_STATS_INC_USED(used, mem->next - ((u8_t *)mem - ram));
        }

        if (mem == lfree) {
          /* Find next free block after mem and update lowest free pointer */
          while (lfree->used && lfree != ram_end) {
            LWIP_MEM_ALLOC_UNPROTECT();
            /* prevent high interrupt latency... */
            LWIP_MEM_ALLOC_PROTECT();
            lfree = (struct mem *)&ram[lfree->next];
          }
          LWIP_ASSERT("mem_malloc: !lfree->used", ((lfree == ram_end) || (!lfree->used)));
        }
        LWIP_MEM_ALLOC_UNPROTECT();
        sys_sem_signal(mem_sem);
        LWIP_ASSERT("mem_malloc: allocated memory not above ram_end.",
         (mem_ptr_t)mem + SIZEOF_STRUCT_MEM + size <= (mem_ptr_t)ram_end);
        LWIP_ASSERT("mem_malloc: allocated memory properly aligned.",
         ((mem_ptr_t)mem + SIZEOF_STRUCT_MEM) % MEM_ALIGNMENT == 0);
        LWIP_ASSERT("mem_malloc: sanity check alignment",
          (((mem_ptr_t)mem) & (MEM_ALIGNMENT-1)) == 0);

        return (u8_t *)mem + SIZEOF_STRUCT_MEM;
      }
    }
#if LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT
    /* if we got interrupted by a mem_free, try again */
  } while(local_mem_free_count != 0);
#endif /* LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT */
  LWIP_DEBUGF(MEM_DEBUG | 2, ("mem_malloc: could not allocate %"S16_F" bytes\n", (s16_t)size));
  MEM_STATS_INC(err);
  LWIP_MEM_ALLOC_UNPROTECT();
  sys_sem_signal(mem_sem);
  return NULL;
}

#endif /* MEM_USE_POOLS */
/**
 * Contiguously allocates enough space for count objects that are size bytes
 * of memory each and returns a pointer to the allocated memory.
 *
 * The allocated memory is filled with bytes of value zero.
 *
 * @param count number of objects to allocate
 * @param size size of the objects to allocate
 * @return pointer to allocated memory / NULL pointer if there is an error
 */
void *mem_calloc(mem_size_t count, mem_size_t size)
{
  void *p;

  /* allocate 'count' objects of size 'size' */
  p = mem_malloc(count * size);
  if (p) {
    /* zero the memory */
    memset(p, 0, count * size);
  }
  return p;
}

#endif /* !MEM_LIBC_MALLOC */
