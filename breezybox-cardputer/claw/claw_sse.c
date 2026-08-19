#include "claw_sse.h"

#include <string.h>

void claw_sse_init(claw_sse_t *p, claw_sse_cb_t cb, void *ctx)
{
    memset(p, 0, sizeof(*p));
    p->cb = cb;
    p->ctx = ctx;
}

static void sse_dispatch(claw_sse_t *p)
{
    if (p->data_len == 0) {
        p->event[0] = '\0';
        return;
    }
    p->data[p->data_len] = '\0';
    if (p->cb) {
        p->cb(p->event, p->data, p->ctx);
    }
    p->data_len = 0;
    p->event[0] = '\0';
}

static void sse_line(claw_sse_t *p, const char *line, size_t len)
{
    if (len == 0) {
        sse_dispatch(p);
        return;
    }
    if (line[0] == ':') {
        return;                      /* comment / keep-alive */
    }

    /* field[:[space]value] */
    const char *colon = memchr(line, ':', len);
    size_t flen = colon ? (size_t)(colon - line) : len;
    const char *value = colon ? colon + 1 : line + len;
    size_t vlen = colon ? len - flen - 1 : 0;
    if (vlen > 0 && value[0] == ' ') {   /* one optional leading space */
        value++;
        vlen--;
    }

    if (flen == 5 && memcmp(line, "event", 5) == 0) {
        size_t n = vlen < sizeof(p->event) - 1 ? vlen : sizeof(p->event) - 1;
        memcpy(p->event, value, n);
        p->event[n] = '\0';
    } else if (flen == 4 && memcmp(line, "data", 4) == 0) {
        /* Multiple data: lines in one event join with newlines, per the spec. */
        if (p->data_len > 0 && p->data_len < CLAW_SSE_MAX_DATA - 1) {
            p->data[p->data_len++] = '\n';
        }
        size_t space = CLAW_SSE_MAX_DATA - 1 - p->data_len;
        size_t n = vlen < space ? vlen : space;
        if (n < vlen) {
            p->truncated = 1;
        }
        memcpy(p->data + p->data_len, value, n);
        p->data_len += n;
    }
    /* id / retry ignored: we never resume a stream. */
}

void claw_sse_feed(claw_sse_t *p, const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\n') {
            size_t l = p->line_len;
            if (l > 0 && p->line[l - 1] == '\r') {
                l--;
            }
            sse_line(p, p->line, l);
            p->line_len = 0;
        } else if (p->line_len < CLAW_SSE_MAX_LINE - 1) {
            p->line[p->line_len++] = c;
        } else {
            p->truncated = 1;        /* overlong line: drop the excess */
        }
    }
}

void claw_sse_finish(claw_sse_t *p)
{
    if (p->line_len > 0) {
        size_t l = p->line_len;
        if (p->line[l - 1] == '\r') {
            l--;
        }
        sse_line(p, p->line, l);
        p->line_len = 0;
    }
    sse_dispatch(p);
}
