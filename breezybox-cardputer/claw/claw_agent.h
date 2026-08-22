/*
 * claw_agent.h - one turn of the agent loop.
 *
 * Memory discipline, which is the whole reason this is C rather than Lua:
 *   - the request body is written to disk and streamed from there, so context
 *     length is bounded by storage, not the heap
 *   - the response is parsed one SSE event at a time and never buffered
 *   - decoded text goes straight to the console
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int    status;        /* HTTP status, or -1 if the connection failed */
    size_t chunks;
    size_t events;
    size_t bytes;
    size_t bytes_sent;    /* request size; grows with each tool round */
    unsigned elapsed_ms;
    int    turns;         /* turns replayed into the request */
    unsigned tool_calls;  /* tools executed while answering */
    bool   got_text;
    char   error[160];    /* empty when the turn succeeded */
} claw_result_t;

/* Send one user prompt and stream the reply to stdout.
 * Returns 0 on success. `verbose` adds transport and heap statistics. */
int claw_agent_ask(const char *prompt, bool verbose, claw_result_t *out);
