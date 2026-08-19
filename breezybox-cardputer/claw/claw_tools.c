#include "claw_tools.h"
#include "claw_config.h"
#include "claw_memory.h"
#include "breezy_exec.h"
#include "breezy_vfs.h"

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
             "Shell command to run in the BreezyBox shell, e.g. 'ls /sd', 'df', 'wifi status'");
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

    /* Reuse the Lua binding rather than duplicating the driver setup: it
     * already handles bus install, teardown and the odd address ranges. */
    char script[256];
    snprintf(script, sizeof(script),
             "local b=require('breezy') "
             "b.i2c.open(%d,%d,{freq=100000}) "
             "local d=b.i2c.scan() "
             "if #d==0 then print('no I2C devices found') else "
             "for _,a in ipairs(d) do print(string.format('0x%%02X',a)) end end "
             "b.i2c.close()",
             sda, scl);

    mkdir("/sd/claw", 0777);
    mkdir("/sd/claw/tmp", 0777);
    const char *path = "/sd/claw/tmp/i2cscan.lua";
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(out, out_len, "error: cannot stage the scan script");
        return false;
    }
    fwrite(script, 1, strlen(script), f);
    fclose(f);

    const char *outfile = "/sd/claw/tmp/i2cscan.txt";
    char cmd[192];
    snprintf(cmd, sizeof(cmd), "lua %s > %s", path, outfile);
    breezybox_exec(cmd);

    size_t used = 0;
    FILE *rf = fopen(outfile, "rb");
    if (rf) {
        used = fread(out, 1, out_len - 96, rf);
        fclose(rf);
        remove(outfile);
    }
    out[used] = '\0';
    remove(path);

    if (used == 0) {
        snprintf(out, out_len, "no output from the scan (is the bus wired?)");
        return true;
    }
    snprintf(out + used, out_len - used,
             "\n(scanned SDA=G%d SCL=G%d)", sda, scl);
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
    return schema_of(NULL, 0, cJSON_CreateObject());
}

static bool lua_api_run(const cJSON *args, char *out, size_t out_len)
{
    (void)args;
    for (size_t i = 0; i < sizeof(k_api_paths) / sizeof(k_api_paths[0]); i++) {
        FILE *f = fopen(k_api_paths[i], "rb");
        if (!f) {
            continue;
        }
        size_t got = fread(out, 1, out_len - 48, f);
        long more = 0;
        if (got == out_len - 48) {
            fseek(f, 0, SEEK_END);
            more = ftell(f) - (long)got;
        }
        fclose(f);
        out[got] = '\0';
        if (more > 0) {
            snprintf(out + got, 48, "\n[truncated, %ld more bytes]", more);
        }
        return true;
    }
    snprintf(out, out_len,
             "The API reference is not installed. The module is `breezy`, "
             "loaded with require(\"breezy\"); useful calls include "
             "breezy.battery.read_pct(), breezy.read_file(path), "
             "breezy.write_file(path, text), breezy.listdir(path), "
             "breezy.exec(cmd), breezy.heap() and breezy.gfx.*. "
             "There is no io or os library.");
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
             "-- it is a module, not a global. Call the lua_api tool first if you "
             "are unsure of a function name; there is no io or os library, so use "
             "print() for output.");
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
    { "run_shell",   "Run a BreezyBox shell command and return its output.", run_shell_schema, run_shell_run },
    { "memory_save", "Remember something for future conversations. Use when you learn a durable fact about the user, their hardware or their project.", memory_save_schema, memory_save_run },
    { "memory_read", "Read the full text of something in your memory index.", memory_read_schema, memory_read_run },
    { "memory_forget", "Delete something from memory. Asks the user first.", memory_forget_schema, memory_forget_run },
    { "i2c_scan",    "Scan the Grove port for connected I2C devices and report their addresses.", i2c_scan_schema, i2c_scan_run },
    { "lua_api",     "Get the exact breezy Lua API reference for this device. Call this before writing a Lua script if unsure of a function name.", lua_api_schema, lua_api_run },
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
