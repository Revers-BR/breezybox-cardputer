#pragma once
/*
 * claw's private heap, made from the graphics framebuffer while claw runs.
 *
 * The framebuffer is lent rather than freed (rgb_display_lend_gfx), so it is
 * never exposed to the general heap and graphics gets it back whole. claw's
 * large buffers and its cJSON trees come from here first, and from the general
 * heap when it is full.
 */
#include <stdbool.h>
#include <stddef.h>

/* Turn `mem` into the arena and route cJSON through it. */
void claw_arena_begin(void *mem, size_t size);

/* Stop using the arena. Returns true when everything allocated from it has
 * been freed, i.e. the memory can go back to its owner. When it returns false
 * the arena stays registered, so later frees of what is left still work. */
bool claw_arena_end(void);

/* Bytes still allocated in the arena; 0 when none is registered. */
size_t claw_arena_in_use(void);

/* Arena first, general heap when it is full or not active. claw_free accepts
 * pointers from either. */
void *claw_malloc(size_t size);
void *claw_calloc(size_t n, size_t size);
void  claw_free(void *p);
