#include "claw_agent.h"
#include "claw_backend.h"
#include "claw_config.h"
#include "claw_memory.h"
#include "claw_prompt.h"
#include "claw_session.h"
#include "claw_sse.h"
#include "claw_text.h"
#include "claw_tools.h"
#include "claw_util.h"

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

/* Upstream esp-claw stops at 10. The cap exists so a model that keeps calling
 * tools cannot spend the user's money or the device's battery indefinitely. */
#define CLAW_MAX_TOOL_ROUNDS 8

/* Where the shipped package lives, for backend CA files. Mirrors the search
 * order used elsewhere so a working copy on the card wins. */
static const char *k_install_dirs[] = {
    "/sd/espclaw",
    "/sd/apps/espclaw",
    "/root/apps/espclaw",
};

typedef struct {
    const claw_backend_t *backend;
    claw_result_t *res;

    bool   in_error_body;     /* status was not 2xx: collect, do not parse */
    char   error_body[512];
    size_t error_body_len;

    /* The reply is echoed as it streams and also collected, so it can be
     * appended to the transcript. Bounded: a longer reply is shown in full but
     * stored truncated, which keeps the heap flat. */
    char  *reply;
    size_t reply_len;
    size_t reply_cap;

    /* One tool call per round. The loop runs again afterwards, which bounds
     * memory and keeps the ordering obvious. */
    bool   has_call;
    claw_tool_accum_t acc;
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

/*
 * Describe the network for an error message.
 *
 * "request write failed" with an association but a dead route looks identical
 * to a dozen other faults. Printing the address and gateway separates "no
 * lease" from "lease but nothing behind it", which need different fixes.
 */
static void network_detail(char *out, size_t out_len)
{
    esp_netif_t *n = esp_netif_get_default_netif();
    esp_netif_ip_info_t ip = {0};
    if (!n || esp_netif_get_ip_info(n, &ip) != ESP_OK || ip.ip.addr == 0) {
        snprintf(out, out_len, "no IP address");
        return;
    }
    snprintf(out, out_len, "ip " IPSTR ", gw " IPSTR, IP2STR(&ip.ip), IP2STR(&ip.gw));
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
        snprintf(out, n,
                 claw_text("agent.resolve_ca.msg",
                           "%s/%s"), k_install_dirs[i], b->ca_file);
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

    if (!ctx->has_call && ctx->backend->extract_tool_call) {
        if (ctx->backend->extract_tool_call(event, obj, &ctx->acc)) {
            ctx->has_call = true;
        }
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

/*
 * One request/response round.
 *
 * `messages` is borrowed. On return, *sctx holds the streamed reply and, if the
 * model asked for one, a pending tool call for the caller to run.
 */
static int claw_round(const claw_backend_t *backend, const cJSON *messages,
                      const char *api_key, bool verbose,
                      claw_result_t *res, stream_ctx_t *sctx,
                      const char *ca_pem, claw_sse_t *parser)
{
    cJSON *body = backend->build_body(messages);
    if (!body) {
        snprintf(res->error, sizeof(res->error),
                 claw_text("agent.write_body.could_build_request",
                           "could not build request body"));
        return 1;
    }
    if (backend->add_tools) {
        backend->add_tools(body);
    }

    /*
     * Print into the parser's buffer rather than allocating another one.
     *
     * The request body is the third large contiguous block a round needs,
     * after the parser and the streaming context, and asking for it once the
     * Lua interpreter has been through the heap is what fails. The parser is
     * idle here -- it is only used to read the response, which has not been
     * sent for yet -- so its 16 KB is free scratch that is already contiguous
     * and already ours.
     */
    char *json = NULL;
    bool owned = false;
    if (cJSON_PrintPreallocated(body, parser->data, CLAW_SSE_MAX_DATA, false)) {
        json = parser->data;
    } else {
        /* Bigger than the scratch buffer: fall back to allocating, which may
         * well work when the heap is fresh. */
        json = cJSON_PrintUnformatted(body);
        owned = true;
    }
    cJSON_Delete(body);
    if (!json) {
        const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        snprintf(res->error, sizeof(res->error),
                 "request too large to serialise (largest block %u). Try "
                 "/new, or lower context_budget.",
                 (unsigned)heap_caps_get_largest_free_block(caps));
        return 1;
    }

    char req_path[64];
    bool staged = write_body(json, req_path, sizeof(req_path));
    int body_len = (int)strlen(json);
    res->bytes_sent = (size_t)body_len;
    if (owned) {
        cJSON_free(json);
    }
    /* Leave no request bytes behind in the buffer the response will parse. */
    parser->data[0] = '\0';
    if (!staged) {
        snprintf(res->error, sizeof(res->error),
                 claw_text("agent.write_body.cannot_write_request",
                           "cannot write request file"));
        return 1;
    }

    char url[256];
    backend->endpoint(url, sizeof(url));

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
        snprintf(res->error, sizeof(res->error),
                 claw_text("agent.write_body.http_client_init",
                           "http client init failed"));
        return 1;
    }
    backend->headers(client, api_key);

    /* Allocated once per request by the caller: see the note there. */
    claw_sse_init(parser, on_sse_event, sctx);

    int rc = 1;
    const int64_t t_open = esp_timer_get_time();
    if (esp_http_client_open(client, body_len) != ESP_OK) {
        char net[64];
        network_detail(net, sizeof(net));
        snprintf(res->error, sizeof(res->error),
                 "could not reach %.60s (%s)", url, net);
        goto done;
    }

    {   /* stream the body off disk rather than holding it in RAM */
        FILE *bf = fopen(req_path, "rb");
        if (!bf) {
            snprintf(res->error, sizeof(res->error),
                     claw_text("agent.write_body.cannot_reopen_request",
                               "cannot reopen request file"));
            goto done;
        }
        char buf[CLAW_IO_CHUNK];
        size_t n;
        size_t sent = 0;
        bool ok = true;
        while ((n = fread(buf, 1, sizeof(buf), bf)) > 0) {
            int w = esp_http_client_write(client, buf, (int)n);
            if (w != (int)n) {
                ok = false;
                break;
            }
            sent += (size_t)w;
        }
        fclose(bf);
        if (!ok) {
            /* How far it got separates a connection that never came up from
             * one that died partway, which look identical otherwise. */
            snprintf(res->error, sizeof(res->error),
                     "sending the request stalled after %u of %d bytes "
                     "(%u ms). The TLS connection may not have completed.",
                     (unsigned)sent, body_len,
                     (unsigned)((esp_timer_get_time() - t_open) / 1000));
            goto done;
        }
    }

    if (esp_http_client_fetch_headers(client) < 0) {
        /* Almost always the read timeout expiring with no reply. The request
         * size matters here: it grows with every tool round, and a large one
         * takes the provider longer to answer. */
        snprintf(res->error, sizeof(res->error),
                 "no reply within %d s to a %d byte request. It grows with "
                 "each tool call -- try /new, or a lower context_budget.",
                 claw_config_get_int("timeout_ms", 60000) / 1000, body_len);
        goto done;
    }
    res->status = esp_http_client_get_status_code(client);
    sctx->in_error_body = (res->status < 200 || res->status >= 300);

    {
        char buf[CLAW_IO_CHUNK];
        int n;
        int reads = 0;
        /* Check completion before each read: a chunked keep-alive stream can be
         * fully delivered while the socket stays open, and another read would
         * then block for the whole timeout. */
        /*
         * Read first, then test for completion.
         *
         * fetch_headers buffers whatever arrived alongside the headers, so a
         * short response can already be complete before the first read -- and
         * testing completion first meant never reading it at all, returning
         * zero bytes with no error. Testing after a read still avoids the
         * blocking read past end-of-body that this check was added for.
         */
        while (true) {
            n = esp_http_client_read(client, buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            res->chunks++;
            res->bytes += (size_t)n;

            if (sctx->in_error_body) {
                size_t space = sizeof(sctx->error_body) - 1 - sctx->error_body_len;
                size_t copy = ((size_t)n < space) ? (size_t)n : space;
                memcpy(sctx->error_body + sctx->error_body_len, buf, copy);
                sctx->error_body_len += copy;
            } else {
                claw_sse_feed(parser, buf, (size_t)n);
            }
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            if ((++reads & 0x0F) == 0) {
                vTaskDelay(1);   /* feed the idle task; TWDT fires at 5 s */
            }
        }
        claw_sse_finish(parser);
    }

    if (sctx->in_error_body) {
        sctx->error_body[sctx->error_body_len] = '\0';
        const char *msg = NULL;
        char errbuf[160];
        cJSON *obj = cJSON_Parse(sctx->error_body);
        if (obj) {
            msg = backend->extract_error(obj, errbuf, sizeof(errbuf));
        }
        if (msg) {
            snprintf(res->error, sizeof(res->error),
                     claw_text("agent.write_body.http",
                               "HTTP %d - %s"), res->status, msg);
        } else if (sctx->error_body_len) {
            snprintf(res->error, sizeof(res->error),
                     claw_text("agent.write_body.http2",
                               "HTTP %d - %.120s"),
                     res->status, sctx->error_body);
        } else {
            snprintf(res->error, sizeof(res->error),
                     claw_text("agent.write_body.http3",
                               "HTTP %d"), res->status);
        }
        cJSON_Delete(obj);
    } else if (res->error[0] == '\0') {
        rc = 0;
    }

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (parser->truncated) {
        /* Truncation means an event was dropped, so the turn is incomplete.
         * Say so rather than leaving the user with "(no text in response)". */
        ESP_LOGW(TAG, "an SSE field exceeded its %d byte buffer", CLAW_SSE_MAX_DATA);
        if (res->error[0] == '\0') {
            snprintf(res->error, sizeof(res->error),
                     "the reply exceeded %d bytes in one message and could not "
                     "be parsed. Ask for a smaller first version, then extend it",
                     CLAW_SSE_MAX_DATA);
            rc = 1;
        }
    }
    (void)verbose;
    return rc;
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
        snprintf(res->error, sizeof(res->error),
                 claw_text("agent.agent_ask.msg",
                           "%s"), keyerr ? keyerr : "no API key");
        return 1;
    }

    const claw_backend_t *backend = claw_backend_active();

    /* Record the user turn first, then replay: the new turn is simply the last
     * line of the transcript, so there is one code path rather than two. */
    /* Remember where the transcript ended, so a request that never produces an
     * answer can be undone rather than left dangling. */
    const long session_mark = claw_session_mark();

    if (!claw_session_append("user", prompt)) {
        snprintf(res->error, sizeof(res->error),
                 claw_text("agent.agent_ask.cannot_write_session",
                           "cannot write to session transcript"));
        return 1;
    }

    size_t budget = (size_t)claw_config_get_int("context_budget", 6144);
    cJSON *messages = claw_session_replay(budget);
    if (!messages) {
        snprintf(res->error, sizeof(res->error),
                 claw_text("agent.agent_ask.out_memory_building",
                           "out of memory building request"));
        return 1;
    }
    res->turns = cJSON_GetArraySize(messages);

    /*
     * Prepend two system turns, memory first so the device description ends up
     * ahead of it: what this machine is, then what it remembers. Only the
     * memory index goes in -- bodies are fetched with memory_read when the
     * model decides one is relevant. Each backend puts a system turn where its
     * API expects it.
     */
    {
        char *mem = malloc(CLAW_MEMORY_INJECT_MAX);
        if (mem) {
            if (claw_memory_context(mem, CLAW_MEMORY_INJECT_MAX) > 0) {
                cJSON *sys = cJSON_CreateObject();
                if (sys) {
                    cJSON_AddStringToObject(sys, "role", "system");
                    cJSON_AddStringToObject(sys, "content", mem);
                    if (!cJSON_InsertItemInArray(messages, 0, sys)) {
                        cJSON_Delete(sys);
                    }
                }
            }
            free(mem);
        }
    }
    {
        char *prompt = malloc(CLAW_PROMPT_MAX);
        if (prompt) {
            if (claw_prompt_build(prompt, CLAW_PROMPT_MAX) > 0) {
                cJSON *sys = cJSON_CreateObject();
                if (sys) {
                    cJSON_AddStringToObject(sys, "role", "system");
                    cJSON_AddStringToObject(sys, "content", prompt);
                    if (!cJSON_InsertItemInArray(messages, 0, sys)) {
                        cJSON_Delete(sys);
                    }
                }
            }
            free(prompt);
        }
    }

    if (verbose) {
        heap_line("before");
    }
    int64_t t0 = esp_timer_get_time();

    /* Read the pinned CA once, not once per tool round: it is a 2 KB SD read
     * for a file that does not change, and a request can take eight rounds. */
    char ca_path[128];
    resolve_ca(backend, ca_path, sizeof(ca_path));
    char *ca_pem = ca_path[0] ? claw_read_file(ca_path, 8192, NULL) : NULL;

    char *reply = calloc(1, CLAW_TURN_MAX + 1);
    if (!reply) {
        ESP_LOGW(TAG, "no memory for reply buffer; this turn will not be saved");
    }

    int rc = 1;
    char *tool_out = NULL;

    /* Signature of the previous tool call. A model that gets an unusable result
     * tends to retry the identical call; catching that turns a silent eight
     * round burn into one clear message. */
    char     last_error[160] = {0};
    char     last_name[64] = {0};
    uint32_t last_hash = 0;
    bool     have_last = false;

    /* Heap, not stack: this carries an 8 KB tool-argument buffer, which the
     * console task's 16 KB stack cannot spare. */
    /*
     * Allocate the two large buffers once and hold them for the whole request.
     *
     * They were allocated per round, and freed around each tool call to leave
     * room for it. That failed: run_lua loads the Lua interpreter, and after it
     * the heap is too fragmented to get 18.5 KB back -- "could not allocate the
     * stream parser" partway through a working conversation. Holding them costs
     * a tool some memory but cannot fail mid-loop, which is the better trade:
     * a tool that is short of memory says so and the model adapts, whereas a
     * failed parser allocation ends the whole request.
     */
    claw_sse_t *parser = calloc(1, sizeof(*parser));
    if (!parser) {
        const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        free(ca_pem);
        free(reply);
        cJSON_Delete(messages);
        snprintf(res->error, sizeof(res->error),
                 "could not allocate the %u byte stream parser "
                 "(free %u, largest block %u)",
                 (unsigned)sizeof(*parser),
                 (unsigned)heap_caps_get_free_size(caps),
                 (unsigned)heap_caps_get_largest_free_block(caps));
        return 1;
    }

    stream_ctx_t *sctxp = calloc(1, sizeof(*sctxp));
    if (!sctxp) {
        free(parser);
        free(ca_pem);
        free(reply);
        cJSON_Delete(messages);
        snprintf(res->error, sizeof(res->error),
                 claw_text("agent.agent_ask.out_memory_starting",
                           "out of memory starting the request"));
        return 1;
    }

    for (int round = 0; round < CLAW_MAX_TOOL_ROUNDS; round++) {
        memset(sctxp, 0, sizeof(*sctxp));
        sctxp->backend   = backend;
        sctxp->res       = res;
        sctxp->reply     = reply;
        sctxp->reply_cap = CLAW_TURN_MAX + 1;
        /* Carry the accumulated reply across rounds: a model may narrate before
         * and after a tool call, and both halves belong to one answer. */
        sctxp->reply_len = reply ? strlen(reply) : 0;
        stream_ctx_t *sctx = sctxp;

        rc = claw_round(backend, messages, api_key, verbose, res, sctx, ca_pem, parser);

        if (rc != 0 || !sctx->has_call) {
            break;
        }

        /* --- run the tool ------------------------------------------------- */
        res->tool_calls++;

        if (sctx->acc.overflow) {
            printf("\n[tool arguments too large for %s]\n", sctx->acc.name);
            snprintf(res->error, sizeof(res->error),
                     "%s was called with arguments larger than %d bytes",
                     sctx->acc.name, CLAW_TOOL_ARGS_MAX);
            rc = 1;
            break;
        }

        /* Fragments are only valid JSON once the call is complete. */
        cJSON *call_args = cJSON_Parse(sctx->acc.json_len ? sctx->acc.json : "{}");
        if (!call_args) {
            call_args = cJSON_CreateObject();
        }

        /*
         * Copy the call out and release the streaming context before running
         * the tool.
         *
         * stream_ctx_t carries a 16 KB argument accumulator that is only needed
         * while a response is streaming. Holding it during dispatch left a tool
         * unable to get a large contiguous block -- which is why a Lua script
         * could not enter graphics mode from the agent while the same script
         * runs fine from the shell. The parsed arguments are all that is needed
         * from here.
         */
        char call_name[sizeof(sctx->acc.name)];
        char call_id[sizeof(sctx->acc.id)];
        snprintf(call_name, sizeof(call_name), "%s", sctx->acc.name);
        snprintf(call_id, sizeof(call_id), "%s", sctx->acc.id);


        if (!tool_out) {
            tool_out = malloc(CLAW_TOOL_RESULT_MAX);
            if (!tool_out) {
                snprintf(res->error, sizeof(res->error),
                         claw_text("agent.agent_ask.out_memory_tool",
                                   "out of memory for tool output"));
                cJSON_Delete(call_args);
                    rc = 1;
                break;
            }
        }

        /*
         * Hash the whole argument string, not a prefix of it.
         *
         * run_lua arguments open with a long `code` string, so comparing the
         * first 180 bytes made two different scripts that share their opening
         * lines look identical -- and a model correcting its own mistake got
         * stopped for repeating itself. FNV-1a over the full arguments.
         */
        char *argstr = cJSON_PrintUnformatted(call_args);
        uint32_t hash = 2166136261u;
        for (const char *c = argstr ? argstr : ""; *c; c++) {
            hash = (hash ^ (uint8_t)*c) * 16777619u;
        }
        if (argstr) {
            cJSON_free(argstr);
        }
        if (have_last && hash == last_hash &&
            strcmp(call_name, last_name) == 0) {
            printf("\n[stopped: %s called twice with the same arguments]\n",
                   call_name);
            snprintf(res->error, sizeof(res->error),
                     "the model repeated the same %s call; its result was probably "
                     "not reaching it", call_name);
            cJSON_Delete(call_args);
            rc = 1;
            break;
        }
        snprintf(last_name, sizeof(last_name), "%s", call_name);
        last_hash = hash;
        have_last = true;

        printf("\n[tool: %s  (%d/%d)]\n", call_name,
               round + 1, CLAW_MAX_TOOL_ROUNDS);
        if (verbose) {
            printf("  request was %u bytes\n", (unsigned)res->bytes_sent);
        }
        bool tool_ok = claw_tools_run(call_name, call_args, tool_out,
                                      CLAW_TOOL_RESULT_MAX);
        /*
         * Show the first line of a failure even without -v. Several rounds of
         * "[tool: run_lua]" with nothing between them is indistinguishable from
         * a hang, when what is actually happening is the model retrying against
         * an error the user cannot see.
         */
        if (!tool_ok || strncmp(tool_out, "SCRIPT FAILED", 13) == 0) {
            /*
             * Show the line that says what went wrong, not the banner above it.
             * "SCRIPT FAILED - it was saved..." is itself the first line, so
             * printing the first line printed only the banner and hid the
             * error -- which is the one thing worth seeing.
             */
            const char *msg = tool_out;
            if (strncmp(msg, "SCRIPT FAILED", 13) == 0) {
                const char *nl = strchr(msg, '\n');
                if (nl) {
                    msg = nl + 1;
                }
            }
            const char *nl = strchr(msg, '\n');
            int n = nl ? (int)(nl - msg) : (int)strlen(msg);
            if (n > 110) {
                n = 110;
            }
            printf("  -> %.*s\n", n, msg);
            snprintf(last_error, sizeof(last_error), "%.*s", n, msg);
        }
        if (verbose) {
            printf("[result: %.120s%s]\n", tool_out,
                   strlen(tool_out) > 120 ? "..." : "");
        }

        if (backend->append_tool_result) {
            backend->append_tool_result(messages, call_name, call_id,
                                        call_args, tool_out);
        }
        cJSON_Delete(call_args);

        if (round == CLAW_MAX_TOOL_ROUNDS - 1) {
            /*
             * Ending here means the model never produced an answer, so say why
             * rather than leaving "(no text in response)". The last failure is
             * almost always the reason it kept trying.
             */
            printf("\n[stopped: reached the %d tool-call limit]\n",
                   CLAW_MAX_TOOL_ROUNDS);
            if (last_error[0]) {
                snprintf(res->error, sizeof(res->error),
                         "gave up after %d tool calls; last failure: %.100s",
                         CLAW_MAX_TOOL_ROUNDS, last_error);
            } else {
                snprintf(res->error, sizeof(res->error),
                         "gave up after %d tool calls without reaching an answer",
                         CLAW_MAX_TOOL_ROUNDS);
            }
            rc = 1;
        }
    }

    if (res->got_text) {
        printf("\n");
    }

    if (rc == 0 && reply && reply[0]) {
        claw_session_append("assistant", reply);
    } else {
        /* No answer, so drop the question too: leaving it would put two user
         * turns in a row on the next request. */
        claw_session_rollback(session_mark);
    }

    free(sctxp);
    free(parser);
    free(ca_pem);
    free(reply);
    free(tool_out);
    cJSON_Delete(messages);

    res->elapsed_ms = (unsigned)((esp_timer_get_time() - t0) / 1000);

    if (verbose) {
        heap_line("after");
        printf("[%u chunks, %u events, %u bytes, %u ms, %d turns, %u tools]\n",
               (unsigned)res->chunks, (unsigned)res->events,
               (unsigned)res->bytes, res->elapsed_ms, res->turns,
               (unsigned)res->tool_calls);
    }
    return rc;
}
