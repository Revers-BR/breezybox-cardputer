#include "claw_tools.h"
#include "claw_config.h"
#include "claw_memory.h"
#include "breezy_exec.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "breezy_vfs.h"
#include "breezybox.h"

#include "esp_heap_caps.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ----------------------------------------------------------- confirmation -- */

static claw_confirm_fn s_confirm;

void claw_tools_set_confirm(claw_confirm_fn fn)
{
    s_confirm = fn;
}

/*
 * Commands that destroy data. Matched on the first word, plus any output
 * redirect, since `foo > bar` overwrites bar just as surely as rm does.
 *
 * The list is deliberately conservative: a false positive costs one keypress,
 * a false negative costs the user's files.
 */
static const char *const k_destructive[] = {
    "rm", "rmdir", "mv", "format", "erase", "mkfs", "dd", "fullclean",
};

bool claw_tools_shell_is_destructive(const char *command)
{
    if (!command) {
        return false;
    }
    while (*command == ' ') {
        command++;
    }
    /* Any redirect overwrites or appends to a file. */
    if (strchr(command, '>')) {
        return true;
    }

    size_t wordlen = 0;
    while (command[wordlen] && command[wordlen] != ' ') {
        wordlen++;
    }
    for (size_t i = 0; i < sizeof(k_destructive) / sizeof(k_destructive[0]); i++) {
        size_t n = strlen(k_destructive[i]);
        if (n == wordlen && strncmp(command, k_destructive[i], n) == 0) {
            return true;
        }
    }
    return false;
}

/* Ask, unless the user has opted out. Returns true when the action may go
 * ahead. */
static bool confirm(const char *action, const char *detail)
{
    char v[8];
    if (claw_config_get("auto_approve", v, sizeof(v), "false") &&
        (strcmp(v, "true") == 0 || strcmp(v, "1") == 0)) {
        return true;
    }
    if (!s_confirm) {
        return false;
    }
    return s_confirm(action, detail);
}

/* ------------------------------------------------------------------ paths -- */

bool claw_tools_path_allowed(const char *path)
{
    if (!path || path[0] != '/') {
        return false;              /* absolute paths only: no cwd surprises */
    }
    if (strstr(path, "..")) {
        return false;              /* no traversal, however it is spelled */
    }
    return strncmp(path, "/root", 5) == 0 || strncmp(path, "/sd", 3) == 0;
}

static const char *arg_str(const cJSON *args, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* A property entry for a JSON schema. */
static void add_prop(cJSON *props, const char *name, const char *type, const char *desc)
{
    cJSON *p = cJSON_CreateObject();
    if (!p) {
        return;
    }
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddStringToObject(p, "description", desc);
    cJSON_AddItemToObject(props, name, p);
}

static cJSON *schema_of(const char *const *required, size_t nreq, cJSON *props)
{
    cJSON *schema = cJSON_CreateObject();
    if (!schema) {
        cJSON_Delete(props);
        return NULL;
    }
    cJSON_AddStringToObject(schema, "type", "object");
    cJSON_AddItemToObject(schema, "properties", props);

    cJSON *req = cJSON_CreateArray();
    for (size_t i = 0; i < nreq; i++) {
        cJSON_AddItemToArray(req, cJSON_CreateString(required[i]));
    }
    cJSON_AddItemToObject(schema, "required", req);
    return schema;
}

/* -------------------------------------------------------------- read_file -- */

static cJSON *read_file_schema(void)
{
    cJSON *props = cJSON_CreateObject();
    add_prop(props, "path", "string", "Absolute path under /root or /sd");
    static const char *req[] = { "path" };
    return schema_of(req, 1, props);
}

static bool read_file_run(const cJSON *args, char *out, size_t out_len)
{
    const char *path = arg_str(args, "path");
    if (!path) {
        snprintf(out, out_len, "error: 'path' is required");
        return false;
    }
    if (!claw_tools_path_allowed(path)) {
        snprintf(out, out_len, "error: path must be under /root or /sd");
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(out, out_len, "error: cannot open %s", path);
        return false;
    }
    size_t got = fread(out, 1, out_len - 64, f);
    long more = 0;
    if (got == out_len - 64) {
        fseek(f, 0, SEEK_END);
        more = ftell(f) - (long)got;
    }
    fclose(f);
    out[got] = '\0';
    if (more > 0) {
        snprintf(out + got, 64, "\n[truncated, %ld more bytes]", more);
    }
    return true;
}

/* ------------------------------------------------------------- write_file -- */

static cJSON *write_file_schema(void)
{
    cJSON *props = cJSON_CreateObject();
    add_prop(props, "path", "string", "Absolute path under /root or /sd");
    add_prop(props, "content", "string", "Text to write");
    add_prop(props, "append", "boolean", "Append instead of overwriting (default false)");
    static const char *req[] = { "path", "content" };
    return schema_of(req, 2, props);
}

static bool write_file_run(const cJSON *args, char *out, size_t out_len)
{
    const char *path = arg_str(args, "path");
    const char *content = arg_str(args, "content");
    if (!path || !content) {
        snprintf(out, out_len, "error: 'path' and 'content' are required");
        return false;
    }
    if (!claw_tools_path_allowed(path)) {
        snprintf(out, out_len, "error: path must be under /root or /sd");
        return false;
    }
    const cJSON *ap = cJSON_GetObjectItemCaseSensitive(args, "append");
    bool append = cJSON_IsTrue(ap);

    /* Creating a file is ordinary; replacing one is not. */
    struct stat st;
    if (!append && stat(path, &st) == 0) {
        if (!confirm("overwrite an existing file", path)) {
            snprintf(out, out_len,
                     "refused: the user did not approve overwriting %s", path);
            return false;
        }
    }

    FILE *f = fopen(path, append ? "ab" : "wb");
    if (!f) {
        snprintf(out, out_len, "error: cannot write %s", path);
        return false;
    }
    size_t len = strlen(content);
    size_t wrote = fwrite(content, 1, len, f);
    fclose(f);
    if (wrote != len) {
        snprintf(out, out_len, "error: short write to %s (%u of %u bytes)",
                 path, (unsigned)wrote, (unsigned)len);
        return false;
    }
    snprintf(out, out_len, "wrote %u bytes to %s", (unsigned)wrote, path);
    return true;
}

/* --------------------------------------------------------------- list_dir -- */

static cJSON *list_dir_schema(void)
{
    cJSON *props = cJSON_CreateObject();
    add_prop(props, "path", "string", "Absolute directory under /root or /sd");
    static const char *req[] = { "path" };
    return schema_of(req, 1, props);
}

static bool list_dir_run(const cJSON *args, char *out, size_t out_len)
{
    const char *path = arg_str(args, "path");
    if (!path) {
        snprintf(out, out_len, "error: 'path' is required");
        return false;
    }
    if (!claw_tools_path_allowed(path)) {
        snprintf(out, out_len, "error: path must be under /root or /sd");
        return false;
    }
    DIR *d = opendir(path);
    if (!d) {
        snprintf(out, out_len, "error: cannot open directory %s", path);
        return false;
    }

    size_t used = 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char full[512];
        int fw = snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        if (fw < 0 || (size_t)fw >= sizeof(full)) {
            continue;                 /* path too long to stat; skip the entry */
        }

        struct stat st;
        bool have = stat(full, &st) == 0;
        bool is_dir = have && S_ISDIR(st.st_mode);

        int w;
        if (is_dir) {
            w = snprintf(out + used, out_len - used, "%s/\n", e->d_name);
        } else {
            w = snprintf(out + used, out_len - used, "%s  %ld bytes\n",
                         e->d_name, have ? (long)st.st_size : 0L);
        }
        if (w < 0 || (size_t)w >= out_len - used) {
            snprintf(out + used, out_len - used, "[truncated]\n");
            break;
        }
        used += (size_t)w;
        n++;
    }
    closedir(d);
    if (n == 0) {
        snprintf(out, out_len, "(empty directory)");
    }
    return true;
}

/* -------------------------------------------------------------- run_shell -- */

static cJSON *run_shell_schema(void)
{
    cJSON *props = cJSON_CreateObject();
    add_prop(props, "command", "string",
             "A single BreezyBox shell command, e.g. 'ls /sd', 'df', 'wifi status', "
             "'help'. Not a Unix shell: no bash, no pipes to external tools, no "
             "scripts. Run 'help' first if unsure what exists.");
    static const char *req[] = { "command" };
    return schema_of(req, 1, props);
}

static bool run_shell_run(const cJSON *args, char *out, size_t out_len)
{
    const char *cmd = arg_str(args, "command");
    if (!cmd || !cmd[0]) {
        snprintf(out, out_len, "error: 'command' is required");
        return false;
    }

    if (claw_tools_shell_is_destructive(cmd)) {
        if (!confirm("run a command that deletes or overwrites data", cmd)) {
            snprintf(out, out_len,
                     "refused: the user did not approve running '%s'", cmd);
            return false;
        }
    }

    /* breezy_exec writes to stdout, so capture via a temp file: the model needs
     * the output, not the user's console. */
    const char *tmp = "/sd/claw/tmp/shell.txt";
    mkdir("/sd/claw", 0777);
    mkdir("/sd/claw/tmp", 0777);

    char redirected[512];
    int n = snprintf(redirected, sizeof(redirected), "%s > %s", cmd, tmp);
    if (n < 0 || (size_t)n >= sizeof(redirected)) {
        snprintf(out, out_len, "error: command too long");
        return false;
    }

    breezybox_exec(redirected);

    FILE *f = fopen(tmp, "rb");
    if (!f) {
        snprintf(out, out_len, "(command produced no output)");
        return true;
    }
    size_t got = fread(out, 1, out_len - 32, f);
    long more = 0;
    if (got == out_len - 32) {
        fseek(f, 0, SEEK_END);
        more = ftell(f) - (long)got;
    }
    fclose(f);
    remove(tmp);

    out[got] = '\0';
    if (got == 0) {
        snprintf(out, out_len, "(command produced no output)");
    } else if (more > 0) {
        snprintf(out + got, 32, "\n[truncated, %ld more]", more);
    }
    return true;
}

/* ---------------------------------------------------------------- memory -- */

/*
 * Long-term memory. The index is in every request; bodies are fetched on
 * demand, which is why memory_read exists at all.
 */
static cJSON *memory_save_schema(void)
{
    cJSON *props = cJSON_CreateObject();
    add_prop(props, "name", "string",
             "Short identifier, letters/digits/dash/underscore only, e.g. "
             "'grove-sensor' or 'user-preferences'. Saving again with the same "
             "name replaces it.");
    add_prop(props, "description", "string",
             "One line describing what this holds. This is what you see in "
             "future conversations, so make it specific enough to know whether "
             "it is worth reading.");
    add_prop(props, "content", "string", "The facts to remember.");
    static const char *req[] = { "name", "description", "content" };
    return schema_of(req, 3, props);
}

static bool memory_save_run(const cJSON *args, char *out, size_t out_len)
{
    const char *name = arg_str(args, "name");
    const char *desc = arg_str(args, "description");
    const char *content = arg_str(args, "content");
    if (!name || !content) {
        snprintf(out, out_len, "error: 'name' and 'content' are required");
        return false;
    }
    if (!claw_memory_save(name, desc, content)) {
        snprintf(out, out_len,
                 "error: could not save '%s' (names may use letters, digits, "
                 "dash and underscore only)", name);
        return false;
    }
    snprintf(out, out_len, "remembered '%s'", name);
    return true;
}

static cJSON *memory_read_schema(void)
{
    cJSON *props = cJSON_CreateObject();
    add_prop(props, "name", "string", "Name from the memory index");
    static const char *req[] = { "name" };
    return schema_of(req, 1, props);
}

static bool memory_read_run(const cJSON *args, char *out, size_t out_len)
{
    const char *name = arg_str(args, "name");
    if (!name) {
        snprintf(out, out_len, "error: 'name' is required");
        return false;
    }
    if (!claw_memory_read(name, out, out_len)) {
        snprintf(out, out_len, "error: nothing remembered under '%s'", name);
        return false;
    }
    return true;
}

static cJSON *memory_forget_schema(void)
{
    cJSON *props = cJSON_CreateObject();
    add_prop(props, "name", "string", "Name from the memory index");
    static const char *req[] = { "name" };
    return schema_of(req, 1, props);
}

static bool memory_forget_run(const cJSON *args, char *out, size_t out_len)
{
    const char *name = arg_str(args, "name");
    if (!name) {
        snprintf(out, out_len, "error: 'name' is required");
        return false;
    }
    /* Forgetting destroys something the user may care about. */
    if (!confirm("forget a stored memory", name)) {
        snprintf(out, out_len, "refused: the user did not approve forgetting '%s'", name);
        return false;
    }
    if (!claw_memory_delete(name)) {
        snprintf(out, out_len, "error: nothing remembered under '%s'", name);
        return false;
    }
    snprintf(out, out_len, "forgot '%s'", name);
    return true;
}

/* -------------------------------------------------------------- i2c_scan -- */

/*
 * Probe the Grove port for I2C devices.
 *
 * Discovery deserves its own tool rather than a Lua script: "what is plugged
 * in?" is the first question anyone asks of an accessory port, and the answer
 * decides what the model writes next.
 */
static cJSON *i2c_scan_schema(void)
{
    cJSON *props = cJSON_CreateObject();
    add_prop(props, "sda", "integer", "SDA pin (default 1, the Grove port)");
    add_prop(props, "scl", "integer", "SCL pin (default 2, the Grove port)");
    return schema_of(NULL, 0, props);
}

static bool i2c_scan_run(const cJSON *args, char *out, size_t out_len)
{
    const cJSON *j;
    int sda = 1, scl = 2;      /* Grove */
    if ((j = cJSON_GetObjectItemCaseSensitive(args, "sda")) && cJSON_IsNumber(j)) {
        sda = (int)j->valuedouble;
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(args, "scl")) && cJSON_IsNumber(j)) {
        scl = (int)j->valuedouble;
    }

    /*
     * Done directly, with no filesystem involved.
     *
     * This previously staged a Lua script to the SD card and captured its
     * output there. With an accessory loading the shared 5V rail, those writes
     * timed out and took the board down with a cache panic during a flash
     * operation. A scan that toggles two GPIOs should not depend on storage
     * being healthy.
     *
     * Port 0: the Cardputer ADV keyboard controller sits on port 1.
     */
    const i2c_port_t port = I2C_NUM_0;

    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = sda,
        .scl_io_num       = scl,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };

    esp_err_t err = i2c_param_config(port, &conf);
    if (err != ESP_OK) {
        snprintf(out, out_len, "error: cannot configure I2C on G%d/G%d: %s",
                 sda, scl, esp_err_to_name(err));
        return false;
    }
    err = i2c_driver_install(port, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        snprintf(out, out_len, "error: cannot install I2C driver: %s",
                 esp_err_to_name(err));
        return false;
    }

    size_t used = 0;
    int found = 0;
    for (uint8_t addr = 1; addr < 0x78; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        if (!cmd) {
            break;
        }
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (uint8_t)((addr << 1) | I2C_MASTER_WRITE), true);
        i2c_master_stop(cmd);
        esp_err_t r = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(30));
        i2c_cmd_link_delete(cmd);

        if (r == ESP_OK) {
            int w = snprintf(out + used, out_len - used, "0x%02X\n", addr);
            if (w > 0 && (size_t)w < out_len - used) {
                used += (size_t)w;
            }
            found++;
        }
    }

    i2c_driver_delete(port);
    /* Leave the pins floating rather than driven, so nothing keeps sinking
     * current through whatever is attached. */
    gpio_reset_pin((gpio_num_t)sda);
    gpio_reset_pin((gpio_num_t)scl);

    if (found == 0) {
        /* State the general limitation rather than guessing at the user's
         * hardware: a scan proves nothing about devices that never drive the
         * bus, and reading "no devices" as "nothing connected" is the wrong
         * conclusion for a whole class of accessory. What is actually attached
         * belongs in memory, not in firmware. */
        snprintf(out, out_len,
                 "No I2C devices responded on G%d/G%d.\n\n"
                 "This only rules out I2C. Output-only devices -- addressable "
                 "LEDs, some displays, servos, relays -- receive on a pin and "
                 "never reply, so a scan cannot detect them and finding nothing "
                 "does not mean nothing is connected.\n\n"
                 "If you know what is attached, drive it directly: call lua_api "
                 "for the relevant module and use run_lua. Otherwise ask the "
                 "user what it is.",
                 sda, scl);
        return true;
    }
    snprintf(out + used, out_len - used, "(%d device%s on G%d/G%d)",
             found, found == 1 ? "" : "s", sda, scl);
    return true;
}

/* ---------------------------------------------------------------- lua_api -- */

/*
 * The device API reference, so the model writes working Lua on the first try
 * instead of guessing a function name and probing.
 *
 * Generated by tools/gen_lua_api.py from the actual bindings, and served on
 * demand rather than pushed into every request -- 4 KB of reference in every
 * turn would cost more than the occasional lookup.
 */
static const char *const k_api_paths[] = {
    "/sd/espclaw/lua_api.md",
    "/sd/apps/espclaw/lua_api.md",
    "/root/apps/espclaw/lua_api.md",
};

static cJSON *lua_api_schema(void)
{
    cJSON *props = cJSON_CreateObject();
    add_prop(props, "module", "string",
             "Which part of the API to return, e.g. 'led', 'i2c', 'gfx', "
             "'hardware' (Grove port and pin map), or 'core'. Omit to list the "
             "available modules. The full reference is far too large to return "
             "at once, so ask for the part you need.");
    return schema_of(NULL, 0, props);
}

/* Read the reference file. Caller frees. */
static char *lua_api_slurp(const char **path_out)
{
    for (size_t i = 0; i < sizeof(k_api_paths) / sizeof(k_api_paths[0]); i++) {
        FILE *f = fopen(k_api_paths[i], "rb");
        if (!f) {
            continue;
        }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len <= 0 || len > 32768) {
            fclose(f);
            continue;
        }
        char *buf = malloc((size_t)len + 1);
        if (!buf) {
            fclose(f);
            return NULL;
        }
        size_t got = fread(buf, 1, (size_t)len, f);
        fclose(f);
        buf[got] = '\0';
        if (path_out) {
            *path_out = k_api_paths[i];
        }
        return buf;
    }
    return NULL;
}

/* Case-insensitive match of a "## " heading against `want`, allowing either
 * "led" or "breezy.led". */
static bool heading_matches(const char *line, const char *want)
{
    if (strncmp(line, "## ", 3) != 0) {
        return false;
    }
    const char *h = line + 3;
    if (strncmp(h, "breezy.", 7) == 0) {
        h += 7;
    }
    /* The index prints sections as "breezy.network", so that is what gets asked
     * for. Normalise both sides rather than expecting the bare name. */
    if (strncmp(want, "breezy.", 7) == 0) {
        want += 7;
    }
    size_t n = strlen(want);
    if (strncasecmp(h, want, n) != 0) {
        return false;
    }
    char after = h[n];
    return after == '\0' || after == '\n' || after == '\r' || after == ' ';
}

/* Append the available module names, read from the reference. */
static size_t append_module_list(char *out, size_t out_len, size_t used)
{
    char *doc = lua_api_slurp(NULL);
    if (!doc) {
        return used;
    }
    used += (size_t)snprintf(out + used, out_len - used, "Available: ");
    bool first = true;
    for (char *line = doc; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (strncmp(line, "## breezy.", 10) == 0) {
            size_t n = nl ? (size_t)(nl - line - 10) : strlen(line + 10);
            int w = snprintf(out + used, out_len - used, "%s%.*s",
                             first ? "" : ", ", (int)n, line + 10);
            if (w > 0 && (size_t)w < out_len - used) {
                used += (size_t)w;
                first = false;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(doc);
    used += (size_t)snprintf(out + used, out_len - used,
                             ".\nFile operations are top-level: breezy.read_file, "
                             "write_file, listdir, exists, mkdir, remove, rename, "
                             "stat.\n");
    return used;
}

static bool lua_api_run(const cJSON *args, char *out, size_t out_len)
{
    const char *path = NULL;
    char *doc = lua_api_slurp(&path);
    if (!doc) {
        snprintf(out, out_len,
                 "The API reference is not installed. The module is `breezy`, "
                 "loaded with require(\"breezy\"); useful calls include "
                 "breezy.battery.read_pct(), breezy.read_file(path), "
                 "breezy.led.open(pin, count) and breezy.gfx.*. "
                 "There is no io or os library.");
        return true;
    }

    const char *want = arg_str(args, "module");
    size_t used = 0;

    if (!want || !want[0]) {
        /* Index only: the whole reference does not fit in a tool result, and
         * sending a truncated one is worse than sending a map -- the model
         * concludes the missing modules do not exist. */
        used = (size_t)snprintf(out, out_len,
                                "(source: %s)\n"
                                "Ask for one section at a time with the "
                                "'module' argument.\n\nAvailable:\n", path);
        for (char *line = doc; line && *line; ) {
            char *nl = strchr(line, '\n');
            if (strncmp(line, "## ", 3) == 0) {
                size_t n = nl ? (size_t)(nl - line - 3) : strlen(line + 3);
                int w = snprintf(out + used, out_len - used, "  %.*s\n", (int)n, line + 3);
                if (w > 0 && (size_t)w < out_len - used) {
                    used += (size_t)w;
                }
            }
            line = nl ? nl + 1 : NULL;
        }
        snprintf(out + used, out_len - used,
                 "\nStart every script with: local breezy = require(\"breezy\")\n"
                 "There is no io or os library; use print().\n");
        free(doc);
        return true;
    }

    /* One section, heading to the next heading. */
    char *start = NULL;
    char *stop = NULL;
    for (char *line = doc; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (!start) {
            if (heading_matches(line, want)) {
                start = line;
            }
        } else if (strncmp(line, "## ", 3) == 0) {
            stop = line;
            break;
        }
        line = nl ? nl + 1 : NULL;
    }

    if (!start) {
        size_t n = (size_t)snprintf(out, out_len, "No section '%s'.\n", want);
        n = append_module_list(out, out_len, n);
        snprintf(out + n, out_len - n,
                 "Also: Core, Grove port, Power, Pins in use, Notes.\n");
        free(doc);
        return true;
    }

    size_t len = stop ? (size_t)(stop - start) : strlen(start);
    used = (size_t)snprintf(out, out_len, "(source: %s)\n", path);
    size_t space = out_len - used - 48;
    bool cut = len > space;
    if (cut) {
        len = space;
    }
    memcpy(out + used, start, len);
    used += len;
    out[used] = '\0';
    if (cut) {
        snprintf(out + used, 48, "\n[section truncated]");
    }
    free(doc);
    return true;
}

/* ---------------------------------------------------------------- run_lua -- */

/*
 * The model writes Lua, it runs on the device, and it can be kept.
 *
 * This is upstream ESP-Claw's headline idea -- the agent programming its own
 * host -- and the reason the Lua runtime stayed in the firmware after the agent
 * core moved to C. A saved script is an ordinary Lua file: runnable later with
 * `lua /sd/claw/skills/name.lua`, with no agent involved.
 */
static cJSON *run_lua_schema(void)
{
    cJSON *props = cJSON_CreateObject();
    add_prop(props, "code", "string",
             "Lua 5.4 source to run. Start with: local breezy = require(\"breezy\") "
             "-- it is a module, not a global. There is no io or os library, so "
             "use print() for output. If unsure of a function, call lua_api "
             "(module= for one section); for worked examples, lua_api's Examples "
             "section lists runnable scripts under /root/lua that read_file can "
             "show you.");
    add_prop(props, "save_as", "string",
             "Optional name to keep this script as a reusable skill, e.g. "
             "'blink'. Saved to /sd/claw/skills/<name>.lua and runnable later "
             "with 'lua /sd/claw/skills/<name>.lua'.");
    static const char *req[] = { "code" };
    return schema_of(req, 1, props);
}

static bool run_lua_run(const cJSON *args, char *out, size_t out_len)
{
    const char *code = arg_str(args, "code");
    if (!code || !code[0]) {
        snprintf(out, out_len, "error: 'code' is required");
        return false;
    }

    mkdir("/sd/claw", 0777);
    mkdir("/sd/claw/tmp", 0777);

    /* Keep it first if asked, so a script that crashes the run is still saved
     * for the user to inspect. */
    const char *save_as = arg_str(args, "save_as");
    char saved_path[128] = {0};
    if (save_as && save_as[0] && !strchr(save_as, '/') && !strstr(save_as, "..")) {
        mkdir("/sd/claw/skills", 0777);
        snprintf(saved_path, sizeof(saved_path), "/sd/claw/skills/%s.lua", save_as);
        FILE *sf = fopen(saved_path, "wb");
        if (sf) {
            fwrite(code, 1, strlen(code), sf);
            fclose(sf);
        } else {
            saved_path[0] = '\0';
        }
    }

    const char *script = "/sd/claw/tmp/run.lua";
    FILE *f = fopen(script, "wb");
    if (!f) {
        snprintf(out, out_len, "error: cannot write the script to run");
        return false;
    }
    fwrite(code, 1, strlen(code), f);
    fclose(f);

    const char *outfile = "/sd/claw/tmp/lua.txt";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "lua %s > %s", script, outfile);
    breezybox_exec(cmd);

    size_t used = 0;
    FILE *rf = fopen(outfile, "rb");
    if (rf) {
        used = fread(out, 1, out_len - 128, rf);
        fclose(rf);
        remove(outfile);
    }
    out[used] = '\0';

    if (used == 0) {
        snprintf(out, out_len, "(script produced no output)");
        used = strlen(out);
    }

    /*
     * A guessed module name fails as "attempt to index a nil value (field
     * 'fs')", which says what broke but not what exists. Naming the real
     * modules turns a dead end into a correction.
     */
    const char *nilfield = strstr(out, "index a nil value (field '");
    if (nilfield && used + 200 < out_len) {
        const char *name = nilfield + strlen("index a nil value (field '");
        const char *end = strchr(name, '\'');
        if (end && (size_t)(end - name) < 32) {
            used += (size_t)snprintf(out + used, out_len - used,
                                     "\n\nThere is no breezy.%.*s. ",
                                     (int)(end - name), name);
            used = append_module_list(out, out_len, used);
        }
    }

    if (saved_path[0]) {
        snprintf(out + used, out_len - used, "\n[saved as %s]", saved_path);
    }
    remove(script);
    return true;
}

/* ------------------------------------------------------------ device_info -- */

static cJSON *device_info_schema(void)
{
    return schema_of(NULL, 0, cJSON_CreateObject());
}

static bool device_info_run(const cJSON *args, char *out, size_t out_len)
{
    (void)args;
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    snprintf(out, out_len,
             "board: M5Stack Cardputer (ESP32-S3, no PSRAM)\n"
             "heap free: %u bytes\n"
             "heap largest block: %u bytes\n"
             "storage: /root (internal flash), /sd (card, if inserted)\n",
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps));
    return true;
}

/* -------------------------------------------------------------- registry -- */

static const claw_tool_t k_tools[] = {
    { "read_file",   "Read a text file from the device.",              read_file_schema,   read_file_run   },
    { "write_file",  "Write or append text to a file on the device.",  write_file_schema,  write_file_run  },
    { "list_dir",    "List the contents of a directory.",              list_dir_schema,    list_dir_run    },
    { "run_shell",   "Run a command in the BreezyBox shell and return its output. This is an embedded shell, not Unix: there is no bash, no shebang scripts and no package manager. Run 'help' to list the available commands. For network requests use run_lua with breezy.https instead.", run_shell_schema, run_shell_run },
    { "memory_save", "Remember something for future conversations. Use when you learn a durable fact about the user, their hardware or their project.", memory_save_schema, memory_save_run },
    { "memory_read", "Read the full text of something in your memory index.", memory_read_schema, memory_read_run },
    { "memory_forget", "Delete something from memory. Asks the user first.", memory_forget_schema, memory_forget_run },
    { "i2c_scan",    "Scan the Grove port for I2C devices. Detects I2C peripherals only; devices that do not drive the bus (LEDs, servos, relays, some displays) cannot be found this way and should be driven directly with run_lua.", i2c_scan_schema, i2c_scan_run },
    { "lua_api",     "Get the breezy Lua API reference for this device. Call with no arguments to list the available sections, then again with module= for the one you need (e.g. 'led' for addressable LEDs, 'hardware' for the Grove pinout). Always check here before writing a Lua script.", lua_api_schema, lua_api_run },
    { "run_lua",     "Run a Lua script on the device, optionally saving it as a reusable skill. Use this to control hardware or compute something the other tools cannot.", run_lua_schema, run_lua_run },
    { "device_info", "Report board, memory and storage information.",  device_info_schema, device_info_run },
};

size_t claw_tools_count(void)
{
    return sizeof(k_tools) / sizeof(k_tools[0]);
}

const claw_tool_t *claw_tools_at(size_t i)
{
    return (i < claw_tools_count()) ? &k_tools[i] : NULL;
}

const claw_tool_t *claw_tools_find(const char *name)
{
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < claw_tools_count(); i++) {
        if (strcmp(k_tools[i].name, name) == 0) {
            return &k_tools[i];
        }
    }
    return NULL;
}

bool claw_tools_run(const char *name, const cJSON *args, char *out, size_t out_len)
{
    const claw_tool_t *t = claw_tools_find(name);
    if (!t) {
        /* Tell the model rather than failing the turn: it can usually pick a
         * different tool. */
        snprintf(out, out_len, "error: no such tool '%s'", name ? name : "(null)");
        return false;
    }
    return t->run(args, out, out_len);
}
