#include "claw_prompt.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "breezybox.h"
#include "claw_text.h"
#include "claw_util.h"

/* Appended to the prompt. Distinct from /sd/claw/prompt.md, which replaces the
 * device description below. */
static const char *const k_user_prompt[] = {
    "/sd/claw/system.md",
    "/root/.claw_system.md",
};

/* Replaces k_device_prompt when present. Not auto-seeded: an absent file must
 * mean the compiled text, so a firmware update carrying better wording still
 * takes effect. `claw text dump` writes a starting point on request. */
#define PROMPT_OVERRIDE "/sd/claw/prompt.md"

/*
 * Facts, not manners. Each line exists because getting it wrong cost a turn:
 * the shell is not Unix, Lua is sandboxed, the API is discoverable, and the
 * screen is 40 columns wide.
 */
static const char k_device_prompt[] =
    "You are running on an M5Stack Cardputer: an ESP32-S3 microcontroller with "
    "a 240x135 screen (40x16 characters), a small keyboard, WiFi, an SD card "
    "slot and a Grove expansion port. You are not on Linux.\n"
    "\n"
    "- run_shell runs the BreezyBox shell, not bash. There is no curl, no "
    "package manager, and shell scripts are not executable.\n"
    "- run_lua runs Lua 5.4. The device API is the `breezy` module, loaded with "
    "require(\"breezy\"). There is no io or os library; use print().\n"
    "- Call lua_api before writing Lua. With no argument it lists sections; "
    "pass module= for one (e.g. 'led', 'https', 'hardware').\n"
    "- File operations are top-level: breezy.read_file, write_file, listdir, "
    "mkdir, remove, rename, stat. There is no breezy.fs.\n"
    "- For network requests use breezy.https.request, which streams the "
    "response body to you. Storage is /root (internal, erased by a firmware "
    "update) and /sd (card, persistent).\n"
    "- Hardware lives on the Grove port, pins G1 and G2. Accessories draw from "
    "a shared 5V rail, so warn before switching on anything bright or motorised.\n"
    "\n"
    "- For text output use breezy.tui and print(): the console is already "
    "40x16 characters, and text is what fits a 40-column screen.\n"
    "- A script that calls breezy.gfx.mode() needs a 36 KB contiguous block "
    "that is not available while you are running. Do not try it with run_lua. "
    "Write the script, save it with save_as, and tell the user to run it from "
    "the shell: lua /sd/claw/skills/<name>.lua\n"
    "- When a call fails with \"there is no breezy.X\", the error lists what "
    "does exist. Use that list; do not guess a second name. The same applies "
    "to an unknown shell command, which lists the real ones.\n"
    "- Otherwise never show a script and stop. Run it with run_lua: that is "
    "the only way either of us finds out whether it works, and a script you "
    "have not run is not an answer. Graphics is the one exception above.\n"
    "\n"
    "Answers are read on a 40-column screen: keep them short. Prefer doing the "
    "thing over describing it, and when something fails, read the error before "
    "concluding a capability is missing.\n";

/*
 * The actual command list, read from the registry rather than written down.
 *
 * Naming them up front is what stops the model reaching for curl or apt: it can
 * see what exists instead of inferring from a failure, and the list cannot go
 * stale because it is the same table the shell dispatches on.
 */
static size_t append_shell_commands(char *out, size_t out_len, size_t used)
{
    used += (size_t)snprintf(out + used, out_len - used,
                             "\nShell commands (run_shell), 'help <name>' for usage:\n");

    size_t n = 0;
    bool first = true;
    const esp_console_cmd_t *cmds = breezybox_get_core_commands(&n);
    for (size_t i = 0; i < n && used + 20 < out_len; i++) {
        used += (size_t)snprintf(out + used, out_len - used, "%s%s",
                                 first ? "" : " ", cmds[i].command);
        first = false;
    }
    cmds = breezybox_get_extra_commands(&n);
    for (size_t i = 0; i < n && used + 20 < out_len; i++) {
        used += (size_t)snprintf(out + used, out_len - used, " %s",
                                 cmds[i].command);
    }
    used += (size_t)snprintf(out + used, out_len - used, "\n");
    return used;
}

size_t claw_prompt_build(char *out, size_t out_len)
{
    size_t used = 0;

    char *override = claw_read_file(PROMPT_OVERRIDE, 4096, NULL);
    if (override && override[0]) {
        used = (size_t)snprintf(out, out_len, "%s", override);
    } else {
        used = (size_t)snprintf(out, out_len, "%s", k_device_prompt);
    }
    free(override);

    /* The command list is enumerated from the live registry either way: it is
     * a fact about this build, not something worth overriding. */
    used = append_shell_commands(out, out_len, used);

    /* Append the user's own instructions, if they left any. */
    for (size_t i = 0; i < sizeof(k_user_prompt) / sizeof(k_user_prompt[0]); i++) {
        FILE *f = fopen(k_user_prompt[i], "rb");
        if (!f) {
            continue;
        }
        used += (size_t)snprintf(out + used, out_len - used, "\n");
        size_t space = (out_len > used + 1) ? (out_len - used - 1) : 0;
        size_t got = fread(out + used, 1, space, f);
        fclose(f);
        used += got;
        out[used] = '\0';
        break;
    }
    return used;
}
