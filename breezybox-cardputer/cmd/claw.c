/*
 * cmd/claw.c - the `claw` console command.
 *
 * The agent core is C (claw/). It was prototyped in Lua and moved here because
 * the Lua runtime cost ~70 KB of heap and fragmented what remained: with it
 * resident the largest contiguous block fell from ~57 KB to ~7.7 KB, and the
 * TLS handshake could not get the memory it needed. See docs/claw-architecture.md.
 *
 * Lua is still the skill runtime -- the `lua` command is untouched.
 */

#include "breezy_cmd.h"
#include "claw_agent.h"
#include "claw_backend.h"
#include "claw_config.h"
#include "claw_memory.h"
#include "claw_models.h"
#include "claw_tools.h"
#include "claw_session.h"
#include "claw_text.h"

#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "rgb_display.h"
#include "linenoise/linenoise.h"

#include "esp_log.h"

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>

#define CLAW_VERSION "0.3.0"

/* Settings shown by `claw config show`, in display order. */
static const char *k_shown_keys[] = {
    "backend", "model.anthropic", "model.openai", "model.gemini",
    "max_tokens", "context_budget", "base_url", "ca_file",
    "timeout_ms", "auto_approve", "store", "anthropic.key", "openai.key", "gemini.key",
};

/* Inspect and seed the SD text overrides. */
static int cmd_text(int argc, char **argv)
{
    const char *sub = (argc > 0) ? argv[0] : "status";

    if (strcmp(sub, "status") == 0) {
        claw_text_status();
        return 0;
    }
    if (strcmp(sub, "dump") == 0) {
        int n = claw_text_dump();
        if (n > 0) {
            printf("edit them, then run 'claw text reload' or /reload in a session\n");
        }
        return 0;
    }
    if (strcmp(sub, "reload") == 0) {
        claw_text_reload();
        claw_text_status();
        return 0;
    }
    printf("usage: claw text <status|dump|reload>\n");
    return 1;
}

static int cmd_stats(void);
static int cmd_claw_run(int argc, char **argv);
static int cmd_memory(int argc, char **argv)
{
    const char *sub = (argc > 0) ? argv[0] : "list";

    if (strcmp(sub, "list") == 0) {
        claw_memory_list();
        return 0;
    }
    if (strcmp(sub, "show") == 0) {
        if (argc < 2) {
            printf("usage: claw memory show <name>\n");
            return 1;
        }
        char *buf = malloc(CLAW_MEMORY_BODY_MAX + 1);
        if (!buf) {
            printf("claw: out of memory\n");
            return 1;
        }
        if (!claw_memory_read(argv[1], buf, CLAW_MEMORY_BODY_MAX + 1)) {
            printf("claw: nothing remembered under '%s'\n", argv[1]);
            free(buf);
            return 1;
        }
        printf("%s\n", buf);
        free(buf);
        return 0;
    }
    if (strcmp(sub, "rm") == 0 || strcmp(sub, "forget") == 0) {
        if (argc < 2) {
            printf("usage: claw memory rm <name>\n");
            return 1;
        }
        if (!claw_memory_delete(argv[1])) {
            printf("claw: nothing remembered under '%s'\n", argv[1]);
            return 1;
        }
        printf("forgot %s\n", argv[1]);
        return 0;
    }
    printf("usage: claw memory <list|show|rm>\n");
    return 1;
}

static int cmd_skills(int argc, char **argv);
static int cmd_text(int argc, char **argv);
static int cmd_memory(int argc, char **argv);

static void print_usage(void)
{
    printf("claw " CLAW_VERSION " - on-device AI agent\n\n");
    printf("  claw                         interactive session\n");
    printf("  claw ask [-v] \"question\"     ask a single question\n");
    printf("  claw model [<name>]          show or set the model\n");
    printf("  claw models                  list suggested models\n");
    printf("  claw backend [<name>]        show or switch provider\n");
    printf("  claw memory <list|show|rm>   what claw remembers about you\n");
    printf("  claw text <status|dump|reload>  override prompt and tool text on SD\n");
    printf("  claw skills [rm <name>]      scripts the model has saved\n");
    printf("  claw session <new|list|show|rm>\n");
    printf("  claw config show             list settings\n");
    printf("  claw config get <key>\n");
    printf("  claw config set <key> <val>\n");
    printf("  claw stats                   status and memory\n\n");
    printf("Setup (pick one backend):\n");
    printf("  claw config set backend openai     && claw config set openai.key sk-...\n");
    printf("  claw config set backend anthropic  && claw config set anthropic.key sk-ant-...\n");
    printf("  claw config set backend gemini     && claw config set gemini.key AIza...\n\n");
    printf("Any OpenAI-compatible endpoint works via base_url:\n");
    printf("  claw config set base_url http://192.168.1.10:11434/v1/chat/completions\n");
}

static void print_models(void)
{
    const claw_backend_t *active = claw_backend_active();
    char current[64];
    claw_backend_model(active, current, sizeof(current));

    printf("catalogue: %s\n", claw_models_path());

    bool any = false;
    for (size_t i = 0; i < claw_backend_count(); i++) {
        const claw_backend_t *b = claw_backend_at(i);
        cJSON *root = NULL;
        cJSON *arr = claw_models_for(b->name, &root);

        printf("%s%s:\n", b == active ? "* " : "  ", b->name);
        if (!arr) {
            printf("      (none listed)\n");
        }
        cJSON *m = NULL;
        cJSON_ArrayForEach(m, arr) {
            if (!cJSON_IsString(m)) {
                continue;
            }
            bool sel = (b == active) && strcmp(m->valuestring, current) == 0;
            printf("    %s%s\n", sel ? "> " : "  ", m->valuestring);
            any = true;
        }
        cJSON_Delete(root);
    }

    if (!any) {
        printf("\nNo catalogue found. Create %s, or edit the copy on the card.\n",
               claw_models_path());
    }
    printf("\nAny model name is accepted; the list is only a convenience.\n");
}

/* Set the model for the active backend. */
static int set_model(const char *model)
{
    const claw_backend_t *b = claw_backend_active();
    if (!claw_backend_set_model(b, model)) {
        printf("claw: could not save model\n");
        return 1;
    }
    printf("%s model = %s\n", b->name, model);
    return 0;
}

/* Switch backend. The model is stored per backend, so this cannot leave a
 * Gemini model pointed at OpenAI. */
static int set_backend(const char *name)
{
    const claw_backend_t *b = claw_backend_find(name);
    if (!b) {
        printf("claw: unknown backend '%s'\n", name);
        printf("      available:");
        for (size_t i = 0; i < claw_backend_count(); i++) {
            printf(" %s", claw_backend_at(i)->name);
        }
        printf("\n");
        return 1;
    }
    if (!claw_config_set("backend", name)) {
        printf("claw: could not save backend\n");
        return 1;
    }
    char model[64];
    claw_backend_model(b, model, sizeof(model));
    printf("backend = %s, model = %s\n", b->name, model);

    char key[48];
    snprintf(key, sizeof(key), "%s.key", b->name);
    char tmp[CLAW_CFG_MAX_VALUE];
    if (!claw_config_get(key, tmp, sizeof(tmp), NULL) || !tmp[0]) {
        printf("claw: no API key for %s yet. Run: claw config set %s <key>\n",
               b->name, key);
    }
    return 0;
}

/*
 * Ask the user to approve a destructive action.
 *
 * Defaults to no: a bare Enter, an unreadable line, or anything that is not a
 * clear yes declines. The model is told it was refused and can suggest
 * something else.
 */
static bool console_confirm(const char *action, const char *detail)
{
    printf("\n");
    printf("claw wants to %s:\n", action);
    printf("  %s\n", detail ? detail : "(no detail)");

    char *line = linenoise("allow? [y/N] ");
    if (!line) {
        printf("declined\n");
        return false;
    }
    bool yes = (line[0] == 'y' || line[0] == 'Y');
    linenoiseFree(line);
    printf(yes ? "allowed\n" : "declined\n");
    return yes;
}

static int run_turn(const char *prompt, bool verbose)
{
    /* Registered per turn so a non-interactive caller of the agent core never
     * inherits an interactive prompt by accident. */
    claw_tools_set_confirm(console_confirm);

    claw_result_t res;
    int rc = claw_agent_ask(prompt, verbose, &res);
    if (rc != 0 && res.error[0]) {
        printf("claw: %s\n", res.error);
    } else if (rc == 0 && !res.got_text) {
        printf("(no text in response)\n");
    }
    return rc;
}

/*
 * Interactive session. Bare `claw` lands here.
 *
 * Every line is a turn in the current session, so context carries across the
 * conversation exactly as it does for `claw ask`.
 */
static int cmd_repl(void)
{
    char sid[CLAW_SESSION_ID_MAX] = {0};
    claw_session_current(sid, sizeof(sid));

    const claw_backend_t *b = claw_backend_active();
    char model[64];
    claw_backend_model(b, model, sizeof(model));

    printf("claw " CLAW_VERSION " - %s / %s\n", b->name, model);
    printf("session %s (%d turns). Ctrl-D or 'exit' to leave, /help for commands.\n",
           sid, claw_session_count());
    if (!b->add_tools) {
        printf("note: %s has no tool support here, so this is chat only.\n", b->name);
    }
    printf("\n");

    bool verbose = false;
    /* The last question asked, so a turn lost to a dropped connection can be
     * sent again without retyping it. */
    char last_prompt[512] = {0};

    while (true) {
        char *line = linenoise("claw> ");
        if (!line) {                      /* Ctrl-D */
            printf("\n");
            break;
        }

        /* Trim trailing whitespace so a stray space is not sent as a turn. */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            linenoiseFree(line);
            continue;
        }

        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
            linenoiseFree(line);
            break;
        }

        linenoiseHistoryAdd(line);

        /* Slash commands keep controls separate from conversation, so a
         * question that happens to read like a command is still a question. */
        if (line[0] == '/') {
            if (strcmp(line, "/new") == 0) {
                if (claw_session_new(sid, sizeof(sid))) {
                    printf("started session %s\n\n", sid);
                }
            } else if (strcmp(line, "/verbose") == 0) {
                verbose = !verbose;
                printf("verbose %s\n\n", verbose ? "on" : "off");
            } else if (strcmp(line, "/models") == 0) {
                print_models();
                printf("\n");
            } else if (strncmp(line, "/model", 6) == 0 &&
                       (line[6] == '\0' || line[6] == ' ')) {
                const char *arg = line + 6;
                while (*arg == ' ') {
                    arg++;
                }
                if (*arg) {
                    set_model(arg);
                    printf("\n");
                } else {
                    const claw_backend_t *b = claw_backend_active();
                    char m[64];
                    claw_backend_model(b, m, sizeof(m));
                    printf("%s / %s   (/models to list, /model <name> to change)\n\n",
                           b->name, m);
                }
            } else if (strncmp(line, "/backend", 8) == 0 &&
                       (line[8] == '\0' || line[8] == ' ')) {
                const char *arg = line + 8;
                while (*arg == ' ') {
                    arg++;
                }
                if (*arg) {
                    set_backend(arg);
                    printf("\n");
                } else {
                    printf("backend %s   (/backend <name> to change:",
                           claw_backend_active()->name);
                    for (size_t i = 0; i < claw_backend_count(); i++) {
                        printf(" %s", claw_backend_at(i)->name);
                    }
                    printf(")\n\n");
                }
            } else if (strcmp(line, "/stats") == 0) {
                cmd_stats();
                printf("\n");
            } else if (strcmp(line, "/retry") == 0) {
                if (!last_prompt[0]) {
                    printf("nothing to retry yet\n\n");
                } else {
                    printf("retrying: %s\n", last_prompt);
                    run_turn(last_prompt, verbose);
                    printf("\n");
                }
            } else if (strcmp(line, "/reload") == 0) {
                claw_text_reload();
                printf("reloaded text overrides\n\n");
            } else if (strcmp(line, "/memory") == 0) {
                claw_memory_list();
                printf("\n");
            } else if (strcmp(line, "/skills") == 0) {
                cmd_skills(0, NULL);
                printf("\n");
            } else if (strcmp(line, "/show") == 0) {
                claw_session_show(NULL);
                printf("\n");
            } else if (strcmp(line, "/help") == 0) {
                printf("  /model            show the current model\n");
                printf("  /model <name>     change model\n");
                printf("  /models           list suggested models\n");
                printf("  /backend <name>   switch provider\n");
                printf("  /new      start a new session\n");
                printf("  /show     print this session\n");
                printf("  /skills   list saved skills\n");
                printf("  /memory   list what claw remembers\n");
                printf("  /reload   re-read prompt and tool text from SD\n");
                printf("  /retry    send the last question again\n");
                printf("  /stats    status and memory\n");
                printf("  /verbose  toggle transport statistics\n");
                printf("  exit      leave\n\n");
            } else {
                printf("unknown command: %s  (try /help)\n\n", line);
            }
            linenoiseFree(line);
            continue;
        }

        snprintf(last_prompt, sizeof(last_prompt), "%s", line);
        if (run_turn(line, verbose) != 0) {
            printf("  (/retry to send that again)\n");
        }
        printf("\n");
        linenoiseFree(line);
    }

    return 0;
}

static int cmd_ask(int argc, char **argv)
{
    bool verbose = false;
    char prompt[512] = {0};
    size_t used = 0;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
            continue;
        }
        int n = snprintf(prompt + used, sizeof(prompt) - used,
                         "%s%s", used ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= sizeof(prompt) - used) {
            used = sizeof(prompt) - 1;
            break;
        }
        used += (size_t)n;
    }

    if (!prompt[0]) {
        printf("usage: claw ask [-v] \"your question\"\n");
        return 1;
    }
    return run_turn(prompt, verbose);
}

/* Scripts the model has written and kept. They are ordinary Lua files: a skill
 * can be run directly with `lua /sd/claw/skills/<name>.lua`, no agent needed. */
static int cmd_skills(int argc, char **argv)
{
    const char *dir = "/sd/claw/skills";

    if (argc > 0 && strcmp(argv[0], "rm") == 0) {
        if (argc < 2) {
            printf("usage: claw skills rm <name>\n");
            return 1;
        }
        char p[160];
        snprintf(p, sizeof(p), "%s/%s.lua", dir, argv[1]);
        if (remove(p) != 0) {
            printf("claw: no such skill: %s\n", argv[1]);
            return 1;
        }
        printf("deleted %s\n", argv[1]);
        return 0;
    }

    DIR *d = opendir(dir);
    if (!d) {
        printf("no skills yet.\n");
        printf("Ask claw to write one, e.g. \"write a lua script that shows the "
               "battery level and save it as battery\"\n");
        return 0;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *dot = strstr(e->d_name, ".lua");
        if (!dot || dot[4] != '\0') {
            continue;
        }
        char p[320];   /* dir + NAME_MAX, so a long filename cannot truncate */
        if (snprintf(p, sizeof(p), "%s/%s", dir, e->d_name) >= (int)sizeof(p)) {
            continue;
        }
        struct stat st;
        long size = (stat(p, &st) == 0) ? (long)st.st_size : 0;
        printf("  %-24s %ld bytes   lua %s\n", e->d_name, size, p);
        n++;
    }
    closedir(d);
    if (n == 0) {
        printf("no skills yet\n");
    }
    return 0;
}

static int cmd_session(int argc, char **argv)
{
    const char *sub = (argc > 0) ? argv[0] : "show";

    if (strcmp(sub, "new") == 0) {
        char id[CLAW_SESSION_ID_MAX];
        if (!claw_session_new(id, sizeof(id))) {
            printf("claw: could not create a session\n");
            return 1;
        }
        printf("started session %s\n", id);
        return 0;
    }
    if (strcmp(sub, "list") == 0) {
        claw_session_list();
        return 0;
    }
    if (strcmp(sub, "show") == 0) {
        return claw_session_show(argc > 1 ? argv[1] : NULL) ? 0 : 1;
    }
    if (strcmp(sub, "rm") == 0) {
        if (argc < 2) {
            printf("usage: claw session rm <id>\n");
            return 1;
        }
        if (!claw_session_delete(argv[1])) {
            printf("claw: no such session: %s\n", argv[1]);
            return 1;
        }
        printf("deleted %s\n", argv[1]);
        return 0;
    }
    printf("usage: claw session <new|list|show|rm>\n");
    return 1;
}

static int cmd_config(int argc, char **argv)
{
    const char *sub = (argc > 0) ? argv[0] : "show";

    if (strcmp(sub, "show") == 0) {
        const char *path = claw_config_path();
        printf("config file: %s  (%s)\n", path,
               strncmp(path, "/sd/", 4) == 0 ? "survives reflash" : "wiped by make flash");
        for (size_t i = 0; i < sizeof(k_shown_keys) / sizeof(k_shown_keys[0]); i++) {
            char raw[CLAW_CFG_MAX_VALUE];
            char shown[CLAW_CFG_MAX_VALUE];
            bool set = claw_config_get(k_shown_keys[i], raw, sizeof(raw), NULL);
            if (!set || !raw[0]) {
                snprintf(shown, sizeof(shown), "(unset)");
            } else {
                claw_config_mask(k_shown_keys[i], raw, shown, sizeof(shown));
            }
            printf("  %-14s %s\n", k_shown_keys[i], shown);
        }
        return 0;
    }

    if (strcmp(sub, "get") == 0) {
        if (argc < 2) {
            printf("usage: claw config get <key>\n");
            return 1;
        }
        char raw[CLAW_CFG_MAX_VALUE];
        char shown[CLAW_CFG_MAX_VALUE];
        if (!claw_config_get(argv[1], raw, sizeof(raw), NULL) || !raw[0]) {
            printf("(unset)\n");
            return 0;
        }
        claw_config_mask(argv[1], raw, shown, sizeof(shown));
        printf("%s\n", shown);
        return 0;
    }

    if (strcmp(sub, "set") == 0) {
        if (argc < 3) {
            printf("usage: claw config set <key> <value>\n");
            return 1;
        }
        /* Join the remaining words so unquoted values still work. */
        char value[CLAW_CFG_MAX_VALUE] = {0};
        size_t used = 0;
        for (int i = 2; i < argc; i++) {
            int n = snprintf(value + used, sizeof(value) - used,
                             "%s%s", used ? " " : "", argv[i]);
            if (n < 0 || (size_t)n >= sizeof(value) - used) {
                break;
            }
            used += (size_t)n;
        }
        if (!claw_config_set(argv[1], value)) {
            printf("claw: could not write %s\n", claw_config_path());
            return 1;
        }
        char shown[CLAW_CFG_MAX_VALUE];
        claw_config_mask(argv[1], value, shown, sizeof(shown));
        printf("%s = %s\n", argv[1], shown);
        return 0;
    }

    printf("usage: claw config <show|get|set>\n");
    return 1;
}

static int cmd_stats(void)
{
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const claw_backend_t *b = claw_backend_active();

    char model[64], url[256];
    claw_backend_model(b, model, sizeof(model));
    b->endpoint(url, sizeof(url));

    esp_netif_t *n = esp_netif_get_default_netif();
    esp_netif_ip_info_t ip = {0};
    bool online = n && esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr != 0;

    printf("claw " CLAW_VERSION " (native)\n");
    printf("  built      " __DATE__ " " __TIME__ "\n");
    printf("  backend    %s\n", b->name);
    printf("  model      %s\n", model);
    printf("  endpoint   %s\n", url);
    printf("  network    %s\n", online ? "connected" : "offline");
    printf("  tools      %s\n",
           b->add_tools ? "yes" : "no (this backend has no tool support yet)");
    printf("  config     %s\n", claw_config_path());
    {
        char sid[CLAW_SESSION_ID_MAX];
        if (claw_session_current(sid, sizeof(sid))) {
            printf("  session    %s (%d turns)\n", sid, claw_session_count());
        }
    }
    printf("  memory     %d stored\n", claw_memory_count());
    {
        /* A working copy on the card shadows the shipped files; if it is old,
         * the model is reading a stale API reference. */
        static const char *const dirs[] = { "/sd/espclaw", "/sd/apps/espclaw" };
        for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
            char p[80];
            struct stat st;
            snprintf(p, sizeof(p), "%s/lua_api.md", dirs[i]);
            if (stat(p, &st) == 0) {
                printf("  lua_api    %s  (overrides the shipped copy)\n", p);
                break;
            }
        }
    }
    printf("  heap       free %u, min %u, largest %u\n",
           (unsigned)heap_caps_get_free_size(caps),
           (unsigned)heap_caps_get_minimum_free_size(caps),
           (unsigned)heap_caps_get_largest_free_block(caps));
    return 0;
}

int cmd_claw(int argc, char **argv)
{
    /*
     * Borrow the graphics framebuffer while we run.
     *
     * It is reserved at boot so a pixel mode is possible at all, but 36 KB of
     * contiguous heap is also the difference between the agent working and
     * failing to serialise a request. Give it back on the way out.
     */
    rgb_display_release_gfx();
    int rc = cmd_claw_run(argc, argv);

    /*
     * Drop everything cached before trying to take the buffer back: a few KB
     * held in the middle of the region is enough to stop 36 KB coalescing.
     * This is not defragmentation -- ESP-IDF cannot compact a heap -- it just
     * improves the odds.
     */
    claw_text_reload();

    if (!rgb_display_reserve_gfx()) {
        /*
         * Say so now. The alternative is the user discovering it later, from a
         * graphics script that fails for reasons that look unrelated to having
         * run the agent.
         */
        const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        printf("note: the graphics framebuffer could not be reclaimed "
               "(largest block %u of 36000 needed).\n"
               "      Reboot before running a graphics script.\n",
               (unsigned)heap_caps_get_largest_free_block(caps));
    }
    return rc;
}

static int cmd_claw_run(int argc, char **argv)
{
    if (argc < 2) {
        return cmd_repl();
    }
    if (strcmp(argv[1], "help") == 0 ||
        strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_usage();
        return 0;
    }

    const char *sub = argv[1];
    int rest_argc = argc - 2;
    char **rest_argv = argv + 2;

    if (strcmp(sub, "ask") == 0) {
        return cmd_ask(rest_argc, rest_argv);
    }
    if (strcmp(sub, "model") == 0) {
        if (rest_argc < 1) {
            const claw_backend_t *b = claw_backend_active();
            char m[64];
            claw_backend_model(b, m, sizeof(m));
            printf("%s / %s\n", b->name, m);
            return 0;
        }
        return set_model(rest_argv[0]);
    }
    if (strcmp(sub, "models") == 0) {
        print_models();
        return 0;
    }
    if (strcmp(sub, "backend") == 0) {
        if (rest_argc < 1) {
            printf("%s\n", claw_backend_active()->name);
            return 0;
        }
        return set_backend(rest_argv[0]);
    }
    if (strcmp(sub, "memory") == 0) {
        return cmd_memory(rest_argc, rest_argv);
    }
    if (strcmp(sub, "text") == 0) {
        return cmd_text(rest_argc, rest_argv);
    }
    if (strcmp(sub, "skills") == 0) {
        return cmd_skills(rest_argc, rest_argv);
    }
    if (strcmp(sub, "session") == 0) {
        return cmd_session(rest_argc, rest_argv);
    }
    if (strcmp(sub, "config") == 0) {
        return cmd_config(rest_argc, rest_argv);
    }
    if (strcmp(sub, "stats") == 0) {
        return cmd_stats();
    }

    printf("claw: unknown command '%s'\n", sub);
    print_usage();
    return 1;
}
