/* Anthropic Messages API. Named SSE events; text at delta.text. */
#include "claw_backend.h"
#include "claw_config.h"
#include "claw_tools.h"

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

/* Anthropic declares tools with a JSON schema under input_schema. */
static void add_tools(cJSON *body)
{
    cJSON *tools = cJSON_CreateArray();
    if (!tools) {
        return;
    }
    for (size_t i = 0; i < claw_tools_count(); i++) {
        const claw_tool_t *t = claw_tools_at(i);
        cJSON *entry = cJSON_CreateObject();
        if (!entry) {
            continue;
        }
        cJSON_AddStringToObject(entry, "name", t->name);
        cJSON_AddStringToObject(entry, "description", t->description);
        cJSON *schema = t->schema();
        if (schema) {
            cJSON_AddItemToObject(entry, "input_schema", schema);
        }
        cJSON_AddItemToArray(tools, entry);
    }
    cJSON_AddItemToObject(body, "tools", tools);
}

/*
 * Arguments arrive as input_json_delta fragments, so this runs across several
 * events: content_block_start opens the call, deltas append to it, and
 * content_block_stop closes it.
 */
static bool extract_tool_call(const char *event, cJSON *obj, claw_tool_accum_t *acc)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(obj, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : (event ? event : "");

    if (strcmp(t, "content_block_start") == 0) {
        cJSON *block = cJSON_GetObjectItemCaseSensitive(obj, "content_block");
        cJSON *bt = block ? cJSON_GetObjectItemCaseSensitive(block, "type") : NULL;
        if (!cJSON_IsString(bt) || strcmp(bt->valuestring, "tool_use") != 0) {
            return false;
        }
        cJSON *nm = cJSON_GetObjectItemCaseSensitive(block, "name");
        cJSON *id = cJSON_GetObjectItemCaseSensitive(block, "id");
        snprintf(acc->name, sizeof(acc->name), "%s",
                 cJSON_IsString(nm) ? nm->valuestring : "");
        snprintf(acc->id, sizeof(acc->id), "%s",
                 cJSON_IsString(id) ? id->valuestring : "");
        acc->json_len = 0;
        acc->json[0] = '\0';
        acc->active = true;
        return false;
    }

    if (strcmp(t, "content_block_delta") == 0 && acc->active) {
        cJSON *d = cJSON_GetObjectItemCaseSensitive(obj, "delta");
        cJSON *dt = d ? cJSON_GetObjectItemCaseSensitive(d, "type") : NULL;
        if (cJSON_IsString(dt) && strcmp(dt->valuestring, "input_json_delta") == 0) {
            cJSON *pj = cJSON_GetObjectItemCaseSensitive(d, "partial_json");
            if (cJSON_IsString(pj)) {
                claw_tool_accum_add(acc, pj->valuestring);
            }
        }
        return false;
    }

    if (strcmp(t, "content_block_stop") == 0 && acc->active) {
        acc->active = false;
        if (acc->json_len == 0) {
            claw_tool_accum_add(acc, "{}");   /* a call with no arguments */
        }
        return acc->name[0] != '\0';
    }

    return false;
}

/* The call goes back as an assistant tool_use block, the result as a user
 * tool_result block referencing its id. */
static void append_tool_result(cJSON *messages, const char *name, const char *id,
                               const cJSON *args, const char *result)
{
    cJSON *assistant = cJSON_CreateObject();
    cJSON *ac = cJSON_CreateArray();
    cJSON *use = cJSON_CreateObject();
    if (assistant && ac && use) {
        cJSON_AddStringToObject(use, "type", "tool_use");
        cJSON_AddStringToObject(use, "id", id ? id : "");
        cJSON_AddStringToObject(use, "name", name);
        cJSON_AddItemToObject(use, "input", args ? cJSON_Duplicate(args, true)
                                                 : cJSON_CreateObject());
        cJSON_AddItemToArray(ac, use);
        cJSON_AddStringToObject(assistant, "role", "assistant");
        cJSON_AddItemToObject(assistant, "content", ac);
        cJSON_AddItemToArray(messages, assistant);
    } else {
        cJSON_Delete(assistant);
        cJSON_Delete(ac);
        cJSON_Delete(use);
    }

    cJSON *user = cJSON_CreateObject();
    cJSON *uc = cJSON_CreateArray();
    cJSON *res = cJSON_CreateObject();
    if (user && uc && res) {
        cJSON_AddStringToObject(res, "type", "tool_result");
        cJSON_AddStringToObject(res, "tool_use_id", id ? id : "");
        cJSON_AddStringToObject(res, "content", result ? result : "");
        cJSON_AddItemToArray(uc, res);
        cJSON_AddStringToObject(user, "role", "user");
        cJSON_AddItemToObject(user, "content", uc);
        cJSON_AddItemToArray(messages, user);
    } else {
        cJSON_Delete(user);
        cJSON_Delete(uc);
        cJSON_Delete(res);
    }
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
    .add_tools     = add_tools,
    .extract_tool_call  = extract_tool_call,
    .append_tool_result = append_tool_result,
};
