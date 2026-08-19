/*
 * claw_backend.h - one interface, three providers.
 *
 * The differences between the providers are larger than they look, and each is
 * captured here rather than smeared through the agent loop:
 *
 *              auth              model      turns              assistant role
 *   anthropic  x-api-key         in body    messages[].content assistant
 *   openai     Bearer            in body    messages[].content assistant
 *   gemini     x-goog-api-key    in URL     contents[].parts[] model
 *
 * Streaming differs too: Anthropic sends named SSE events, the other two send
 * unnamed `data:` lines (OpenAI terminating with a literal [DONE]).
 */
#pragma once

#include "cJSON.h"
#include "esp_http_client.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *name;
    /* Last-resort default, used only when the model catalogue is missing or has
     * no entry for this backend. The catalogue (claw_models.h) is the real
     * source; see /sd/claw/models.json. */
    const char *default_model;

    /* A pinned root CA, or NULL to use the IDF certificate bundle. Gemini needs
     * one: its chain ends in a cross-signed GTS Root R1 that the bundle cannot
     * verify. Path is relative to the install dir. */
    const char *ca_file;

    void (*endpoint)(char *out, size_t out_len);
    void (*headers)(esp_http_client_handle_t client, const char *api_key);

    /* Build the request body. `messages` is a JSON array of {role, content},
     * borrowed -- the agent loop reuses it across tool rounds, so an
     * implementation must duplicate anything it keeps. Returns an object the
     * caller owns. */
    cJSON *(*build_body)(const cJSON *messages);

    /* Text to emit for one decoded event, or NULL. Points into `obj`. */
    const char *(*extract_text)(const char *event, cJSON *obj);

    /* An error carried inside the payload, or NULL. Writes into `buf`. */
    const char *(*extract_error)(cJSON *obj, char *buf, size_t buf_len);

    /* --- tool calling. NULL on backends that do not support it yet. ------- */

    /* Declare the registered tools on an outgoing request body. */
    void (*add_tools)(cJSON *body);

    /* Pull a tool call out of one decoded event. Returns true when this event
     * carried a complete call. `id` distinguishes concurrent calls where the
     * provider supplies one; it may be left empty.
     * `args_out` receives an object the caller owns. */
    bool (*extract_tool_call)(const char *event, cJSON *obj,
                              char *name_out, size_t name_len,
                              char *id_out, size_t id_len,
                              cJSON **args_out);

    /* Append the assistant's tool call and its result to a messages array, in
     * whatever shape this provider expects to receive them back. */
    void (*append_tool_result)(cJSON *messages,
                               const char *name, const char *id,
                               const cJSON *args, const char *result);
} claw_backend_t;

/* The three implementations. Declared here so each can reference its own
 * struct from helpers defined above it. */
extern const claw_backend_t claw_backend_anthropic;
extern const claw_backend_t claw_backend_openai;
extern const claw_backend_t claw_backend_gemini;

/*
 * The model for this backend.
 *
 * Stored per backend as `model.<name>`, so switching providers cannot leave a
 * Gemini model pointed at OpenAI. Falls back to a bare `model` key (which older
 * configs used) and then to the backend's default.
 */
void claw_backend_model(const claw_backend_t *b, char *out, size_t out_len);

/* Set the model for a backend; persists to `model.<name>`. */
bool claw_backend_set_model(const claw_backend_t *b, const char *model);

/* Iterate the registered backends. */
size_t claw_backend_count(void);
const claw_backend_t *claw_backend_at(size_t i);

/* Look up by name; NULL if unknown. */
const claw_backend_t *claw_backend_find(const char *name);
/* The backend named by the `backend` setting. Never NULL. */
const claw_backend_t *claw_backend_active(void);
