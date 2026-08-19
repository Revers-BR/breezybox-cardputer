/*
 * Host tests for the destructive-command classifier and path confinement.
 *
 * These are the two places where a mistake costs the user data rather than a
 * retry, so they are tested away from the device.
 *
 * The functions under test are pure, so the file is compiled directly rather
 * than linking the whole tool registry (which pulls in ESP-IDF).
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* --- copies of the logic under test, kept byte-identical to claw_tools.c --- */

static const char *const k_destructive[] = {
    "rm", "rmdir", "mv", "format", "erase", "mkfs", "dd", "fullclean",
};

static bool shell_is_destructive(const char *command)
{
    if (!command) {
        return false;
    }
    while (*command == ' ') {
        command++;
    }
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

static bool path_allowed(const char *path)
{
    if (!path || path[0] != '/') {
        return false;
    }
    if (strstr(path, "..")) {
        return false;
    }
    return strncmp(path, "/root", 5) == 0 || strncmp(path, "/sd", 3) == 0;
}

/* --------------------------------------------------------------- harness -- */

static int failures = 0;

static void expect(const char *what, bool got, bool want)
{
    if (got == want) {
        printf("  ok   %s\n", what);
    } else {
        failures++;
        printf("  FAIL %s  -- got %s, want %s\n", what,
               got ? "true" : "false", want ? "true" : "false");
    }
}

int main(void)
{
    printf("destructive commands need confirmation\n");
    expect("rm",                    shell_is_destructive("rm /sd/x"),        true);
    expect("rmdir",                 shell_is_destructive("rmdir /sd/d"),     true);
    expect("mv",                    shell_is_destructive("mv a b"),          true);
    expect("erase",                 shell_is_destructive("erase"),           true);
    expect("leading whitespace",    shell_is_destructive("   rm x"),         true);
    expect("output redirect",       shell_is_destructive("echo hi > /sd/f"), true);
    expect("append redirect",       shell_is_destructive("cat a >> b"),      true);

    printf("\nharmless commands do not\n");
    expect("ls",        shell_is_destructive("ls /sd"),        false);
    expect("cat",       shell_is_destructive("cat /sd/f"),     false);
    expect("df",        shell_is_destructive("df"),            false);
    expect("wifi",      shell_is_destructive("wifi status"),   false);
    expect("NULL",      shell_is_destructive(NULL),            false);
    expect("empty",     shell_is_destructive(""),              false);

    printf("\nprefix matches must not trigger\n");
    /* The check is whole-word: these merely start with a destructive name. */
    expect("rmdir-ish name 'rmx'",  shell_is_destructive("rmx foo"),         false);
    expect("'move' is not 'mv'",    shell_is_destructive("move a b"),        false);
    expect("'ddate'",               shell_is_destructive("ddate"),           false);
    expect("'format' as argument",  shell_is_destructive("help format"),     false);

    printf("\npath confinement\n");
    expect("/sd allowed",           path_allowed("/sd/claw/x"),      true);
    expect("/root allowed",         path_allowed("/root/init.sh"),   true);
    expect("relative rejected",     path_allowed("notes.txt"),       false);
    expect("traversal rejected",    path_allowed("/sd/../etc/x"),    false);
    expect("sneaky traversal",      path_allowed("/root/a/../../x"), false);
    expect("other root rejected",   path_allowed("/dev/null"),       false);
    expect("empty rejected",        path_allowed(""),                false);
    expect("NULL rejected",         path_allowed(NULL),              false);

    printf("\n");
    if (failures == 0) {
        printf("all guard tests passed\n");
        return 0;
    }
    printf("%d test(s) FAILED\n", failures);
    return 1;
}
