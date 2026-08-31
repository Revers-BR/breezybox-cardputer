/*
 * Host tests for the format-specifier validator.
 *
 * Overriding a format string from the SD card means the compiler can no longer
 * check it against its call site, so this check is the only thing standing
 * between a typo and reading an integer as a pointer. It is the part of this
 * feature where a bug is expensive rather than merely annoying, so it is tested
 * off-device.
 *
 * Run: sh tests/c/run.sh
 */
#include "claw_text.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>

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

static void spec(const char *name, const char *fmt, const char *want)
{
    char got[64];
    bool ok = claw_text_specifiers(fmt, got, sizeof(got));
    if (ok && strcmp(got, want) == 0) {
        printf("  ok   %s\n", name);
        return;
    }
    failures++;
    printf("  FAIL %s\n", name);
    printf("       fmt  %s\n", fmt);
    printf("       got  '%s'  want '%s'\n", ok ? got : "(overflow)", want);
}

int main(void)
{
    printf("specifier extraction\n");
    spec("none",                "plain prose",                "");
    spec("one string",          "no such tool '%s'",          "s");
    spec("two mixed",           "wrote %u bytes to %s",       "us");
    spec("three",              "free %u, min %u, largest %u", "uuu");
    spec("long modifier",       "%s  %ld bytes",              "sld");
    spec("escaped percent",     "100%% done",                 "");
    spec("escape then real",    "100%% of %d",                "d");
    spec("width and flags",     "%-24s %8d",                  "sd");
    spec("precision",           "%.120s",                     "s");
    /* '*' takes its value from an argument, so "%.*s" consumes two. */
    spec("star precision",      "%.*s",                       "*s");
    spec("star width",          "%*d",                        "*d");
    spec("trailing percent",    "ends with %",                "");
    spec("adjacent",            "%d%s",                       "ds");

    printf("\ncompatible overrides are accepted\n");
    check("identical",
          claw_text_compatible("wrote %u bytes to %s", "wrote %u bytes to %s"), NULL);
    check("reworded, same specifiers",
          claw_text_compatible("wrote %u bytes to %s", "saved %u bytes into %s"), NULL);
    check("prose for prose",
          claw_text_compatible("No devices found.", "Nothing responded on the bus."), NULL);
    check("width changed, type kept",
          claw_text_compatible("%s %d", "%-20s %04d"), NULL);
    check("escaped percent added",
          claw_text_compatible("%d done", "%d%% done"), NULL);

    printf("\nunsafe overrides are refused\n");
    check("reordered types",
          !claw_text_compatible("%s %d", "%d %s"), "must not accept a swap");
    check("type changed",
          !claw_text_compatible("wrote %u bytes", "wrote %s bytes"), "u -> s");
    check("specifier dropped",
          !claw_text_compatible("%s on %s", "%s only"), NULL);
    check("specifier added",
          !claw_text_compatible("no devices", "no devices on %s"), NULL);
    check("length modifier dropped",
          !claw_text_compatible("%ld bytes", "%d bytes"), "ld -> d");
    check("prose replaced by a specifier",
          !claw_text_compatible("all good", "%s"), NULL);
    check("star precision type swap",
          !claw_text_compatible("%.*s", "%.*d"), "both consume int+arg");
    check("star precision dropped",
          !claw_text_compatible("%.*s", "%s"), "loses the int argument");

    printf("\nlookup falls back safely\n");
    {
        /* No SD card on the host, so every lookup must return its fallback. */
        const char *fb = "the compiled default";
        check("unknown id returns fallback",
              claw_text("no.such.id", fb) == fb, NULL);
        check("NULL id returns fallback",
              claw_text(NULL, fb) == fb, NULL);
        check("NULL fallback tolerated",
              claw_text("x", NULL) == NULL, NULL);
    }

    printf("\nend-to-end against a real override file\n");
    {
        /* Compiled with -DTEXT_DIR_SD pointing here, so this exercises the
         * loader, the lookup and the validator together. */
        mkdir(TEXT_DIR_SD, 0777);
        char path[256];
        snprintf(path, sizeof(path), "%s/messages.json", TEXT_DIR_SD);

        FILE *f = fopen(path, "wb");
        if (!f) {
            failures++;
            printf("  FAIL cannot write %s\n", path);
        } else {
            fputs("{\n"
                  "  \"t.safe\": \"reworded, same shape: %s\",\n"
                  "  \"t.unsafe\": \"swapped %d and %s\"\n"
                  "}\n", f);
            fclose(f);
            claw_text_reload();

            const char *safe = claw_text("t.safe", "original: %s");
            check("valid override is applied",
                  strcmp(safe, "reworded, same shape: %s") == 0, safe);

            const char *fb = "original %s then %d";
            check("unsafe override is refused", claw_text("t.unsafe", fb) == fb, NULL);

            const char *absent = "still compiled";
            check("absent key uses fallback",
                  claw_text("t.absent", absent) == absent, NULL);

            /* A malformed file must fall back entirely, not half-apply. */
            f = fopen(path, "wb");
            fputs("{ this is not json", f);
            fclose(f);
            claw_text_reload();
            const char *d = "compiled default";
            check("malformed file falls back entirely",
                  claw_text("t.safe", d) == d, NULL);

            remove(path);
            claw_text_reload();
            check("removing the file restores the default",
                  claw_text("t.safe", d) == d, NULL);
        }
    }

    printf("\n");
    if (failures == 0) {
        printf("all text tests passed\n");
        return 0;
    }
    printf("%d test(s) FAILED\n", failures);
    return 1;
}
