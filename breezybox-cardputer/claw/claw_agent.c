#include "claw_agent.h"
#include "claw_backend.h"
#include "claw_config.h"
#include "claw_session.h"
#include "claw_sse.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "claw";

#define CLAW_IO_CHUNK   512
#define CLAW_REQ_SD     "/sd/claw/tmp/req.json"
#define CLAW_REQ_FLASH  "/root/.claw_req.json"

/* Where the shipped package lives, for backend CA files. Mirrors the search
 * order in cmd/claw.c so a working copy on the card wins. */
static const char *k_install_dirs[] = {
    "/sd/espclaw",
    "/sd/apps/espclaw",
    "/root/apps/espclaw",
};

typedef struct {
    const claw_backend_t *backend;
    claw_result_t *res;
    bool  in_error_body;      /* status was not 2xx: collect, do not parse */
    char  error_body[512];
    size_t error_body_len;

    /* The reply is echoed to the console as it streams, and also collected here
     * so it can be appended to the transcript. Bounded: a reply longer than
     * this is shown in full but stored truncated, which keeps the heap flat. */
    char  *reply;
    size_t reply_len;
    size_t reply_cap;
} stream_ctx_t;

static void heap_line(const char *label)
{
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    printf("[heap %s: free %u, min %u, largest %u]\n", label,
           (unsigned)heap_caps_get_free_size(caps),
           (unsigned)heap_caps_get_minimum_free_size(caps),
           (unsigned)heap_caps_get_largest_free_block(caps));
}

static bool network_ready(void)
{
    esp_netif_t *n = esp_netif_get_default_netif();
    if (!n) {
        return false;
    }
    esp_netif_ip_info_t ip;
    return esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr != 0;
}

static bool path_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

/* Resolve a backend's CA file against the install dirs. Empty when not found,
 * in which case we fall back to the certificate bundle. */
static void resolve_ca(const claw_backend_t *b, char *out, size_t n)
{
    out[0] = '\0';
    if (!b->ca_file) {
        return;
    }
    for (size_t i = 0; i < sizeof(k_install_dirs) / sizeof(k_install_dirs[0]); i++) {
        snprintf(out, n, "%s/%s", k_install_dirs[i], b->ca_file);
        if (path_exists(out)) {
            return;
        }
    }
    /* Always say so: silently downgrading from a pinned root to the bundle
     * looks like an unrelated TLS failure later. */
    printf("claw: warning - CA file missing, using cert bundle instead\n");
    printf("      %s\n", b->ca_file);
    out[0] = '\0';
}

static void on_sse_event(const char *event, const char *data, void *vctx)
{
    stream_ctx_t *ctx = vctx;
    ctx->res->events++;

    if (strcmp(data, "[DONE]") == 0) {
        return;
    }
    cJSON *obj = cJSON_Parse(data);
    if (!obj) {
        return;
    }

    char errbuf[160];
    const char *e = ctx->backend->extract_error(obj, errbuf, sizeof(errbuf));
    if (e) {
        snprintf(ctx->res->error, sizeof(ctx->res->error), "%s", e);
        cJSON_Delete(obj);
        return;
    }

    const char *text = ctx->backend->extract_text(event, obj);
    if (text && text[0]) {
        fputs(text, stdout);
        fflush(stdout);
        ctx->res->got_text = true;

        if (ctx->reply) {
            size_t n = strlen(text);
            size_t space = ctx->reply_cap - 1 - ctx->reply_len;
            size_t copy = (n < space) ? n : space;
            memcpy(ctx->reply + ctx->reply_len, text, copy);
            ctx->reply_len += copy;
            ctx->reply[ctx->reply_len] = '\0';
        }
    }
    cJSON_Delete(obj);
}

/* Write the request body to disk so it can be streamed with a known length. */
static bool write_body(const char *json, char *path_out, size_t path_len)
{
    const char *candidates[] = { CLAW_REQ_SD, CLAW_REQ_FLASH };
    /* Best-effort: the tmp dir may not exist yet. mkdir has no -p equivalent. */
    mkdir("/sd/claw", 0777);
    mkdir("/sd/claw/tmp", 0777);

    size_t len = strlen(json);
    for (size_t i = 0; i < 2; i++) {
        FILE *f = fopen(candidates[i], "wb");
        if (!f) {
            continue;
        }
        size_t wrote = fwrite(json, 1, len, f);
        fclose(f);
        if (wrote == len) {
            snprintf(path_out, path_len, "%s", candidates[i]);
            return true;
        }
    }
    return false;
}

int claw_agent_ask(const char *prompt, bool verbose, claw_result_t *out)
{
    claw_result_t local;
    claw_result_t *res = out ? out : &local;
    memset(res, 0, sizeof(*res));
    res->status = -1;

    if (!network_ready()) {
        snprintf(res->error, sizeof(res->error),
                 "no network. Run: wifi connect <ssid> <password>");
        return 1;
    }

    char api_key[CLAW_CFG_MAX_VALUE];
    const char *keyerr = NULL;
    if (!claw_config_api_key(api_key, sizeof(api_key), &keyerr)) {
        snprintf(res->error, sizeof(res->error), "%s", keyerr ? keyerr : "no API key");
        return 1;
    }

    const claw_backend_t *backend = claw_backend_active();

    /* --- build the request ------------------------------------------------ */
    /* Record the user turn first, then replay: the new turn is simply the last
     * line of the transcript, so there is one code path rather than two. */
    if (!claw_session_append("user", prompt)) {
        snprintf(res->error, sizeof(res->error), "cannot write to session transcript");
        return 1;
    }

    size_t budget = (size_t)claw_config_get_int("context_budget", 6144);
    cJSON *messages = claw_session_replay(budget);
    if (!messages) {
        snprintf(res->error, sizeof(res->error), "out of memory building request");
        return 1;
    }
    res->turns = cJSON_GetArraySize(messages);

    cJSON *body = backend->build_body(messages);   /* takes ownership */
    if (!body) {
        snprintf(res->error, sizeof(res->error), "could not build request body");
        return 1;
    }
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) {
        snprintf(res->error, sizeof(res->error), "out of memory serialising request");
        return 1;
    }

    char req_path[64];
    bool staged = write_body(json, req_path, sizeof(req_path));
    int body_len = (int)strlen(json);
    cJSON_free(json);
    if (!staged) {
        snprintf(res->error, sizeof(res->error), "cannot write request file");
        return 1;
    }

    if (verbose) {
        heap_line("before");
    }
    int64_t t0 = esp_timer_get_time();

    /* --- send ------------------------------------------------------------- */
    char url[256];
    backend->endpoint(url, sizeof(url));

    char ca_path[128];
    resolve_ca(backend, ca_path, sizeof(ca_path));

    char *ca_pem = NULL;
    if (ca_path[0]) {
        FILE *cf = fopen(ca_path, "rb");
        if (cf) {
            fseek(cf, 0, SEEK_END);
            long n = ftell(cf);
            fseek(cf, 0, SEEK_SET);
            if (n > 0 && n < 8192) {
                ca_pem = malloc((size_t)n + 1);
                if (ca_pem) {
                    size_t got = fread(ca_pem, 1, (size_t)n, cf);
                    ca_pem[got] = '\0';
                }
            }
            fclose(cf);
        }
    }

    esp_http_client_config_t cfg = {
        .url                   = url,
        .method                = HTTP_METHOD_POST,
        .timeout_ms            = claw_config_get_int("timeout_ms", 60000),
        .buffer_size           = CLAW_IO_CHUNK,
        .buffer_size_tx        = CLAW_IO_CHUNK,
        .cert_pem              = ca_pem,
        .crt_bundle_attach     = ca_pem ? NULL : esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(ca_pem);
        snprintf(res->error, sizeof(res->error), "http client init failed");
        return 1;
    }
    backend->headers(client, api_key);

    stream_ctx_t sctx = { .backend = backend, .res = res };
    sctx.reply_cap = CLAW_TURN_MAX + 1;
    sctx.reply = calloc(1, sctx.reply_cap);
    if (!sctx.reply) {
        /* Not fatal: stream the reply, just do not remember it. */
        ESP_LOGW(TAG, "no memory for reply buffer; this turn will not be saved");
    }
    claw_sse_t parser;
    claw_sse_init(&parser, on_sse_event, &sctx);

    int rc = 1;
    if (esp_http_client_open(client, body_len) != ESP_OK) {
        snprintf(res->error, sizeof(res->error), "connect/open failed");
        goto done;
    }

    {   /* stream the body off disk rather than holding it in RAM */
        FILE *bf = fopen(req_path, "rb");
        if (!bf) {
            snprintf(res->error, sizeof(res->error), "cannot reopen request file");
            goto done;
        }
        char buf[CLAW_IO_CHUNK];
        size_t n;
        bool ok = true;
        while ((n = fread(buf, 1, sizeof(buf), bf)) > 0) {
            if (esp_http_client_write(client, buf, (int)n) != (int)n) {
                ok = false;
                break;
            }
        }
        fclose(bf);
        if (!ok) {
            snprintf(res->error, sizeof(res->error), "request write failed");
            goto done;
        }
    }

    if (esp_http_client_fetch_headers(client) < 0) {
        snprintf(res->error, sizeof(res->error), "fetch headers failed");
        goto done;
    }
    res->status = esp_http_client_get_status_code(client);
    sctx.in_error_body = (res->status < 200 || res->status >= 300);

    {
        char buf[CLAW_IO_CHUNK];
        int n;
        int reads = 0;
        /* Check completion before each read: a chunked keep-alive stream can be
         * fully delivered while the socket stays open, and another read would
         * then block for the whole timeout. */
        while (!esp_http_client_is_complete_data_received(client)) {
            n = esp_http_client_read(client, buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            res->chunks++;
            res->bytes += (size_t)n;

            if (sctx.in_error_body) {
                /* An HTTP error body is plain JSON, not SSE. Buffer a bounded
                 * amount so the API's own message can be reported. */
                size_t space = sizeof(sctx.error_body) - 1 - sctx.error_body_len;
                size_t copy = ((size_t)n < space) ? (size_t)n : space;
                memcpy(sctx.error_body + sctx.error_body_len, buf, copy);
                sctx.error_body_len += copy;
            } else {
                claw_sse_feed(&parser, buf, (size_t)n);
            }
            if ((++reads & 0x0F) == 0) {
                vTaskDelay(1);   /* feed the idle task; TWDT fires at 5 s */
            }
        }
        claw_sse_finish(&parser);
    }

    if (res->got_text) {
        printf("\n");
    }

    if (sctx.in_error_body) {
        sctx.error_body[sctx.error_body_len] = '\0';
        const char *msg = NULL;
        char errbuf[160];
        cJSON *obj = cJSON_Parse(sctx.error_body);
        if (obj) {
            msg = backend->extract_error(obj, errbuf, sizeof(errbuf));
        }
        if (msg) {
            snprintf(res->error, sizeof(res->error), "HTTP %d - %s", res->status, msg);
        } else if (sctx.error_body_len) {
            snprintf(res->error, sizeof(res->error), "HTTP %d - %.120s",
                     res->status, sctx.error_body);
        } else {
            snprintf(res->error, sizeof(res->error), "HTTP %d", res->status);
        }
        cJSON_Delete(obj);
    } else if (res->error[0] == '\0') {
        rc = 0;
    }

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(ca_pem);
    free(sctx.reply);

    res->elapsed_ms = (unsigned)((esp_timer_get_time() - t0) / 1000);

    if (verbose) {
        heap_line("after");
        printf("[%u chunks, %u events, %u bytes, %u ms]\n",
               (unsigned)res->chunks, (unsigned)res->events,
               (unsigned)res->bytes, res->elapsed_ms);
    }
    if (parser.truncated) {
        ESP_LOGW(TAG, "an SSE field exceeded its buffer and was truncated");
    }
    return rc;
}
