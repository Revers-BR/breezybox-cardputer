/*
 * The streaming JSON writer must match cJSON_PrintUnformatted byte for byte.
 *
 * It produces the request bodies that three different APIs parse, so a
 * difference in escaping or number formatting is a 400 from the provider with
 * nothing local to show for it. Escaping, integers versus fractions, UTF-8
 * passthrough, empty containers and a realistic multi-KB request are all
 * compared against cJSON's own output.
 *
 * Run: sh tests/c/run.sh
 */
#include "claw_json_write.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void same(const char *name, cJSON *o)
{
    char *want = cJSON_PrintUnformatted(o);
    const char *path = "/tmp/claw_jw_test.json";
    long n = claw_json_write_file(o, path);

    FILE *f = fopen(path, "rb");
    char got[65536] = {0};
    size_t r = f ? fread(got, 1, sizeof(got) - 1, f) : 0;
    if (f) fclose(f);
    got[r] = '\0';

    if (!want) { printf("  SKIP %s (cJSON could not print)\n", name); return; }
    if (n < 0 || strcmp(want, got) != 0) {
        fails++;
        printf("  FAIL %s\n    want: %s\n    got : %s\n", name, want, got);
    } else {
        printf("  ok   %s (%ld bytes)\n", name, n);
    }
    free(want);
    remove(path);
}

int main(void)
{
    cJSON *o;

    o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "model", "gemini-2.5-flash");
    cJSON_AddNumberToObject(o, "max_tokens", 2048);
    cJSON_AddBoolToObject(o, "stream", 1);
    same("flat object", o); cJSON_Delete(o);

    o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "quote", "he said \"hi\"");
    cJSON_AddStringToObject(o, "back", "a\\b");
    cJSON_AddStringToObject(o, "lines", "one\ntwo\ttabbed\r\n");
    cJSON_AddStringToObject(o, "ctrl", "bell\x07 and \x1b escape");
    cJSON_AddStringToObject(o, "utf8", "temp 21°C ☁");
    same("string escaping", o); cJSON_Delete(o);

    o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "int", 2048);
    cJSON_AddNumberToObject(o, "zero", 0);
    cJSON_AddNumberToObject(o, "neg", -17);
    cJSON_AddNumberToObject(o, "frac", 3.25);
    cJSON_AddNumberToObject(o, "big", 1e12);
    same("numbers", o); cJSON_Delete(o);

    o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "empty_obj", cJSON_CreateObject());
    cJSON_AddItemToObject(o, "empty_arr", cJSON_CreateArray());
    cJSON_AddNullToObject(o, "nothing");
    same("empty and null", o); cJSON_Delete(o);

    /* A realistic request: nested contents, tools, and a long script. */
    o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "model", "gemini-2.5-flash");
    cJSON *contents = cJSON_CreateArray();
    for (int i = 0; i < 40; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "role", i % 2 ? "model" : "user");
        cJSON *parts = cJSON_CreateArray();
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "text",
            "local breezy = require(\"breezy\")\nprint(\"line\")\n-- \"quoted\"\n");
        cJSON_AddItemToArray(parts, p);
        cJSON_AddItemToObject(t, "parts", parts);
        cJSON_AddItemToArray(contents, t);
    }
    cJSON_AddItemToObject(o, "contents", contents);
    same("realistic request", o); cJSON_Delete(o);

    printf("\n");
    if (fails == 0) { printf("streaming writer matches cJSON\n"); return 0; }
    printf("%d mismatch(es)\n", fails);
    return 1;
}
