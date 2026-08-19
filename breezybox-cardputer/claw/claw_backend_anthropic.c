/* Anthropic Messages API. Named SSE events; text at delta.text. */
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
    snprintf(out, n, "https://api.anthropic.com/v1/messages");
}

static void hdrs(esp_http_client_handle_t c, const char *key)
{
    esp_http_client_set_header(c, "content-type", "application/json");
    esp_http_client_set_header(c, "anthropic-version", "2023-06-01");
    esp_http_client_set_header(c, "x-api-key", key);
}

static cJSON *body(const cJSON *messages)
{
    char model[64];
    claw_backend_model(&claw_backend_anthropic, model, sizeof(model));

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddNumberToObject(root, "max_tokens", claw_config_get_int("max_tokens", 2048));
    cJSON_AddBoolToObject(root, "stream", true);
    cJSON_AddItemToObject(root, "messages", cJSON_Duplicate(messages, true));
    return root;
}

static const char *text(const char *event, cJSON *obj)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(obj, "type");
    bool is_delta = (event && strcmp(event, "content_block_delta") == 0) ||
                    (cJSON_IsString(type) && strcmp(type->valuestring, "content_block_delta") == 0);
    if (!is_delta) {
        return NULL;
    }
    cJSON *d = cJSON_GetObjectItemCaseSensitive(obj, "delta");
    if (!d) {
        return NULL;
    }
    cJSON *dt = cJSON_GetObjectItemCaseSensitive(d, "type");
    if (!cJSON_IsString(dt) || strcmp(dt->valuestring, "text_delta") != 0) {
        return NULL;
    }
    cJSON *t = cJSON_GetObjectItemCaseSensitive(d, "text");
    return cJSON_IsString(t) ? t->valuestring : NULL;
}

static const char *err(cJSON *obj, char *buf, size_t n)
{
    cJSON *e = cJSON_GetObjectItemCaseSensitive(obj, "error");
    if (!cJSON_IsObject(e)) {
        return NULL;
    }
    cJSON *t = cJSON_GetObjectItemCaseSensitive(e, "type");
    cJSON *m = cJSON_GetObjectItemCaseSensitive(e, "message");
    snprintf(buf, n, "%s: %s",
             cJSON_IsString(t) ? t->valuestring : "error",
             cJSON_IsString(m) ? m->valuestring : "unknown");
    return buf;
}

const claw_backend_t claw_backend_anthropic = {
    .name          = "anthropic",
    .default_model = "claude-sonnet-5",
    .ca_file       = NULL,
    .endpoint      = ep,
    .headers       = hdrs,
    .build_body    = body,
    .extract_text  = text,
    .extract_error = err,
};
