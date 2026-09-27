#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <malloc.h>
#include <windows.h>

#include "memory.h"
#include "../debug_print.h"

// The pools: the display lists' (emptied every frame), the graph nodes' (kept), the objects' (the chain chomp's
// chain, the wiggler's body).  Two ways to catch a block written past its end.  Guard pages (LIBSM64_DEBUG_GUARD_PAGES):
// every block sits at the very end of its own page(s) with a page nobody may touch after it, so the write faults
// on the spot and the crash logger names the line.  Canaries otherwise: a strip of bytes after each block,
// checked when its pool is freed.
u32 gLibsm64DebugFlags = 0;

#define CANARY_BYTES 16
#define CANARY_BYTE 0xC5

struct AllocOnlyPool
{
    size_t allocatedCount;
    size_t capacity;
    void **allocatedBlocks;
    size_t *allocatedSizes;
    uint8_t *allocatedGuarded;                       // 1: from the guarded pages, 0: from malloc with a canary
};

struct AllocOnlyPool *s_display_list_pool;

// ------------------------------------------------------------------------------------------------ guarded pages
// One reserved region, carved into runs of pages: a block's data pages and one guard page after them.  Runs are
// committed once and recycled by length, so a frame's worth of display lists settles into the same pages.
#define GUARD_PAGE 4096
#define GUARD_REGION (256u << 20)
#define GUARD_RUNS 16                                // free lists for runs of 1..16 data pages

static uint8_t *s_guardBase;
static size_t s_guardUsed;
static void *s_guardFree[GUARD_RUNS + 1];            // per data page count: the head of a list threaded through the runs

static bool guard_pages_on(void) { return (gLibsm64DebugFlags & LIBSM64_DEBUG_GUARD_PAGES) != 0; }

static size_t guard_data_pages(size_t size) { return (size + GUARD_PAGE - 1) / GUARD_PAGE; }

static void *guard_alloc(size_t size)
{
    if( size == 0 ) size = 1;                        // an empty block still needs an address of its own
    size_t pages = guard_data_pages(size);
    uint8_t *run = NULL;
    if( pages <= GUARD_RUNS && s_guardFree[pages] != NULL )
    {
        run = s_guardFree[pages];
        s_guardFree[pages] = *(void **)run;
    }
    else
    {
        if( s_guardBase == NULL )
        {
            s_guardBase = VirtualAlloc( NULL, GUARD_REGION, MEM_RESERVE, PAGE_NOACCESS );
            if( s_guardBase == NULL ) return NULL;
        }
        size_t need = ( pages + 1 ) * GUARD_PAGE;
        if( s_guardUsed + need > GUARD_REGION ) return NULL;
        run = s_guardBase + s_guardUsed;
        s_guardUsed += need;
        if( VirtualAlloc( run, pages * GUARD_PAGE, MEM_COMMIT, PAGE_READWRITE ) == NULL ) return NULL;
        // the guard page stays reserved and unmapped: touching it is an access violation
    }
    return run + pages * GUARD_PAGE - size;          // the block ends exactly where the guard page begins
}

static void guard_free(void *block, size_t size)
{
    if( size == 0 ) size = 1;
    size_t pages = guard_data_pages(size);
    uint8_t *run = (uint8_t *)block + size - pages * GUARD_PAGE;
    if( pages <= GUARD_RUNS )
    {
        *(void **)run = s_guardFree[pages];
        s_guardFree[pages] = run;
    }
    // bigger runs are simply left; nothing here is that big
}

// ------------------------------------------------------------------------------------------------ the heap check
static bool heap_ok(void)
{
    if( _heapchk() != _HEAPOK ) return false;        // the C runtime's heap, the one this library's malloc uses
    HANDLE heaps[64];
    DWORD n = GetProcessHeaps( 64, heaps );
    if( n > 64 ) n = 64;
    for( DWORD i = 0; i < n; ++i )
        if( !HeapValidate( heaps[i], 0, NULL )) return false;
    return true;
}

void libsm64_heap_check(const char *where)
{
    static bool reported = false;
    if( !( gLibsm64DebugFlags & ( LIBSM64_DEBUG_HEAP_CHECKS | LIBSM64_DEBUG_HEAP_OBJECTS ))) return;
    if( reported || heap_ok() ) return;
    reported = true;
    DEBUG_PRINT( "!! the heap is corrupt: first seen %s", where );
}

// ------------------------------------------------------------------------------------------------ the pools
void memory_init(void)
{
    const char *env = getenv( "SM64_DEBUG" );        // the smoke test's way in; the game sets the flags itself
    if( env != NULL && gLibsm64DebugFlags == 0 ) gLibsm64DebugFlags = (u32) strtoul( env, NULL, 0 );
    s_display_list_pool = alloc_only_pool_init();
}

void memory_terminate(void)
{
    alloc_only_pool_free( s_display_list_pool );
}

struct AllocOnlyPool *alloc_only_pool_init(void)
{
    struct AllocOnlyPool *newPool = malloc( sizeof( struct AllocOnlyPool ));
    newPool->allocatedCount = 0;
    newPool->capacity = 0;
    newPool->allocatedBlocks = NULL;
    newPool->allocatedSizes = NULL;
    newPool->allocatedGuarded = NULL;
    return newPool;
}

void *alloc_only_pool_alloc(struct AllocOnlyPool *pool, s32 size)
{
    if( size < 0 ) size = 0;
    if( pool->allocatedCount == pool->capacity )
    {
        pool->capacity = pool->capacity ? pool->capacity * 2 : 256;
        pool->allocatedBlocks = realloc( pool->allocatedBlocks, pool->capacity * sizeof( void * ));
        pool->allocatedSizes = realloc( pool->allocatedSizes, pool->capacity * sizeof( size_t ));
        pool->allocatedGuarded = realloc( pool->allocatedGuarded, pool->capacity );
    }
    uint8_t *block;
    uint8_t guarded = guard_pages_on() ? 1 : 0;
    if( guarded )
    {
        block = guard_alloc( (size_t)size );
        if( block == NULL ) { guarded = 0; }         // the region is full: fall back to the heap
    }
    if( !guarded )
    {
        block = malloc( (size_t)size + CANARY_BYTES );
        memset( block + size, CANARY_BYTE, CANARY_BYTES );
    }
    pool->allocatedBlocks[ pool->allocatedCount ] = block;
    pool->allocatedSizes[ pool->allocatedCount ] = (size_t)size;
    pool->allocatedGuarded[ pool->allocatedCount ] = guarded;
    pool->allocatedCount++;
    return block;
}

void alloc_only_pool_free(struct AllocOnlyPool *pool)
{
    for( size_t i = 0; i < pool->allocatedCount; ++i )
    {
        uint8_t *block = pool->allocatedBlocks[i];
        size_t size = pool->allocatedSizes[i];
        if( pool->allocatedGuarded[i] )
        {
            guard_free( block, size );
            continue;
        }
        for( int c = 0; c < CANARY_BYTES; ++c )
        {
            if( block[size + c] != CANARY_BYTE )
            {
                DEBUG_PRINT( "!! pool block %u of %u (%u bytes) was written past its end by at least %d bytes", (unsigned)i, (unsigned)pool->allocatedCount, (unsigned)size, c + 1 );
                break;
            }
        }
        free( block );
    }
    free( pool->allocatedBlocks );
    free( pool->allocatedSizes );
    free( pool->allocatedGuarded );
    free( pool );
}

void display_list_pool_reset(void)
{
    alloc_only_pool_free( s_display_list_pool );
    s_display_list_pool = alloc_only_pool_init();
}

void *alloc_display_list(u32 size)
{
    return alloc_only_pool_alloc( s_display_list_pool, (s32)size );
}

// the object engine's pools (the chain chomp's chain, the wiggler's body): each allocation its own block
struct MemoryPool { u32 unused; };

#define MEM_POOL_BLOCKS 256
static struct { void *block; size_t size; } s_memPoolBlocks[MEM_POOL_BLOCKS];   // the guarded ones, to free them

struct MemoryPool *mem_pool_init(u32 size, u32 side)
{
    (void)size; (void)side;
    return malloc( sizeof( struct MemoryPool ));
}

void *mem_pool_alloc(struct MemoryPool *pool, u32 size)
{
    (void)pool;
    if( guard_pages_on() )
    {
        for( int i = 0; i < MEM_POOL_BLOCKS; ++i )
        {
            if( s_memPoolBlocks[i].block != NULL ) continue;
            void *block = guard_alloc( size );
            if( block == NULL ) break;
            s_memPoolBlocks[i].block = block;
            s_memPoolBlocks[i].size = size;
            return block;
        }
    }
    return malloc( size );
}

void mem_pool_free(struct MemoryPool *pool, void *addr)
{
    (void)pool;
    for( int i = 0; i < MEM_POOL_BLOCKS; ++i )
    {
        if( s_memPoolBlocks[i].block != addr ) continue;
        guard_free( addr, s_memPoolBlocks[i].size );
        s_memPoolBlocks[i].block = NULL;
        return;
    }
    free( addr );
}
