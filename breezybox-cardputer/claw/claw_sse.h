/*
 * claw_sse.h - incremental Server-Sent Events parser.
 *
 * Chunks arrive from the HTTP layer at arbitrary byte boundaries, so a line --
 * or a whole event -- can straddle two reads. Only the trailing partial line is
 * retained between feeds, which keeps memory independent of response length.
 *
 * Fixed-size buffers throughout: no allocation happens during a stream.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * An SSE `data:` payload.
 *
 * This has to hold the largest single event, and the largest events are tool
 * calls: a model writing a screenful of Lua sends the whole thing in one
 * `data:` line. 2 KB was not enough -- an 8x8 LED pattern overflowed it, the
 * truncated JSON failed to parse, and the turn produced nothing.
 *
 * The struct is heap-allocated (see claw_round) precisely so this can be
 * generous; it would not fit on the console task stack.
 *
 * 8 KB was still not enough: a model writing a complete TUI application sends
 * the whole script in one event, and 16 KB covers what a screenful of Lua
 * plus its JSON escaping actually costs.
 */
#define CLAW_SSE_MAX_DATA   16384
#define CLAW_SSE_MAX_EVENT  64

/*
 * Carry-over for a line split across reads.
 *
 * This does not need to match CLAW_SSE_MAX_DATA. Only the tail of an
 * incomplete line lives here -- each fragment is appended to `data` as soon as
 * a newline arrives -- so it needs to hold one read's worth, not a whole event.
 * Sizing it as MAX_DATA + 32 doubled the struct to 32 KB in one contiguous
 * allocation, which a fragmented heap could not satisfy.
 */
#define CLAW_SSE_MAX_LINE   2048

/* event may be "" when the stream sends only `data:` lines (OpenAI, Gemini). */
typedef void (*claw_sse_cb_t)(const char *event, const char *data, void *ctx);

typedef struct {
    char   line[CLAW_SSE_MAX_LINE];   /* partial line carried between feeds */
    size_t line_len;
    char   event[CLAW_SSE_MAX_EVENT];
    char   data[CLAW_SSE_MAX_DATA];
    size_t data_len;
    bool   in_data;                   /* mid-way through an overlong data: line */
    int    truncated;                 /* a field exceeded its buffer */
    claw_sse_cb_t cb;
    void  *ctx;
} claw_sse_t;

void claw_sse_init(claw_sse_t *p, claw_sse_cb_t cb, void *ctx);
void claw_sse_feed(claw_sse_t *p, const char *buf, size_t len);
/* Flush an event the server left without a trailing blank line. */
void claw_sse_finish(claw_sse_t *p);
