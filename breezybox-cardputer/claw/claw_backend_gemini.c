/*
 * Google Gemini (generativelanguage).
 *
 * Differs from the other two in ways that all matter:
 *   - the model is in the URL path, not the body
 *   - auth is the x-goog-api-key header
 *   - turns are `contents` with `parts`, not `messages` with `content`
 *   - the assistant role is called "model"
 *   - ?alt=sse is required, or the response is a JSON array rather than a stream
 *   - the chain needs a pinned root (see ca/README.md)
 */
#include "claw_backend.h"
#include "claw_config.h"

#include <stdio.h>
#include <string.h>

static void ep(char *out, size_t n)
{
    char base[192];
    claw_config_get("base_url", base, sizeof(base), "");
    if (base[0]) {
        snprintf(out, n, "%s", base);
        return;
    }
    char model[64];
    claw_backend_model(&claw_backend_gemini, model, sizeof(model));
    snprintf(out, n,
             "https://generativelanguage.googleapis.com/v1beta/models/%s"
             ":streamGenerateContent?alt=sse", model);
}

static void hdrs(esp_http_client_handle_t c, const char *key)
{
    esp_http_client_set_header(c, "content-type", "application/json");
    esp_http_client_set_header(c, "x-goog-api-key", key);
}

/* {role, content} -> contents/parts. System turns are hoisted into
 * systemInstruction, which is where Gemini wants them. */
static cJSON *body(cJSON *messages)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *contents = cJSON_CreateArray();
    if (!root || !contents) {
        cJSON_Delete(root);
        cJSON_Delete(contents);
        cJSON_Delete(messages);
        return NULL;
    }

    cJSON *sys_text = NULL;
    cJSON *m = NULL;
    cJSON_ArrayForEach(m, messages) {
        cJSON *role = cJSON_GetObjectItemCaseSensitive(m, "role");
        cJSON *content = cJSON_GetObjectItemCaseSensitive(m, "content");
        const char *r = cJSON_IsString(role) ? role->valuestring : "user";
        const char *c = cJSON_IsString(content) ? content->valuestring : "";

        if (strcmp(r, "system") == 0) {
            if (!sys_text) {
                sys_text = cJSON_CreateString(c);
            }
            continue;
        }

        cJSON *turn = cJSON_CreateObject();
        cJSON *parts = cJSON_CreateArray();
        cJSON *part = cJSON_CreateObject();
        if (!turn || !parts || !part) {
            cJSON_Delete(turn);
            cJSON_Delete(parts);
            cJSON_Delete(part);
            continue;
        }
        cJSON_AddStringToObject(part, "text", c);
        cJSON_AddItemToArray(parts, part);
        cJSON_AddStringToObject(turn, "role", strcmp(r, "assistant") == 0 ? "model" : "user");
        cJSON_AddItemToObject(turn, "parts", parts);
        cJSON_AddItemToArray(contents, turn);
    }
    cJSON_Delete(messages);   /* converted, not adopted */

    cJSON_AddItemToObject(root, "contents", contents);

    cJSON *gen = cJSON_CreateObject();
    if (gen) {
        cJSON_AddNumberToObject(gen, "maxOutputTokens", claw_config_get_int("max_tokens", 2048));
        cJSON_AddItemToObject(root, "generationConfig", gen);
    }

    if (sys_text) {
        cJSON *si = cJSON_CreateObject();
        cJSON *parts = cJSON_CreateArray();
        cJSON *part = cJSON_CreateObject();
        if (si && parts && part) {
            cJSON_AddItemToObject(part, "text", sys_text);
            cJSON_AddItemToArray(parts, part);
            cJSON_AddItemToObject(si, "parts", parts);
            cJSON_AddItemToObject(root, "systemInstruction", si);
        } else {
            cJSON_Delete(si);
            cJSON_Delete(parts);
            cJSON_Delete(part);
            cJSON_Delete(sys_text);
        }
    }
    return root;
}

/* A chunk can carry several parts; return the first with text. The agent calls
 * this per event, so returning one pointer is enough in practice. */
static const char *text(const char *event, cJSON *obj)
{
    (void)event;
    cJSON *cands = cJSON_GetObjectItemCaseSensitive(obj, "candidates");
    cJSON *first = cJSON_IsArray(cands) ? cJSON_GetArrayItem(cands, 0) : NULL;
    if (!first) {
        return NULL;
    }
    cJSON *content = cJSON_GetObjectItemCaseSensitive(first, "content");
    if (!content) {
        return NULL;
    }
    cJSON *parts = cJSON_GetObjectItemCaseSensitive(content, "parts");
    cJSON *part = NULL;
    cJSON_ArrayForEach(part, parts) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(part, "text");
        if (cJSON_IsString(t) && t->valuestring[0]) {
            return t->valuestring;
        }
    }
    return NULL;
}

static const char *err(cJSON *obj, char *buf, size_t n)
{
    cJSON *e = cJSON_GetObjectItemCaseSensitive(obj, "error");
    if (!cJSON_IsObject(e)) {
        return NULL;
    }
    cJSON *st = cJSON_GetObjectItemCaseSensitive(e, "status");
    cJSON *code = cJSON_GetObjectItemCaseSensitive(e, "code");
    cJSON *m = cJSON_GetObjectItemCaseSensitive(e, "message");
    char status[32];
    if (cJSON_IsString(st)) {
        snprintf(status, sizeof(status), "%s", st->valuestring);
    } else if (cJSON_IsNumber(code)) {
        snprintf(status, sizeof(status), "%d", (int)code->valuedouble);
    } else {
        snprintf(status, sizeof(status), "error");
    }
    snprintf(buf, n, "%s: %s", status, cJSON_IsString(m) ? m->valuestring : "unknown");
    return buf;
}

const claw_backend_t claw_backend_gemini = {
    .name          = "gemini",
    .default_model = "gemini-2.5-flash",
    .ca_file       = "ca/gts_root_r1.pem",
    .endpoint      = ep,
    .headers       = hdrs,
    .build_body    = body,
    .extract_text  = text,
    .extract_error = err,
};
