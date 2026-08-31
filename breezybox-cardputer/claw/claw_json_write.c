/*
 * claw_json_write.c - serialise cJSON straight to a file.
 *
 * cJSON's printers build the whole document in one allocation and grow it by
 * doubling, so a 12 KB request can ask for 32 KB contiguous. On a heap whose
 * largest free block is often around 31 KB that fails, and it fails for the
 * requests that matter most -- the ones with tool results accumulated in them.
 *
 * The request is written to a file anyway, so there is no reason to build it in
 * memory first. This walks the tree and writes as it goes: peak memory is one
 * stack frame per nesting level, whatever the document's size.
 */

#include "claw_json_write.h"

#include <stdio.h>
#include <string.h>

static void write_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\b': fputs("\\b", f);  break;
        case '\f': fputs("\\f", f);  break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        default:
            if (*p < 0x20) {
                fprintf(f, "\\u%04x", *p);
            } else {
                fputc(*p, f);        /* UTF-8 passes through byte by byte */
            }
        }
    }
    fputc('"', f);
}

static void write_number(FILE *f, double d)
{
    /* Match cJSON: integral values print without a decimal point, which
     * matters because APIs reject 2048.0 where they expect 2048. */
    if (d == (double)(long long)d && d >= -9007199254740992.0 &&
        d <= 9007199254740992.0) {
        fprintf(f, "%lld", (long long)d);
    } else {
        fprintf(f, "%.17g", d);
    }
}

static void write_item(FILE *f, const cJSON *item)
{
    if (!item) {
        fputs("null", f);
        return;
    }
    switch (item->type & 0xFF) {
    case cJSON_NULL:   fputs("null", f);  break;
    case cJSON_False:  fputs("false", f); break;
    case cJSON_True:   fputs("true", f);  break;
    case cJSON_Number: write_number(f, item->valuedouble); break;
    case cJSON_String: write_string(f, item->valuestring ? item->valuestring : ""); break;
    case cJSON_Raw:
        if (item->valuestring) {
            fputs(item->valuestring, f);
        } else {
            fputs("null", f);
        }
        break;
    case cJSON_Array: {
        fputc('[', f);
        for (const cJSON *c = item->child; c; c = c->next) {
            write_item(f, c);
            if (c->next) {
                fputc(',', f);
            }
        }
        fputc(']', f);
        break;
    }
    case cJSON_Object: {
        fputc('{', f);
        for (const cJSON *c = item->child; c; c = c->next) {
            write_string(f, c->string ? c->string : "");
            fputc(':', f);
            write_item(f, c);
            if (c->next) {
                fputc(',', f);
            }
        }
        fputc('}', f);
        break;
    }
    default:
        fputs("null", f);
        break;
    }
}

long claw_json_write_file(const cJSON *item, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    write_item(f, item);
    long n = ftell(f);
    if (fclose(f) != 0) {
        return -1;
    }
    return n;
}
