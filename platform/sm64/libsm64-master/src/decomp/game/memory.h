#pragma once

// game/memory.h as the decompilation's files name it: libsm64's own allocators (../memory.h) and the pools the
// chain chomp and the wiggler take their segments from.
#include "../memory.h"

#define MEMORY_POOL_LEFT  0
#define MEMORY_POOL_RIGHT 1

struct MemoryPool;

struct MemoryPool *mem_pool_init(u32 size, u32 side);
void *mem_pool_alloc(struct MemoryPool *pool, u32 size);
void mem_pool_free(struct MemoryPool *pool, void *addr);
// segmented_to_virtual / virtual_to_segmented: shim.h's, the same in every file
