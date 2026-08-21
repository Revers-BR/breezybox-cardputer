#include "claw_text.h"
#include "claw_util.h"

#include "cJSON.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

__attribute__((unused)) static const char *TAG = "claw_text";

/*
 * Overrides live here; shipped defaults are read from the install dirs.
 *
 * Overridable at compile time so the host tests can point at a writable
 * directory -- macOS will not allow creating /sd -- and exercise the loader,
 * lookup and validator together rather than in isolation.
 */
#ifndef TEXT_DIR_SD
#define TEXT_DIR_SD  "/sd/claw"
#endif

/* Device paths are short ("/sd/claw/messages.json" is 22), but the host tests
 * build with TEXT_DIR_SD pointing at a temp directory, which is not. */
#define CLAW_TEXT_PATH_MAX 256

static const char *const k_shipped_dirs[] = {
    "/sd/espclaw",
    "/sd/apps/espclaw",
    "/root/apps/espclaw",
};

/* The overridable files. `prompt` is plain text; the others are JSON objects
 * mapping id -> string. */
static const char *const k_files[] = { "prompt.md", "tools.json", "messages.json" };

static cJSON  *s_tools;          /* parsed /sd/claw/tools.json */
static cJSON  *s_messages;       /* parsed /sd/claw/messages.json */
static bool    s_loaded;
static int     s_refused;        /* overrides rejected for unsafe specifiers */
static char    s_last_refused[64];

/* ------------------------------------------------------- specifier parsing -- */

bool claw_text_specifiers(const char *fmt, char *out, size_t out_len)
{
    size_t n = 0;
    if (!fmt || !out || out_len == 0) {
        return false;
    }
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            continue;
        }
        p++;
        if (*p == '\0') {
            break;
        }
        if (*p == '%') {
            continue;                 /* an escaped percent, not a conversion */
        }
        while (*p && strchr("-+ #0", *p)) {
            p++;                      /* flags do not change the argument */
        }

        /*
         * Width and precision are usually literal digits, but a '*' takes the
         * value from an argument of its own -- "%.*s" consumes an int and then
         * a char*. Recording the '*' matters: without it "%.*s" and "%.*d"
         * compare as identical and an override could swap them.
         */
        if (*p == '*') {
            if (n + 1 >= out_len) {
                return false;
            }
            out[n++] = '*';
            p++;
        } else {
            while (*p && (*p >= '0' && *p <= '9')) {
                p++;
            }
        }
        if (*p == '.') {
            p++;
            if (*p == '*') {
                if (n + 1 >= out_len) {
                    return false;
                }
                out[n++] = '*';
                p++;
            } else {
                while (*p && (*p >= '0' && *p <= '9')) {
                    p++;
                }
            }
        }
        /* length modifiers do change it, so keep them */
        char len_mod[3] = {0};
        size_t li = 0;
        while (*p && strchr("hlLzjt", *p) && li < 2) {
            len_mod[li++] = *p++;
        }
        if (*p == '\0') {
            break;
        }
        /* one slot per conversion: length modifier plus the type letter */
        for (size_t i = 0; i < li; i++) {
            if (n + 1 >= out_len) {
                return false;
            }
            out[n++] = len_mod[i];
        }
        if (n + 1 >= out_len) {
            return false;
        }
        out[n++] = *p;
    }
    out[n] = '\0';
    return true;
}

bool claw_text_compatible(const char *fallback, const char *candidate)
{
    char a[64];
    char b[64];
    if (!claw_text_specifiers(fallback, a, sizeof(a))) {
        return false;                 /* more specifiers than we can verify */
    }
    if (!claw_text_specifiers(candidate, b, sizeof(b))) {
        return false;
    }
    return strcmp(a, b) == 0;
}

/* ------------------------------------------------------------------ loading -- */

static cJSON *load_json(const char *name)
{
    char path[CLAW_TEXT_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", TEXT_DIR_SD, name);

    char *raw = claw_read_file(path, 16384, NULL);
    if (!raw) {
        return NULL;
    }
    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root || !cJSON_IsObject(root)) {
        /* A malformed file falls back entirely rather than half-applying. */
        ESP_LOGW(TAG, "%s is not a JSON object; ignoring it", path);
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static void ensure_loaded(void)
{
    if (s_loaded) {
        return;
    }
    s_loaded = true;
    s_refused = 0;
    s_last_refused[0] = '\0';
    s_tools = load_json("tools.json");
    s_messages = load_json("messages.json");
}

void claw_text_reload(void)
{
    cJSON_Delete(s_tools);
    cJSON_Delete(s_messages);
    s_tools = NULL;
    s_messages = NULL;
    s_loaded = false;
}

/* ------------------------------------------------------------------- lookup -- */

/* Ids are dotted: "tools.<tool>.<key>" reads tools.json, everything else
 * reads messages.json. */
static const char *lookup(const char *id)
{
    ensure_loaded();

    cJSON *from = s_messages;
    if (s_tools && strncmp(id, "tools.", 6) == 0) {
        /* tools.<name>.description / tools.<name>.param.<prop> */
        const char *rest = id + 6;
        const char *dot = strchr(rest, '.');
        if (dot) {
            char tool[48];
            size_t n = (size_t)(dot - rest);
            if (n < sizeof(tool)) {
                memcpy(tool, rest, n);
                tool[n] = '\0';
                cJSON *entry = cJSON_GetObjectItemCaseSensitive(s_tools, tool);
                if (cJSON_IsObject(entry)) {
                    cJSON *v = cJSON_GetObjectItemCaseSensitive(entry, dot + 1);
                    if (cJSON_IsString(v)) {
                        return v->valuestring;
                    }
                }
            }
        }
    }
    if (!from) {
        return NULL;
    }
    cJSON *v = cJSON_GetObjectItemCaseSensitive(from, id);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

const char *claw_text(const char *id, const char *fallback)
{
    if (!id || !fallback) {
        return fallback;
    }
    const char *over = lookup(id);
    if (!over || !over[0]) {
        return fallback;
    }
    if (!claw_text_compatible(fallback, over)) {
        /* Refusing is the whole point: a mismatched specifier would be read as
         * the wrong type at the call site. */
        s_refused++;
        snprintf(s_last_refused, sizeof(s_last_refused), "%s", id);
        ESP_LOGW(TAG, "override '%s' has different format specifiers; ignoring", id);
        return fallback;
    }
    return over;
}

/* ------------------------------------------------------------------ command -- */

static bool file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static int count_keys(cJSON *o)
{
    int n = 0;
    cJSON *c = NULL;
    cJSON_ArrayForEach(c, o) {
        n++;
    }
    return n;
}

void claw_text_status(void)
{
    ensure_loaded();

    char path[CLAW_TEXT_PATH_MAX];
    printf("text overrides (%s):\n", TEXT_DIR_SD);

    snprintf(path, sizeof(path), "%s/prompt.md", TEXT_DIR_SD);
    printf("  prompt.md      %s\n", file_exists(path) ? "active" : "(using built-in)");

    printf("  tools.json     %s", s_tools ? "active" : "(using built-in)");
    if (s_tools) {
        printf(", %d tool(s) overridden", count_keys(s_tools));
    }
    printf("\n");

    printf("  messages.json  %s", s_messages ? "active" : "(using built-in)");
    if (s_messages) {
        printf(", %d message(s) overridden", count_keys(s_messages));
    }
    printf("\n");

    if (s_refused > 0) {
        printf("\n  %d override(s) refused for changing format specifiers"
               " (last: %s)\n", s_refused, s_last_refused);
        printf("  those keys are using the built-in text.\n");
    }
}

int claw_text_dump(void)
{
    mkdir("/sd", 0777);
    mkdir(TEXT_DIR_SD, 0777);

    int written = 0;
    for (size_t i = 0; i < sizeof(k_files) / sizeof(k_files[0]); i++) {
        char dst[CLAW_TEXT_PATH_MAX];
        snprintf(dst, sizeof(dst), "%s/%s", TEXT_DIR_SD, k_files[i]);
        if (file_exists(dst)) {
            printf("  %s already exists, leaving it alone\n", dst);
            continue;
        }

        char *data = NULL;
        for (size_t d = 0; d < sizeof(k_shipped_dirs) / sizeof(k_shipped_dirs[0]); d++) {
            char src[CLAW_TEXT_PATH_MAX];
            snprintf(src, sizeof(src), "%s/%s", k_shipped_dirs[d], k_files[i]);
            data = claw_read_file(src, 16384, NULL);
            if (data) {
                break;
            }
        }
        if (!data) {
            printf("  %s: no shipped default found\n", k_files[i]);
            continue;
        }

        FILE *f = fopen(dst, "wb");
        if (f) {
            fwrite(data, 1, strlen(data), f);
            fclose(f);
            printf("  wrote %s\n", dst);
            written++;
        } else {
            printf("  cannot write %s\n", dst);
        }
        free(data);
    }
    claw_text_reload();
    return written;
}
