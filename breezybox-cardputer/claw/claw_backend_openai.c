/* OpenAI-compatible /v1/chat/completions. Also covers OpenRouter, Ollama and
 * llama.cpp via base_url. Unnamed SSE events terminated by `data: [DONE]`. */
#include "claw_backend.h"
#include "claw_config.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

static void ep(char *out, size_t n)
{
    char base[192];
    claw_config_get("base_url", base, sizeof(base), "");
    if (base[0]) {
        snprintf(out, n, "%s", base);
        return;
    }
    snprintf(out, n, "https://api.openai.com/v1/chat/completions");
}

static void hdrs(esp_http_client_handle_t c, const char *key)
{
    char auth[CLAW_CFG_MAX_VALUE + 8];
    snprintf(auth, sizeof(auth), "Bearer %s", key);
    esp_http_client_set_header(c, "content-type", "application/json");
    esp_http_client_set_header(c, "authorization", auth);
}

/* Reasoning-era models reject `max_tokens` and want `max_completion_tokens`.
 * Guessing from the name avoids a confusing 400 on a first run. */
static bool wants_max_completion(const char *model)
{
    if (!model) {
        return false;
    }
    if (model[0] == 'o' && isdigit((unsigned char)model[1])) {
        return true;                       /* o1, o3, o4... */
    }
    return strncmp(model, "gpt-5", 5) == 0;
}

static cJSON *body(const cJSON *messages)
{
    char model[64];
    claw_backend_model(&claw_backend_openai, model, sizeof(model));

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddBoolToObject(root, "stream", true);
    int limit = claw_config_get_int("max_tokens", 2048);
    cJSON_AddNumberToObject(root,
                            wants_max_completion(model) ? "max_completion_tokens" : "max_tokens",
                            limit);
    cJSON_AddItemToObject(root, "messages", cJSON_Duplicate(messages, true));
    return root;
}

static const char *text(const char *event, cJSON *obj)
{
    (void)event;
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(obj, "choices");
    cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    if (!first) {
        return NULL;
    }
    cJSON *delta = cJSON_GetObjectItemCaseSensitive(first, "delta");
    if (!delta) {
        return NULL;
    }
    cJSON *c = cJSON_GetObjectItemCaseSensitive(delta, "content");
    return cJSON_IsString(c) ? c->valuestring : NULL;
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

const claw_backend_t claw_backend_openai = {
    .name          = "openai",
    .default_model = "gpt-4o-mini",
    .ca_file       = NULL,
    .endpoint      = ep,
    .headers       = hdrs,
    .build_body    = body,
    .extract_text  = text,
    .extract_error = err,
};
