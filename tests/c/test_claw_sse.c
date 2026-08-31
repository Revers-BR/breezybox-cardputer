/*
 * Host test for the C SSE parser. Mirrors tests/test_sse.lua so the C core is
 * held to the behaviour the Lua prototype was verified against.
 *
 * Build & run:  sh tests/c/run.sh
 */
#include "claw_sse.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;

static void check(const char *name, int ok, const char *detail)
{
    if (ok) {
        printf("  ok   %s\n", name);
    } else {
        failures++;
        printf("  FAIL %s%s%s\n", name, detail ? "  -- " : "", detail ? detail : "");
    }
}

#define MAX_EV 32
typedef struct {
    int  n;
    char event[MAX_EV][CLAW_SSE_MAX_EVENT];
    char data[MAX_EV][CLAW_SSE_MAX_DATA];
} collected_t;

static void on_event(const char *event, const char *data, void *ctx)
{
    collected_t *c = ctx;
    if (c->n >= MAX_EV) {
        return;
    }
    snprintf(c->event[c->n], CLAW_SSE_MAX_EVENT, "%s", event);
    snprintf(c->data[c->n], CLAW_SSE_MAX_DATA, "%s", data);
    c->n++;
}

/* chunk == 0 feeds the whole stream in one go. */
static void collect(collected_t *out, const char *stream, size_t chunk)
{
    claw_sse_t p;
    memset(out, 0, sizeof(*out));
    claw_sse_init(&p, on_event, out);
    size_t len = strlen(stream);
    if (chunk == 0) {
        claw_sse_feed(&p, stream, len);
    } else {
        for (size_t i = 0; i < len; i += chunk) {
            size_t n = (i + chunk <= len) ? chunk : (len - i);
            claw_sse_feed(&p, stream + i, n);
        }
    }
    claw_sse_finish(&p);
}

static int same(const collected_t *a, const collected_t *b)
{
    if (a->n != b->n) {
        return 0;
    }
    for (int i = 0; i < a->n; i++) {
        if (strcmp(a->event[i], b->event[i]) != 0) return 0;
        if (strcmp(a->data[i], b->data[i]) != 0)   return 0;
    }
    return 1;
}

static const char *ANTHROPIC =
    "event: message_start\n"
    "data: {\"type\":\"message_start\"}\n"
    "\n"
    "event: content_block_delta\n"
    "data: {\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}\n"
    "\n"
    "event: content_block_delta\n"
    "data: {\"delta\":{\"type\":\"text_delta\",\"text\":\", world\"}}\n"
    "\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n"
    "\n";

static const char *OPENAI =
    "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n"
    "\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n"
    "\n"
    "data: [DONE]\n"
    "\n";

int main(void)
{
    collected_t base, got;

    printf("basic parsing\n");
    collect(&base, ANTHROPIC, 0);
    check("event count", base.n == 4, NULL);
    check("first event name", strcmp(base.event[0], "message_start") == 0, base.event[0]);
    check("text delta present", strstr(base.data[1], "Hello") != NULL, base.data[1]);
    check("last event", strcmp(base.event[3], "message_stop") == 0, base.event[3]);

    printf("\nsplit invariance (every chunk size)\n");
    {
        char detail[64];
        int bad = 0;
        for (size_t sz = 1; sz <= strlen(ANTHROPIC); sz++) {
            collect(&got, ANTHROPIC, sz);
            if (!same(&base, &got)) {
                snprintf(detail, sizeof(detail), "chunk_size=%zu", sz);
                bad = 1;
                break;
            }
        }
        check("identical events at every split", !bad, bad ? detail : NULL);
    }

    printf("\nopenai shape\n");
    {
        collected_t oa;
        collect(&oa, OPENAI, 0);
        check("event count", oa.n == 3, NULL);
        check("events are unnamed", oa.event[0][0] == '\0', oa.event[0]);
        check("[DONE] surfaces as data", strcmp(oa.data[2], "[DONE]") == 0, oa.data[2]);

        int bad = 0;
        for (size_t sz = 1; sz <= strlen(OPENAI); sz++) {
            collected_t g;
            collect(&g, OPENAI, sz);
            if (!same(&oa, &g)) { bad = 1; break; }
        }
        check("split invariance", !bad, NULL);
    }

    printf("\nedge cases\n");
    {
        collected_t c;

        collect(&c, "event: ping\r\ndata: {}\r\n\r\n", 0);
        check("CRLF line endings",
              c.n == 1 && strcmp(c.event[0], "ping") == 0 && strcmp(c.data[0], "{}") == 0, NULL);

        collect(&c, ": keep-alive\n\nevent: x\ndata: 1\n\n", 0);
        check("comments ignored", c.n == 1 && strcmp(c.event[0], "x") == 0, NULL);

        collect(&c, "data: bare\n\n", 0);
        check("data with no event name",
              c.n == 1 && c.event[0][0] == '\0' && strcmp(c.data[0], "bare") == 0, NULL);

        collect(&c, "data: a\ndata: b\n\n", 0);
        check("multi-line data joined", c.n == 1 && strcmp(c.data[0], "a\nb") == 0, c.data[0]);

        collect(&c, "data: {\"url\":\"https://x.test/y\"}\n\n", 0);
        check("colons in value survive",
              c.n == 1 && strcmp(c.data[0], "{\"url\":\"https://x.test/y\"}") == 0, c.data[0]);

        collect(&c, "event: last\ndata: 9", 0);
        check("trailing event flushed",
              c.n == 1 && strcmp(c.event[0], "last") == 0 && strcmp(c.data[0], "9") == 0, NULL);

        collect(&c, "", 0);
        check("empty stream", c.n == 0, NULL);

        collect(&c, "data:nospace\n\n", 0);
        check("value without leading space", c.n == 1 && strcmp(c.data[0], "nospace") == 0, c.data[0]);
    }

    printf("\noverlong input is bounded, not fatal\n");
    {
        /* A data field larger than CLAW_SSE_MAX_DATA must truncate and flag,
         * never overflow. */
        size_t big = CLAW_SSE_MAX_DATA * 2;
        char *s = malloc(big + 32);
        memcpy(s, "data: ", 6);
        memset(s + 6, 'x', big);
        memcpy(s + 6 + big, "\n\n", 3);

        claw_sse_t p;
        collected_t c;
        memset(&c, 0, sizeof(c));
        claw_sse_init(&p, on_event, &c);
        claw_sse_feed(&p, s, strlen(s));
        claw_sse_finish(&p);

        check("oversized event still delivered", c.n == 1, NULL);
        check("truncated to buffer", strlen(c.data[0]) <= CLAW_SSE_MAX_DATA - 1, NULL);
        check("truncation flagged", p.truncated == 1, NULL);
        free(s);
    }

    printf("\nlines longer than the carry-over buffer\n");
    {
        /*
         * The line buffer is deliberately smaller than the data buffer, so a
         * data: line longer than CLAW_SSE_MAX_LINE has to spill into `data`
         * rather than be dropped. A model writing a screenful of Lua sends
         * exactly that.
         */
        size_t payload = CLAW_SSE_MAX_LINE * 3;
        char *s2 = malloc(payload + 32);
        memcpy(s2, "data: ", 6);
        memset(s2 + 6, 'x', payload);
        memcpy(s2 + 6 + payload, "\n\n", 3);

        collected_t c;
        collect(&c, s2, 0);
        check("long line delivered as one event", c.n == 1, NULL);
        check("payload survives the spill",
              c.n == 1 && strlen(c.data[0]) == payload, NULL);
        bool all_x = c.n == 1;
        for (size_t i = 0; all_x && i < strlen(c.data[0]); i++) {
            if (c.data[0][i] != 'x') all_x = false;
        }
        check("content intact", all_x, NULL);

        /* And identical however the stream is chopped up. */
        collected_t base2;
        collect(&base2, s2, 0);
        int bad = 0;
        size_t sizes[] = { 1, 7, 64, 511, 1024, 4096 };
        for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
            collected_t g;
            collect(&g, s2, sizes[i]);
            if (!same(&base2, &g)) { bad = 1; break; }
        }
        check("split invariance on a long line", !bad, NULL);
        free(s2);
    }

    printf("\n");
    if (failures == 0) {
        printf("all C sse tests passed\n");
        return 0;
    }
    printf("%d test(s) FAILED\n", failures);
    return 1;
}
