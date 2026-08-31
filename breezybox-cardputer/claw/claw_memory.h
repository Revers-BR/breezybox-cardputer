/*
 * claw_memory.h - facts that outlive a conversation.
 *
 * Distinct from sessions. A session transcript is what was said; memory is what
 * the device should still know next week -- which accessory is on the Grove
 * port, what the user is building, how they like things done.
 *
 * Layout on the card:
 *
 *     /sd/claw/memory/MEMORY.md    index: one line per memory
 *     /sd/claw/memory/<name>.md    the memory itself
 *
 * The index is injected into every request; the bodies are not. Sending
 * everything would spend the context budget on facts that are usually
 * irrelevant, and sending nothing means the model never knows to look. The
 * index is small enough to carry always and specific enough to prompt a
 * memory_read when it matters.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define CLAW_MEMORY_NAME_MAX  48
#define CLAW_MEMORY_BODY_MAX  2048

/* Hard cap on what gets injected per request, so memory cannot crowd out the
 * conversation. Older index entries are dropped past this. */
#define CLAW_MEMORY_INJECT_MAX 768

/* Write the index as a system-prompt fragment. Returns the length written,
 * 0 when there is nothing remembered. */
size_t claw_memory_context(char *out, size_t out_len);

bool claw_memory_save(const char *name, const char *description, const char *content);
bool claw_memory_read(const char *name, char *out, size_t out_len);
bool claw_memory_delete(const char *name);

/* Print the index for the user. */
void claw_memory_list(void);
int  claw_memory_count(void);
