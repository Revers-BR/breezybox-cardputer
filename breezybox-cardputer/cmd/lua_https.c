/*
 * lua_https.c - breezy.https: streaming TLS client for Lua.
 *
 * The older breezy.network.http_* helpers are hand-rolled over raw TCP and
 * refuse https:// outright, so no TLS endpoint was reachable from Lua. This
 * binding wraps esp_http_client with the ESP-IDF certificate bundle attached.
 *
 * Two properties matter for a PSRAM-less board, and both exist to keep peak
 * RAM independent of message size:
 *
 *   body_file  the request body is streamed off the filesystem in small
 *              chunks with a pre-computed Content-Length, so a long
 *              conversation never becomes one large Lua string.
 *   on_chunk   the response is delivered incrementally and never buffered.
 *
 * on_status is also provided: it fires once the response headers are in, before
 * any body arrives, so the caller can decide whether to stream-parse the body
 * or buffer it as an error message.
 *
 * Note CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY is enabled globally for the SSH
 * client. Attaching the bundle explicitly is what makes traffic here actually
 * verified; do not drop it.
 */

#include "breezy_cmd.h"
#include "breezy_vfs.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lauxlib.h"
#include "lua.h"

#include <stdio.h>
#include <string.h>

#define HTTPS_IO_CHUNK      512    /* body write / response read granularity */
#define HTTPS_DEFAULT_TMO   120000

typedef struct {
    lua_State *L;
    int        on_chunk_ref;   /* LUA_NOREF when no callback was supplied */
    int        aborted;        /* callback returned false */
    int        lua_error;      /* callback raised */
    size_t     total;
} https_stream_ctx_t;

static int https_network_ready(void)
{
    esp_netif_t *netif = esp_netif_get_default_netif();
    if (!netif) {
        return 0;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK) {
        return 0;
    }
    return ip.ip.addr != 0;
}

/* Push each response chunk to the Lua callback. Returns 0 to keep going. */
static int https_emit(https_stream_ctx_t *ctx, const char *data, int len)
{
    ctx->total += (size_t)len;

    if (ctx->on_chunk_ref == LUA_NOREF) {
        return 0;
    }

    lua_State *L = ctx->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->on_chunk_ref);
    lua_pushlstring(L, data, (size_t)len);

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        ctx->lua_error = 1;
        return -1;   /* leave the message on the stack for the caller */
    }

    /* Only an explicit `false` aborts; returning nothing continues. */
    if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
        ctx->aborted = 1;
        lua_pop(L, 1);
        return -1;
    }
    lua_pop(L, 1);
    return 0;
}

static void https_apply_headers(lua_State *L, int tbl_idx, esp_http_client_handle_t client)
{
    lua_getfield(L, tbl_idx, "headers");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        /* key at -2, value at -1; only accept string/string */
        if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TSTRING) {
            esp_http_client_set_header(client, lua_tostring(L, -2), lua_tostring(L, -1));
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

/*
 * breezy.https.request{ url=, method=, headers=, body=|body_file=,
 *                       ca_file=|ca_pem=, on_status=, on_chunk=, timeout_ms= }
 *
 * ca_file / ca_pem pin a specific root instead of using the IDF certificate
 * bundle. This is needed for hosts served from a cross-signed chain: the
 * bundle looks up a root by issuer name, takes the first match, and gives up
 * if the signature does not verify (esp_crt_bundle.c, "Certificate matched but
 * signature verification failed") -- it never tries the other entries sharing
 * that name. Google is the case in point: its chain ends with a GTS Root R1
 * cross-signed by GlobalSign, and the bundle holds several GlobalSign entries.
 *   -> status, bytes        on success
 *   -> nil, "message"       on failure
 */
static int l_https_request(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "url");
    const char *url = luaL_checkstring(L, -1);
    lua_pop(L, 1);

    if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "url must start with https:// or http://");
        return 2;
    }

    lua_getfield(L, 1, "method");
    const char *method = lua_isstring(L, -1) ? lua_tostring(L, -1) : "GET";
    lua_pop(L, 1);

    lua_getfield(L, 1, "timeout_ms");
    int timeout_ms = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : HTTPS_DEFAULT_TMO;
    lua_pop(L, 1);

    /* Body: either an inline string or a file streamed from disk. */
    const char *body = NULL;
    size_t body_len = 0;
    lua_getfield(L, 1, "body");
    if (lua_isstring(L, -1)) {
        body = lua_tolstring(L, -1, &body_len);
    }
    /* leave `body` on the stack so the string stays alive for the call */

    char body_path[BREEZYBOX_MAX_PATH * 2];
    FILE *body_fp = NULL;
    long body_file_len = 0;
    lua_getfield(L, 1, "body_file");
    if (lua_isstring(L, -1)) {
        const char *raw = lua_tostring(L, -1);
        if (!breezybox_resolve_path(raw, body_path, sizeof(body_path))) {
            lua_pushnil(L);
            lua_pushfstring(L, "cannot resolve body_file: %s", raw);
            return 2;
        }
        body_fp = fopen(body_path, "rb");
        if (!body_fp) {
            lua_pushnil(L);
            lua_pushfstring(L, "cannot open body_file: %s", body_path);
            return 2;
        }
        fseek(body_fp, 0, SEEK_END);
        body_file_len = ftell(body_fp);
        fseek(body_fp, 0, SEEK_SET);
        if (body_file_len < 0) {
            fclose(body_fp);
            lua_pushnil(L);
            lua_pushstring(L, "cannot size body_file");
            return 2;
        }
    }
    lua_pop(L, 1);   /* body_file */

    if (!https_network_ready()) {
        if (body_fp) fclose(body_fp);
        lua_pushnil(L);
        lua_pushstring(L, "no network");
        return 2;
    }

    https_stream_ctx_t ctx = {
        .L = L,
        .on_chunk_ref = LUA_NOREF,
        .aborted = 0,
        .lua_error = 0,
        .total = 0,
    };
    lua_getfield(L, 1, "on_chunk");
    if (lua_isfunction(L, -1)) {
        ctx.on_chunk_ref = luaL_ref(L, LUA_REGISTRYINDEX);   /* pops it */
    } else {
        lua_pop(L, 1);
    }

    int on_status_ref = LUA_NOREF;
    lua_getfield(L, 1, "on_status");
    if (lua_isfunction(L, -1)) {
        on_status_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        lua_pop(L, 1);
    }

    /* Optional explicit CA: inline PEM, or a PEM file on disk. */
    char *ca_buf = NULL;
    const char *ca_pem = NULL;

    lua_getfield(L, 1, "ca_pem");
    if (lua_isstring(L, -1)) {
        ca_pem = lua_tostring(L, -1);   /* stays on the stack for the call */
    } else {
        lua_pop(L, 1);

        lua_getfield(L, 1, "ca_file");
        if (lua_isstring(L, -1)) {
            char ca_path[BREEZYBOX_MAX_PATH * 2];
            const char *raw = lua_tostring(L, -1);
            if (!breezybox_resolve_path(raw, ca_path, sizeof(ca_path))) {
                lua_pop(L, 1);
                if (body_fp) fclose(body_fp);
                lua_pushnil(L);
                lua_pushfstring(L, "cannot resolve ca_file: %s", raw);
                return 2;
            }
            FILE *cf = fopen(ca_path, "rb");
            if (!cf) {
                lua_pop(L, 1);
                if (body_fp) fclose(body_fp);
                lua_pushnil(L);
                lua_pushfstring(L, "cannot open ca_file: %s", ca_path);
                return 2;
            }
            fseek(cf, 0, SEEK_END);
            long ca_len = ftell(cf);
            fseek(cf, 0, SEEK_SET);
            if (ca_len > 0 && ca_len < 32768) {
                ca_buf = malloc((size_t)ca_len + 1);
            }
            if (!ca_buf) {
                fclose(cf);
                lua_pop(L, 1);
                if (body_fp) fclose(body_fp);
                lua_pushnil(L);
                lua_pushstring(L, "ca_file too large or out of memory");
                return 2;
            }
            size_t got = fread(ca_buf, 1, (size_t)ca_len, cf);
            fclose(cf);
            ca_buf[got] = '\0';        /* esp_http_client wants it NUL-terminated */
            ca_pem = ca_buf;
        }
        lua_pop(L, 1);
    }

    esp_http_client_config_t config = {
        .url                   = url,
        .timeout_ms            = timeout_ms,
        .buffer_size           = HTTPS_IO_CHUNK,
        .buffer_size_tx        = HTTPS_IO_CHUNK,
        /* An explicit CA wins; otherwise fall back to the IDF bundle. */
        .cert_pem              = ca_pem,
        .crt_bundle_attach     = ca_pem ? NULL : esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        if (body_fp) fclose(body_fp);
        free(ca_buf);
        luaL_unref(L, LUA_REGISTRYINDEX, ctx.on_chunk_ref);
        luaL_unref(L, LUA_REGISTRYINDEX, on_status_ref);
        lua_pushnil(L);
        lua_pushstring(L, "http client init failed");
        return 2;
    }

    esp_http_client_method_t m = HTTP_METHOD_GET;
    if      (strcasecmp(method, "POST")   == 0) m = HTTP_METHOD_POST;
    else if (strcasecmp(method, "PUT")    == 0) m = HTTP_METHOD_PUT;
    else if (strcasecmp(method, "DELETE") == 0) m = HTTP_METHOD_DELETE;
    else if (strcasecmp(method, "HEAD")   == 0) m = HTTP_METHOD_HEAD;
    esp_http_client_set_method(client, m);

    https_apply_headers(L, 1, client);
    /* Some hosts vary their response on User-Agent and send nothing useful
     * without one. Only a default -- an explicit header above wins. */
    esp_http_client_set_header(client, "User-Agent", "breezybox/claw");

    const int content_len = body_fp ? (int)body_file_len : (int)body_len;
    const char *err_msg = NULL;
    int status = -1;

    if (esp_http_client_open(client, content_len) != ESP_OK) {
        err_msg = "connect/open failed";
        goto done;
    }

    if (body_fp) {
        char wbuf[HTTPS_IO_CHUNK];
        size_t n;
        while ((n = fread(wbuf, 1, sizeof(wbuf), body_fp)) > 0) {
            if (esp_http_client_write(client, wbuf, (int)n) != (int)n) {
                err_msg = "body_file write failed";
                goto done;
            }
        }
    } else if (body && body_len > 0) {
        size_t off = 0;
        while (off < body_len) {
            int want = (int)((body_len - off) > HTTPS_IO_CHUNK ? HTTPS_IO_CHUNK : (body_len - off));
            int wrote = esp_http_client_write(client, body + off, want);
            if (wrote != want) {
                err_msg = "body write failed";
                goto done;
            }
            off += (size_t)wrote;
        }
    }

    if (esp_http_client_fetch_headers(client) < 0) {
        err_msg = "fetch headers failed";
        goto done;
    }
    status = esp_http_client_get_status_code(client);

    /*
     * Follow redirects ourselves.
     *
     * esp_http_client only auto-follows inside esp_http_client_perform(); this
     * streams with open/fetch_headers/read, where a 3xx simply arrives with an
     * empty body. Plenty of ordinary URLs redirect -- http to https, a bare
     * host to a path -- and "0 bytes, no error" is an unhelpful way to learn
     * that.
     *
     * Only bodyless requests are retried: re-sending a body would mean
     * rewinding it, and a redirected POST is ambiguous anyway. A redirect with
     * a body is reported instead.
     */
    for (int hop = 0; hop < 4; hop++) {
        if (status != 301 && status != 302 && status != 303 &&
            status != 307 && status != 308) {
            break;
        }
        char *location = NULL;
        if (esp_http_client_get_header(client, "Location", &location) != ESP_OK ||
            !location || !location[0]) {
            break;                       /* nowhere to go; report the 3xx */
        }
        if (content_len > 0 && status != 303) {
            err_msg = "redirected, but the request has a body; retry with the new URL";
            goto done;
        }

        if (esp_http_client_set_url(client, location) != ESP_OK) {
            err_msg = "cannot follow redirect";
            goto done;
        }
        /* 303 means "fetch that with GET"; the others keep the method, and by
         * here we know there is no body. */
        esp_http_client_set_method(client, HTTP_METHOD_GET);
        esp_http_client_close(client);

        if (esp_http_client_open(client, 0) != ESP_OK) {
            err_msg = "cannot open redirect target";
            goto done;
        }
        if (esp_http_client_fetch_headers(client) < 0) {
            err_msg = "fetch headers failed after redirect";
            goto done;
        }
        status = esp_http_client_get_status_code(client);
    }

    if (on_status_ref != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, on_status_ref);
        lua_pushinteger(L, status);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            ctx.lua_error = 1;
            goto done;
        }
    }

    {
        char rbuf[HTTPS_IO_CHUNK];
        int n;
        int reads = 0;
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
            n = esp_http_client_read(client, rbuf, sizeof(rbuf));
            if (n <= 0) {
                break;   /* connection closed, or error */
            }
            if (https_emit(&ctx, rbuf, n) != 0) {
                break;   /* aborted by callback, or the callback raised */
            }
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            /* Yield periodically so the idle task still runs: the task
             * watchdog here fires at 5 s and watches both idle tasks. */
            if ((++reads & 0x0F) == 0) {
                vTaskDelay(1);
            }
        }
    }

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (body_fp) {
        fclose(body_fp);
    }
    free(ca_buf);
    luaL_unref(L, LUA_REGISTRYINDEX, ctx.on_chunk_ref);
    luaL_unref(L, LUA_REGISTRYINDEX, on_status_ref);

    if (ctx.lua_error) {
        return lua_error(L);   /* re-raise the callback's error */
    }
    if (err_msg) {
        lua_pushnil(L);
        lua_pushstring(L, err_msg);
        return 2;
    }

    lua_pushinteger(L, status);
    lua_pushinteger(L, (lua_Integer)ctx.total);
    return 2;
}

const luaL_Reg *breezy_lua_https_lib(void)
{
    static const luaL_Reg lib[] = {
        { "request", l_https_request },
        { NULL, NULL },
    };
    return lib;
}
