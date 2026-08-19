/*
 * claw_tools.h - the capability registry.
 *
 * Each tool is a JSON-schema declaration plus a handler. The schema is sent to
 * the model so it knows what it can call; the handler runs on device.
 *
 * Filesystem access is confined to /root and /sd. That is enforced here rather
 * than left to the model, because a model that has been talked into reading
 * /dev or writing outside those roots should still fail.
 */
#pragma once

#include "cJSON.h"

#include <stdbool.h>
#include <stddef.h>

/* Tool output is truncated to this, with a marker. The full text of a long
 * result is left on disk where the model can read it back in pieces, rather
 * than being pushed through the context window in one go. */
#define CLAW_TOOL_RESULT_MAX 2048

typedef struct {
    const char *name;
    const char *description;

    /* JSON-schema `properties` and `required`, built on demand so the schema
     * lives next to the handler it documents. */
    cJSON *(*schema)(void);

    /* Run the tool. `args` is the model's decoded arguments (may be NULL).
     * Writes a human-readable result into `out`. Returns false on failure,
     * in which case `out` should explain why -- the message goes back to the
     * model, which can usually correct itself. */
    bool (*run)(const cJSON *args, char *out, size_t out_len);
} claw_tool_t;

/*
 * Confirmation hook for destructive actions.
 *
 * The tool layer refuses to delete or overwrite without asking. The prompt
 * itself belongs to whatever is driving the agent, so it is injected: the
 * console registers a linenoise prompt, and anything non-interactive (a
 * scheduled task, say) leaves it unset.
 *
 * With no hook registered the answer is NO. Denying by default is the only
 * safe behaviour when there is nobody to ask.
 */
typedef bool (*claw_confirm_fn)(const char *action, const char *detail);
void claw_tools_set_confirm(claw_confirm_fn fn);

/* True when this shell command would delete or overwrite something. */
bool claw_tools_shell_is_destructive(const char *command);

size_t claw_tools_count(void);
const claw_tool_t *claw_tools_at(size_t i);
const claw_tool_t *claw_tools_find(const char *name);

/* Dispatch by name. Always writes something to `out`, including for an unknown
 * tool, so the model always gets a usable reply. */
bool claw_tools_run(const char *name, const cJSON *args, char *out, size_t out_len);

/* True when `path` resolves inside /root or /sd. */
bool claw_tools_path_allowed(const char *path);
