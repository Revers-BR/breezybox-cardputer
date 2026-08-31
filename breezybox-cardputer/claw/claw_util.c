#include "claw_util.h"

#include <stdio.h>
#include <stdlib.h>

char *claw_read_file(const char *path, size_t max_bytes, size_t *len_out)
{
    if (len_out) {
        *len_out = 0;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || (size_t)len > max_bytes) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';
    if (len_out) {
        *len_out = got;
    }
    return buf;
}
