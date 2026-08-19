#include "claw_memory.h"

#include "esp_log.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "claw_mem";

#define MEM_DIR_SD    "/sd/claw/memory"
#define MEM_DIR_FLASH "/root/.claw_memory"
#define INDEX_FILE    "MEMORY.md"

static char s_dir[64];

static bool is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool ensure_dir(const char *p)
{
    if (is_dir(p)) {
        return true;
    }
    mkdir(p, 0777);
    return is_dir(p);
}

/* Memory follows the config and transcripts: the card when it is there. */
static const char *mem_dir(void)
{
    if (s_dir[0]) {
        return s_dir;
    }
    if (ensure_dir("/sd/claw") && ensure_dir(MEM_DIR_SD)) {
        snprintf(s_dir, sizeof(s_dir), "%s", MEM_DIR_SD);
    } else {
        ensure_dir(MEM_DIR_FLASH);
        snprintf(s_dir, sizeof(s_dir), "%s", MEM_DIR_FLASH);
    }
    return s_dir;
}

/* Names become filenames, so they must not escape the directory. */
static bool name_ok(const char *name)
{
    if (!name || !name[0] || strlen(name) >= CLAW_MEMORY_NAME_MAX) {
        return false;
    }
    if (strchr(name, '/') || strstr(name, "..")) {
        return false;
    }
    for (const char *p = name; *p; p++) {
        bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '-' || *p == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static void mem_path(const char *name, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%s.md", mem_dir(), name);
}

static void index_path(char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%s", mem_dir(), INDEX_FILE);
}

/* Rewrite the index without `drop_name`, appending `add_line` if given.
 * The index is small, so a rewrite is cheaper than editing in place. */
static bool index_rewrite(const char *drop_name, const char *add_line)
{
    char ip[96];
    index_path(ip, sizeof(ip));

    char *kept = malloc(4096);
    if (!kept) {
        return false;
    }
    size_t used = 0;
    kept[0] = '\0';

    FILE *f = fopen(ip, "rb");
    if (f) {
        char line[256];
        char prefix[CLAW_MEMORY_NAME_MAX + 8];
        snprintf(prefix, sizeof(prefix), "- [%s]", drop_name ? drop_name : "\x01");
        while (fgets(line, sizeof(line), f)) {
            if (drop_name && strncmp(line, prefix, strlen(prefix)) == 0) {
                continue;                 /* replaced or deleted */
            }
            size_t n = strlen(line);
            if (used + n < 4095) {
                memcpy(kept + used, line, n);
                used += n;
                kept[used] = '\0';
            }
        }
        fclose(f);
    }

    f = fopen(ip, "wb");
    if (!f) {
        free(kept);
        return false;
    }
    if (used) {
        fwrite(kept, 1, used, f);
    }
    if (add_line) {
        fputs(add_line, f);
    }
    fclose(f);
    free(kept);
    return true;
}

bool claw_memory_save(const char *name, const char *description, const char *content)
{
    if (!name_ok(name) || !content) {
        return false;
    }
    char p[96];
    mem_path(name, p, sizeof(p));

    FILE *f = fopen(p, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot write %s", p);
        return false;
    }
    size_t len = strlen(content);
    if (len > CLAW_MEMORY_BODY_MAX) {
        len = CLAW_MEMORY_BODY_MAX;
    }
    fwrite(content, 1, len, f);
    fputc('\n', f);
    fclose(f);

    char line[256];
    snprintf(line, sizeof(line), "- [%s] %.180s\n", name,
             (description && description[0]) ? description : "(no description)");
    return index_rewrite(name, line);
}

bool claw_memory_read(const char *name, char *out, size_t out_len)
{
    if (!name_ok(name)) {
        return false;
    }
    char p[96];
    mem_path(name, p, sizeof(p));

    FILE *f = fopen(p, "rb");
    if (!f) {
        return false;
    }
    size_t got = fread(out, 1, out_len - 1, f);
    fclose(f);
    out[got] = '\0';
    return true;
}

bool claw_memory_delete(const char *name)
{
    if (!name_ok(name)) {
        return false;
    }
    char p[96];
    mem_path(name, p, sizeof(p));
    if (remove(p) != 0) {
        return false;
    }
    index_rewrite(name, NULL);
    return true;
}

size_t claw_memory_context(char *out, size_t out_len)
{
    char ip[96];
    index_path(ip, sizeof(ip));

    FILE *f = fopen(ip, "rb");
    if (!f) {
        return 0;
    }

    const char *header =
        "You have persistent memory on this device. What you already know:\n\n";
    size_t used = snprintf(out, out_len, "%s", header);

    size_t body_start = used;
    size_t got = fread(out + used, 1, (out_len > used) ? (out_len - used - 160) : 0, f);
    fclose(f);
    used += got;
    out[used] = '\0';

    if (used == body_start) {
        return 0;                        /* index exists but is empty */
    }

    used += snprintf(out + used, out_len - used,
                     "\nUse memory_read to see the detail of any of these. "
                     "Use memory_save when you learn something worth keeping "
                     "for future conversations.\n");
    return used;
}

int claw_memory_count(void)
{
    DIR *d = opendir(mem_dir());
    if (!d) {
        return 0;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *dot = strstr(e->d_name, ".md");
        if (dot && dot[3] == '\0' && strcmp(e->d_name, INDEX_FILE) != 0) {
            n++;
        }
    }
    closedir(d);
    return n;
}

void claw_memory_list(void)
{
    char ip[96];
    index_path(ip, sizeof(ip));

    FILE *f = fopen(ip, "rb");
    if (!f) {
        printf("nothing remembered yet.\n");
        printf("claw saves things it decides are worth keeping; you can also "
               "say \"remember that ...\"\n");
        return;
    }
    printf("memory (%s):\n", mem_dir());
    char line[256];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '-') {
            printf("  %s", line);
            n++;
        }
    }
    fclose(f);
    if (n == 0) {
        printf("  (empty)\n");
    }
}
