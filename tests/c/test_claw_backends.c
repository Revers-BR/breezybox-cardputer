/*
 * Host test for request-body construction, one per backend.
 *
 * These bodies are the contract with three different APIs, and a mistake shows
 * up as an unhelpful HTTP 400 on the device. The shapes differ more than they
 * look -- model in the body vs the URL, `messages` vs `contents`, "assistant"
 * vs "model", system prompt as a message vs a top-level field -- so each is
 * checked against what that API actually expects.
 *
 * Built against the real backend sources with a small stub for the config and
 * tool registry, so it tests the shipped code rather than a copy.
 *
 * Run: sh tests/c/run.sh
 */
#include "claw_backend.h"
#include "claw_config.h"
#include "claw_models.h"
#include "claw_tools.h"
#include "claw_json_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ stubs -- */

static const char *s_backend = "anthropic";
static const char *s_model   = "";
static const char *s_base    = "";
static const char *s_thinking = "";   /* gemini.thinking_budget; "" = unset */

bool claw_config_get(const char *key, char *out, size_t out_len, const char *fallback)
{
    const char *v = NULL;
    if (strcmp(key, "backend") == 0)  v = s_backend;
    else if (strcmp(key, "model") == 0) v = s_model;
    else if (strcmp(key, "base_url") == 0) v = s_base;
    else if (strcmp(key, "gemini.thinking_budget") == 0) v = s_thinking;
    else if (strncmp(key, "model.", 6) == 0) v = s_model;

    if (v && v[0]) {
        snprintf(out, out_len, "%s", v);
        return true;
    }
    snprintf(out, out_len, "%s", fallback ? fallback : "");
    return false;
}

int claw_config_get_int(const char *key, int fallback)
{
    (void)key;
    return fallback;
}

bool claw_config_set(const char *key, const char *value)
{
    (void)key; (void)value;
    return true;
}

/* No catalogue on the host, so each backend falls back to its own default --
 * which is the behaviour worth testing anyway. */
bool claw_models_default(const char *backend, char *out, size_t out_len)
{
    (void)backend; (void)out; (void)out_len;
    return false;
}

/* Tool registry: one tool, enough to check the declaration shape. */
static cJSON *dummy_schema(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON_AddItemToObject(s, "properties", cJSON_CreateObject());
    return s;
}

static bool dummy_run(const cJSON *a, char *o, size_t n)
{
    (void)a; (void)o; (void)n;
    return true;
}

static const claw_tool_t k_tool = {
    "read_file", "Read a file.", dummy_schema, dummy_run,
};

size_t claw_tools_count(void) { return 1; }

/* The real one consults the SD override; on the host there is none, so the
 * compiled description is what the backends should receive. */
const char *claw_tool_description(const claw_tool_t *t) { return t->description; }
const claw_tool_t *claw_tools_at(size_t i) { return i == 0 ? &k_tool : NULL; }

/* --------------------------------------------------------------- harness -- */

static int failures;

static void check(const char *name, bool ok, const char *detail)
{
    if (ok) {
        printf("  ok   %s\n", name);
    } else {
        failures++;
        printf("  FAIL %s%s%s\n", name, detail ? "  -- " : "", detail ? detail : "");
    }
}

/* A conversation with all three roles, which is where the shapes diverge. */
static cJSON *sample_messages(void)
{
    cJSON *msgs = cJSON_CreateArray();
    const char *roles[]    = { "system",   "user", "assistant", "user" };
    const char *contents[] = { "be terse", "hi",   "hello",     "again" };
    for (int i = 0; i < 4; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", roles[i]);
        cJSON_AddStringToObject(m, "content", contents[i]);
        cJSON_AddItemToArray(msgs, m);
    }
    return msgs;
}

static void test_anthropic(void)
{
    printf("anthropic\n");
    s_backend = "anthropic"; s_model = ""; s_base = "";
    const claw_backend_t *b = claw_backend_find("anthropic");

    cJSON *msgs = sample_messages();
    cJSON *body = b->build_body(msgs);

    check("messages array present", cJSON_IsArray(cJSON_GetObjectItem(body, "messages")), NULL);
    check("streams", cJSON_IsTrue(cJSON_GetObjectItem(body, "stream")), NULL);
    check("max_tokens set", cJSON_IsNumber(cJSON_GetObjectItem(body, "max_tokens")), NULL);

    /* Anthropic rejects role:"system" inside messages; it must be hoisted. */
    cJSON *sys = cJSON_GetObjectItem(body, "system");
    check("system hoisted to top level", cJSON_IsString(sys), NULL);
    check("system text preserved",
          sys && strcmp(sys->valuestring, "be terse") == 0, sys ? sys->valuestring : "(null)");

    cJSON *arr = cJSON_GetObjectItem(body, "messages");
    check("system removed from messages", cJSON_GetArraySize(arr) == 3, NULL);
    bool any_system = false;
    cJSON *m = NULL;
    cJSON_ArrayForEach(m, arr) {
        cJSON *r = cJSON_GetObjectItem(m, "role");
        if (cJSON_IsString(r) && strcmp(r->valuestring, "system") == 0) any_system = true;
    }
    check("no system role remains", !any_system, NULL);

    b->add_tools(body);
    cJSON *tools = cJSON_GetObjectItem(body, "tools");
    check("tools declared", cJSON_IsArray(tools) && cJSON_GetArraySize(tools) == 1, NULL);
    cJSON *t0 = cJSON_GetArrayItem(tools, 0);
    check("uses input_schema", t0 && cJSON_GetObjectItem(t0, "input_schema") != NULL, NULL);

    cJSON_Delete(body);
    cJSON_Delete(msgs);   /* borrowed, so the caller still owns it */
}

static void test_openai(void)
{
    printf("\nopenai\n");
    s_backend = "openai"; s_model = ""; s_base = "";
    const claw_backend_t *b = claw_backend_find("openai");

    cJSON *msgs = sample_messages();
    cJSON *body = b->build_body(msgs);

    cJSON *arr = cJSON_GetObjectItem(body, "messages");
    check("all four turns kept", cJSON_GetArraySize(arr) == 4, NULL);
    check("system stays a message",
          strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 0), "role")->valuestring,
                 "system") == 0, NULL);
    check("max_tokens for gpt-4o-mini", cJSON_GetObjectItem(body, "max_tokens") != NULL, NULL);

    b->add_tools(body);
    cJSON *t0 = cJSON_GetArrayItem(cJSON_GetObjectItem(body, "tools"), 0);
    check("wrapped in a function object",
          t0 && cJSON_GetObjectItem(t0, "function") != NULL, NULL);
    cJSON_Delete(body);

    /* Reasoning models reject max_tokens. */
    s_model = "o3-mini";
    cJSON *body2 = b->build_body(msgs);
    check("o3 uses max_completion_tokens",
          cJSON_GetObjectItem(body2, "max_completion_tokens") != NULL &&
          cJSON_GetObjectItem(body2, "max_tokens") == NULL, NULL);
    cJSON_Delete(body2);
    cJSON_Delete(msgs);
}

static void test_gemini(void)
{
    printf("\ngemini\n");
    s_backend = "gemini"; s_model = ""; s_base = "";
    const claw_backend_t *b = claw_backend_find("gemini");

    cJSON *msgs = sample_messages();
    cJSON *body = b->build_body(msgs);

    cJSON *contents = cJSON_GetObjectItem(body, "contents");
    check("uses contents, not messages", cJSON_IsArray(contents), NULL);
    check("system excluded from contents", cJSON_GetArraySize(contents) == 3, NULL);

    cJSON *si = cJSON_GetObjectItem(body, "systemInstruction");
    check("system hoisted to systemInstruction", si != NULL, NULL);

    cJSON *turn2 = cJSON_GetArrayItem(contents, 1);
    check("assistant renamed to model",
          strcmp(cJSON_GetObjectItem(turn2, "role")->valuestring, "model") == 0,
          cJSON_GetObjectItem(turn2, "role")->valuestring);

    cJSON *parts = cJSON_GetObjectItem(cJSON_GetArrayItem(contents, 0), "parts");
    check("content becomes parts[].text",
          cJSON_IsArray(parts) &&
          strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(parts, 0), "text")->valuestring,
                 "hi") == 0, NULL);
    check("no stream flag (it is in the URL)",
          cJSON_GetObjectItem(body, "stream") == NULL, NULL);

    b->add_tools(body);
    cJSON *tools = cJSON_GetObjectItem(body, "tools");
    check("functionDeclarations used",
          cJSON_IsArray(tools) &&
          cJSON_GetObjectItem(cJSON_GetArrayItem(tools, 0), "functionDeclarations") != NULL,
          NULL);

    /* The model is in the URL for Gemini, unlike the other two. */
    char url[256];
    b->endpoint(url, sizeof(url));
    check("model in the endpoint path", strstr(url, "gemini-2.5-flash") != NULL, url);
    check("streaming requested via alt=sse", strstr(url, "alt=sse") != NULL, url);

    cJSON_Delete(body);
    cJSON_Delete(msgs);
}

/* gemini.thinking_budget: unset leaves the model's own default, so no
 * thinkingConfig at all; set, it goes in generationConfig.thinkingConfig. */
static cJSON *gemini_thinking_config(const char *setting)
{
    s_backend = "gemini"; s_model = ""; s_base = ""; s_thinking = setting;
    const claw_backend_t *b = claw_backend_find("gemini");
    cJSON *msgs = sample_messages();
    cJSON *body = b->build_body(msgs);
    cJSON_Delete(msgs);
    cJSON *gen = cJSON_GetObjectItem(body, "generationConfig");
    cJSON *tc = cJSON_DetachItemFromObject(gen, "thinkingConfig");
    cJSON_Delete(body);
    s_thinking = "";
    return tc;
}

static void test_gemini_thinking(void)
{
    printf("\ngemini thinking budget\n");

    cJSON *tc = gemini_thinking_config("");
    check("unset: no thinkingConfig sent", tc == NULL, NULL);
    cJSON_Delete(tc);

    tc = gemini_thinking_config("0");
    cJSON *budget = cJSON_GetObjectItem(tc, "thinkingBudget");
    check("0: thinkingBudget 0 (thinking off)",
          cJSON_IsNumber(budget) && budget->valueint == 0, NULL);
    cJSON_Delete(tc);

    tc = gemini_thinking_config("512");
    budget = cJSON_GetObjectItem(tc, "thinkingBudget");
    check("512: thinkingBudget 512", cJSON_IsNumber(budget) && budget->valueint == 512, NULL);
    cJSON_Delete(tc);

    tc = gemini_thinking_config("-1");
    budget = cJSON_GetObjectItem(tc, "thinkingBudget");
    check("-1: thinkingBudget -1 (dynamic)",
          cJSON_IsNumber(budget) && budget->valueint == -1, NULL);
    cJSON_Delete(tc);

    tc = gemini_thinking_config("off");
    check("non-numeric: no thinkingConfig sent", tc == NULL, NULL);
    cJSON_Delete(tc);
}

/* A tool round appends provider-native turns to the same array; build_body must
 * pass them through rather than reinterpreting them as {role, content}. */
static void test_native_passthrough(void)
{
    printf("\ntool turns survive a second round\n");
    s_backend = "gemini"; s_model = ""; s_base = "";
    const claw_backend_t *b = claw_backend_find("gemini");

    cJSON *msgs = sample_messages();
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "path", "/sd/x");
    b->append_tool_result(msgs, "read_file", "", args, "file contents");
    cJSON_Delete(args);

    cJSON *body = b->build_body(msgs);
    cJSON *contents = cJSON_GetObjectItem(body, "contents");

    bool has_call = false, has_response = false;
    cJSON *turn = NULL;
    cJSON_ArrayForEach(turn, contents) {
        cJSON *parts = cJSON_GetObjectItem(turn, "parts");
        cJSON *p = NULL;
        cJSON_ArrayForEach(p, parts) {
            if (cJSON_GetObjectItem(p, "functionCall"))     has_call = true;
            if (cJSON_GetObjectItem(p, "functionResponse")) has_response = true;
        }
    }
    check("functionCall preserved", has_call, NULL);
    check("functionResponse preserved", has_response, NULL);

    cJSON_Delete(body);
    cJSON_Delete(msgs);
}

/*
 * Bodies reference the transcript rather than copying it (CLAW_BODY_REFERENCES
 * in claw_backend.h). Two things must hold for that to be safe: the file the
 * streaming writer produces is exactly what cJSON would print, reference nodes
 * included, and building and deleting a body leaves `messages` untouched.
 */
static char *read_all(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf) {
        buf[fread(buf, 1, (size_t)n, f)] = '\0';
    }
    fclose(f);
    return buf;
}

static void test_body_references(void)
{
    printf("\nbodies reference the transcript safely\n");
    const char *names[] = { "anthropic", "openai", "gemini" };
    for (size_t i = 0; i < 3; i++) {
        s_backend = names[i]; s_model = ""; s_base = "";
        const claw_backend_t *b = claw_backend_find(names[i]);

        cJSON *msgs = sample_messages();
        if (b->append_tool_result) {
            cJSON *args = cJSON_CreateObject();
            cJSON_AddStringToObject(args, "path", "/sd/x");
            b->append_tool_result(msgs, "read_file", "call_1", args, "file contents");
            cJSON_Delete(args);
        }
        char *before = cJSON_PrintUnformatted(msgs);

        cJSON *body = b->build_body(msgs);
        b->add_tools(body);
        char *printed = cJSON_PrintUnformatted(body);
        const char *path = "/tmp/claw_body_ref_test.json";
        long n = claw_json_write_file(body, path);
        char *written = read_all(path);
        char label[96];
        snprintf(label, sizeof(label), "%s: written file matches cJSON's print", names[i]);
        check(label, n > 0 && written && printed && strcmp(written, printed) == 0, NULL);
        cJSON_Delete(body);

        char *after = cJSON_PrintUnformatted(msgs);
        snprintf(label, sizeof(label), "%s: transcript unchanged by build and delete", names[i]);
        check(label, before && after && strcmp(before, after) == 0, NULL);

        free(before); free(after); free(printed); free(written);
        cJSON_Delete(msgs);
        remove(path);
    }
}

int main(void)
{
    test_anthropic();
    test_openai();
    test_gemini();
    test_gemini_thinking();
    test_native_passthrough();
    test_body_references();

    printf("\n");
    if (failures == 0) {
        printf("all backend tests passed\n");
        return 0;
    }
    printf("%d test(s) FAILED\n", failures);
    return 1;
}
