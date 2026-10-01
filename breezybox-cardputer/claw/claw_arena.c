/*
 * claw's private heap, made from the graphics framebuffer while claw runs.
 *
 * claw needs the framebuffer's 36 KB while it runs. Freeing it to the general
 * heap and allocating it again on exit failed: allocations made during the
 * session (lwIP and WiFi buffers, caches) landed in the gap, and afterwards
 * 105 KB was free but the largest block was 31-34 KB, so graphics needed a
 * reboot. Here the buffer stays allocated and becomes a multi_heap of its own.
 *
 * cJSON is routed through it with cJSON_InitHooks, installed once and left
 * installed: claw_free checks which heap a pointer came from, so a tree built
 * in the arena can be freed after the arena closes, and a tree built on the
 * general heap can be freed while it is open.
 */
#include "claw_arena.h"

#include "cJSON.h"
#include "multi_heap.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static multi_heap_handle_t s_heap;
static uintptr_t s_start;
static uintptr_t s_end;
static bool s_active;            /* new allocations may come from the arena */
static bool s_hooks_installed;

static bool in_arena(const void *p)
{
    const uintptr_t a = (uintptr_t)p;
    return s_heap && a >= s_start && a < s_end;
}

void *claw_malloc(size_t size)
{
    if (s_active) {
        void *p = multi_heap_malloc(s_heap, size);
        if (p) {
            return p;
        }
    }
    return malloc(size);
}

void *claw_calloc(size_t n, size_t size)
{
    if (size && n > SIZE_MAX / size) {
        return NULL;
    }
    void *p = claw_malloc(n * size);
    if (p) {
        memset(p, 0, n * size);
    }
    return p;
}

void claw_free(void *p)
{
    if (!p) {
        return;
    }
    if (in_arena(p)) {
        multi_heap_free(s_heap, p);
    } else {
        free(p);
    }
}

void claw_arena_begin(void *mem, size_t size)
{
    if (!s_hooks_installed) {
        cJSON_Hooks hooks = { .malloc_fn = claw_malloc, .free_fn = claw_free };
        cJSON_InitHooks(&hooks);
        s_hooks_installed = true;
    }
    if (s_heap) {
        /* A previous session left something allocated; keep that arena. */
        s_active = true;
        return;
    }
    if (!mem || size == 0) {
        return;
    }
    s_heap = multi_heap_register(mem, size);
    if (s_heap) {
        s_start = (uintptr_t)mem;
        s_end = (uintptr_t)mem + size;
        s_active = true;
    }
}

size_t claw_arena_in_use(void)
{
    if (!s_heap) {
        return 0;
    }
    multi_heap_info_t info;
    multi_heap_get_info(s_heap, &info);
    return info.total_allocated_bytes;
}

bool claw_arena_end(void)
{
    s_active = false;
    if (!s_heap) {
        return true;
    }
    if (claw_arena_in_use() != 0) {
        return false;
    }
    s_heap = NULL;
    s_start = s_end = 0;
    return true;
}
