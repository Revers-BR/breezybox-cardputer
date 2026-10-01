/* OpenAI-compatible /v1/chat/completions. Also covers OpenRouter, Ollama and
 * llama.cpp via base_url. Unnamed SSE events terminated by `data: [DONE]`. */
#include "claw_backend.h"
#include "claw_config.h"
#include "claw_tools.h"

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
    /* A reference, not a copy: see CLAW_BODY_REFERENCES in claw_backend.h. */
    cJSON_AddItemToObjectCS(root, "messages", cJSON_CreateArrayReference(messages->child));
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

/* OpenAI wraps each tool in a function object. */
static void add_tools(cJSON *body)
{
    cJSON *tools = cJSON_CreateArray();
    if (!tools) {
        return;
    }
    for (size_t i = 0; i < claw_tools_count(); i++) {
        const claw_tool_t *t = claw_tools_at(i);
        cJSON *entry = cJSON_CreateObject();
        cJSON *fn = cJSON_CreateObject();
        if (!entry || !fn) {
            cJSON_Delete(entry);
            cJSON_Delete(fn);
            continue;
        }
        cJSON_AddStringToObject(fn, "name", t->name);
        cJSON_AddStringToObject(fn, "description", claw_tool_description(t));
        cJSON *schema = t->schema();
        if (schema) {
            cJSON_AddItemToObject(fn, "parameters", schema);
        }
        cJSON_AddStringToObject(entry, "type", "function");
        cJSON_AddItemToObject(entry, "function", fn);
        cJSON_AddItemToArray(tools, entry);
    }
    cJSON_AddItemToObject(body, "tools", tools);
}

/*
 * Arguments stream as fragments on tool_calls[].function.arguments. The name
 * and id arrive on the first delta only, and completion is signalled by
 * finish_reason == "tool_calls" rather than by a closing event.
 */
static bool extract_tool_call(const char *event, cJSON *obj, claw_tool_accum_t *acc)
{
    (void)event;

    cJSON *choices = cJSON_GetObjectItemCaseSensitive(obj, "choices");
    cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    if (!first) {
        return false;
    }

    cJSON *delta = cJSON_GetObjectItemCaseSensitive(first, "delta");
    cJSON *calls = delta ? cJSON_GetObjectItemCaseSensitive(delta, "tool_calls") : NULL;
    cJSON *call = cJSON_IsArray(calls) ? cJSON_GetArrayItem(calls, 0) : NULL;
    if (call) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(call, "id");
        if (cJSON_IsString(id) && id->valuestring[0]) {
            snprintf(acc->id, sizeof(acc->id), "%s", id->valuestring);
            acc->active = true;
            acc->json_len = 0;
            acc->json[0] = '\0';
        }
        cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
        if (fn) {
            cJSON *nm = cJSON_GetObjectItemCaseSensitive(fn, "name");
            if (cJSON_IsString(nm) && nm->valuestring[0]) {
                snprintf(acc->name, sizeof(acc->name), "%s", nm->valuestring);
                acc->active = true;
            }
            cJSON *a = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
            if (cJSON_IsString(a)) {
                claw_tool_accum_add(acc, a->valuestring);
            }
        }
    }

    cJSON *fr = cJSON_GetObjectItemCaseSensitive(first, "finish_reason");
    if (cJSON_IsString(fr) && strcmp(fr->valuestring, "tool_calls") == 0 && acc->active) {
        acc->active = false;
        if (acc->json_len == 0) {
            claw_tool_accum_add(acc, "{}");
        }
        return acc->name[0] != '\0';
    }
    return false;
}

/* The call goes back on an assistant message, the result as a separate message
 * with role "tool" referencing the call id. */
static void append_tool_result(cJSON *messages, const char *name, const char *id,
                               const cJSON *args, const char *result)
{
    cJSON *assistant = cJSON_CreateObject();
    cJSON *calls = cJSON_CreateArray();
    cJSON *call = cJSON_CreateObject();
    cJSON *fn = cJSON_CreateObject();
    if (assistant && calls && call && fn) {
        char *argstr = args ? cJSON_PrintUnformatted(args) : NULL;
        cJSON_AddStringToObject(fn, "name", name);
        /* OpenAI wants the arguments as a JSON *string*, not an object. */
        cJSON_AddStringToObject(fn, "arguments", argstr ? argstr : "{}");
        if (argstr) {
            cJSON_free(argstr);
        }
        cJSON_AddStringToObject(call, "id", id ? id : "");
        cJSON_AddStringToObject(call, "type", "function");
        cJSON_AddItemToObject(call, "function", fn);
        cJSON_AddItemToArray(calls, call);

        cJSON_AddStringToObject(assistant, "role", "assistant");
        cJSON_AddNullToObject(assistant, "content");
        cJSON_AddItemToObject(assistant, "tool_calls", calls);
        cJSON_AddItemToArray(messages, assistant);
    } else {
        cJSON_Delete(assistant);
        cJSON_Delete(calls);
        cJSON_Delete(call);
        cJSON_Delete(fn);
    }

    cJSON *tool = cJSON_CreateObject();
    if (tool) {
        cJSON_AddStringToObject(tool, "role", "tool");
        cJSON_AddStringToObject(tool, "tool_call_id", id ? id : "");
        cJSON_AddStringToObject(tool, "content", result ? result : "");
        cJSON_AddItemToArray(messages, tool);
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

const claw_backend_t claw_backend_openai = {
    .name          = "openai",
    .default_model = "gpt-4o-mini",
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
