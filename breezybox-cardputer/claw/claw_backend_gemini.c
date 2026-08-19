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
static cJSON *body(const cJSON *messages)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *contents = cJSON_CreateArray();
    if (!root || !contents) {
        cJSON_Delete(root);
        cJSON_Delete(contents);
        return NULL;
    }

    cJSON *sys_text = NULL;
    const cJSON *m = NULL;
    cJSON_ArrayForEach(m, messages) {
        cJSON *role = cJSON_GetObjectItemCaseSensitive(m, "role");
        cJSON *content = cJSON_GetObjectItemCaseSensitive(m, "content");

        /*
         * Two shapes share this array. Transcript turns are the neutral
         * {role, content}; tool call/result turns are already Gemini-native
         * {role, parts} and must be passed through untouched -- converting them
         * would drop the functionCall/functionResponse and the model would see
         * its tool result vanish, then call the tool again.
         */
        cJSON *native_parts = cJSON_GetObjectItemCaseSensitive(m, "parts");
        if (cJSON_IsArray(native_parts)) {
            cJSON *copy = cJSON_Duplicate(m, true);
            if (copy) {
                cJSON_AddItemToArray(contents, copy);
            }
            continue;
        }

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

/* Gemini declares tools as functionDeclarations, each with a JSON schema. */
static void add_tools(cJSON *body)
{
    cJSON *decls = cJSON_CreateArray();
    if (!decls) {
        return;
    }
    for (size_t i = 0; i < claw_tools_count(); i++) {
        const claw_tool_t *t = claw_tools_at(i);
        cJSON *d = cJSON_CreateObject();
        if (!d) {
            continue;
        }
        cJSON_AddStringToObject(d, "name", t->name);
        cJSON_AddStringToObject(d, "description", t->description);
        cJSON *schema = t->schema();
        if (schema) {
            cJSON_AddItemToObject(d, "parameters", schema);
        }
        cJSON_AddItemToArray(decls, d);
    }

    cJSON *tools = cJSON_CreateArray();
    cJSON *entry = cJSON_CreateObject();
    if (!tools || !entry) {
        cJSON_Delete(decls);
        cJSON_Delete(tools);
        cJSON_Delete(entry);
        return;
    }
    cJSON_AddItemToObject(entry, "functionDeclarations", decls);
    cJSON_AddItemToArray(tools, entry);
    cJSON_AddItemToObject(body, "tools", tools);
}

/* A call arrives whole inside a part, so there is no fragment reassembly here
 * (unlike Anthropic and OpenAI, which stream the arguments). */
static bool extract_tool_call(const char *event, cJSON *obj, claw_tool_accum_t *acc)
{
    (void)event;

    cJSON *cands = cJSON_GetObjectItemCaseSensitive(obj, "candidates");
    cJSON *first = cJSON_IsArray(cands) ? cJSON_GetArrayItem(cands, 0) : NULL;
    if (!first) {
        return false;
    }
    cJSON *content = cJSON_GetObjectItemCaseSensitive(first, "content");
    cJSON *parts = content ? cJSON_GetObjectItemCaseSensitive(content, "parts") : NULL;

    cJSON *part = NULL;
    cJSON_ArrayForEach(part, parts) {
        cJSON *fc = cJSON_GetObjectItemCaseSensitive(part, "functionCall");
        if (!cJSON_IsObject(fc)) {
            continue;
        }
        cJSON *nm = cJSON_GetObjectItemCaseSensitive(fc, "name");
        if (!cJSON_IsString(nm)) {
            continue;
        }
        snprintf(acc->name, sizeof(acc->name), "%s", nm->valuestring);
        acc->id[0] = '\0';          /* Gemini supplies no call id */

        cJSON *a = cJSON_GetObjectItemCaseSensitive(fc, "args");
        char *txt = a ? cJSON_PrintUnformatted(a) : NULL;
        acc->json_len = 0;
        acc->json[0] = '\0';
        claw_tool_accum_add(acc, txt ? txt : "{}");
        if (txt) {
            cJSON_free(txt);
        }
        return true;
    }
    return false;
}

/* The call goes back as a model turn, the result as a user turn holding a
 * functionResponse part. */
static void append_tool_result(cJSON *messages, const char *name, const char *id,
                               const cJSON *args, const char *result)
{
    (void)id;

    cJSON *call_turn = cJSON_CreateObject();
    cJSON *call_parts = cJSON_CreateArray();
    cJSON *call_part = cJSON_CreateObject();
    cJSON *fc = cJSON_CreateObject();
    if (call_turn && call_parts && call_part && fc) {
        cJSON_AddStringToObject(fc, "name", name);
        cJSON_AddItemToObject(fc, "args", args ? cJSON_Duplicate(args, true)
                                               : cJSON_CreateObject());
        cJSON_AddItemToObject(call_part, "functionCall", fc);
        cJSON_AddItemToArray(call_parts, call_part);
        cJSON_AddStringToObject(call_turn, "role", "model");
        cJSON_AddItemToObject(call_turn, "parts", call_parts);
        cJSON_AddItemToArray(messages, call_turn);
    } else {
        cJSON_Delete(call_turn);
        cJSON_Delete(call_parts);
        cJSON_Delete(call_part);
        cJSON_Delete(fc);
    }

    cJSON *res_turn = cJSON_CreateObject();
    cJSON *res_parts = cJSON_CreateArray();
    cJSON *res_part = cJSON_CreateObject();
    cJSON *fr = cJSON_CreateObject();
    cJSON *resp = cJSON_CreateObject();
    if (res_turn && res_parts && res_part && fr && resp) {
        cJSON_AddStringToObject(resp, "result", result ? result : "");
        cJSON_AddStringToObject(fr, "name", name);
        cJSON_AddItemToObject(fr, "response", resp);
        cJSON_AddItemToObject(res_part, "functionResponse", fr);
        cJSON_AddItemToArray(res_parts, res_part);
        cJSON_AddStringToObject(res_turn, "role", "user");
        cJSON_AddItemToObject(res_turn, "parts", res_parts);
        cJSON_AddItemToArray(messages, res_turn);
    } else {
        cJSON_Delete(res_turn);
        cJSON_Delete(res_parts);
        cJSON_Delete(res_part);
        cJSON_Delete(fr);
        cJSON_Delete(resp);
    }
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
    .add_tools     = add_tools,
    .extract_tool_call  = extract_tool_call,
    .append_tool_result = append_tool_result,
};
