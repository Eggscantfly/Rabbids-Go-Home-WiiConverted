#pragma once

#include "include/types.h"

struct AllocOnlyPool;

extern void memory_init(void);
extern void memory_terminate(void);

extern struct AllocOnlyPool *alloc_only_pool_init(void);
extern void *alloc_only_pool_alloc(struct AllocOnlyPool *pool, s32 size);
extern void alloc_only_pool_free(struct AllocOnlyPool *pool);

extern void display_list_pool_reset(void);

// libsm64: what the diagnostic build does (sm64_set_debug, or the SM64_DEBUG environment variable)
#define LIBSM64_DEBUG_GUARD_PAGES 1                  // every pool block ends at a page nobody may touch
#define LIBSM64_DEBUG_HEAP_CHECKS 2                  // the heaps are validated at each phase of the tick
#define LIBSM64_DEBUG_HEAP_OBJECTS 4                 // and before each object's behaviour runs
extern u32 gLibsm64DebugFlags;
extern void libsm64_heap_check(const char *where);
extern void *alloc_display_list(u32 size);