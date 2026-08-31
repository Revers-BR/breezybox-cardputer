#include "claw_session.h"
#include "claw_config.h"

#include "esp_log.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *TAG = "claw_sess";

#define SESS_DIR_SD    "/sd/claw/sessions"
#define SESS_DIR_FLASH "/root/.claw_sessions"
#define CURRENT_FILE   ".current"

/* Cap on turns we will index when replaying. Older turns beyond this are not
 * considered; the byte budget usually bites long before this does. */
#define MAX_INDEXED_TURNS 256

static char s_dir[CLAW_SESSION_PATH_MAX];

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

/* Transcripts follow the config: SD when available, flash otherwise. */
static const char *session_dir(void)
{
    if (s_dir[0]) {
        return s_dir;
    }
    if (ensure_dir("/sd/claw") && ensure_dir(SESS_DIR_SD)) {
        snprintf(s_dir, sizeof(s_dir), "%s", SESS_DIR_SD);
    } else {
        ensure_dir(SESS_DIR_FLASH);
        snprintf(s_dir, sizeof(s_dir), "%s", SESS_DIR_FLASH);
    }
    return s_dir;
}

bool claw_session_path(const char *id, char *out, size_t out_len)
{
    if (!id || !id[0] || strchr(id, '/')) {
        return false;                 /* never let an id escape the directory */
    }
    snprintf(out, out_len, "%s/%s.jsonl", session_dir(), id);
    return true;
}

static void current_path(char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%s", session_dir(), CURRENT_FILE);
}

bool claw_session_set_current(const char *id)
{
    char p[CLAW_SESSION_PATH_MAX];
    current_path(p, sizeof(p));
    FILE *f = fopen(p, "wb");
    if (!f) {
        return false;
    }
    fputs(id, f);
    fclose(f);
    return true;
}

bool claw_session_new(char *out, size_t out_len)
{
    /* Seconds since boot is enough to order sessions and keeps ids short; the
     * device has no reliable wall clock before NTP. */
    static unsigned counter;
    time_t now = time(NULL);
    char id[CLAW_SESSION_ID_MAX];
    if (now > 1600000000) {
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(id, sizeof(id), "%Y%m%d-%H%M%S", &tm);
    } else {
        snprintf(id, sizeof(id), "s%lu-%u", (unsigned long)(clock() / 1000), ++counter);
    }

    char p[CLAW_SESSION_PATH_MAX];
    if (!claw_session_path(id, p, sizeof(p))) {
        return false;
    }
    FILE *f = fopen(p, "ab");
    if (!f) {
        ESP_LOGE(TAG, "cannot create %s", p);
        return false;
    }
    fclose(f);

    claw_session_set_current(id);
    snprintf(out, out_len, "%s", id);
    return true;
}

bool claw_session_current(char *out, size_t out_len)
{
    char p[CLAW_SESSION_PATH_MAX];
    current_path(p, sizeof(p));

    FILE *f = fopen(p, "rb");
    if (f) {
        char id[CLAW_SESSION_ID_MAX] = {0};
        if (fgets(id, sizeof(id), f)) {
            id[strcspn(id, "\r\n")] = '\0';
        }
        fclose(f);
        if (id[0]) {
            char sp[CLAW_SESSION_PATH_MAX];
            if (claw_session_path(id, sp, sizeof(sp))) {
                struct stat st;
                if (stat(sp, &st) == 0) {
                    snprintf(out, out_len, "%s", id);
                    return true;
                }
            }
        }
    }
    return claw_session_new(out, out_len);
}

bool claw_session_append(const char *role, const char *content)
{
    char id[CLAW_SESSION_ID_MAX];
    if (!claw_session_current(id, sizeof(id))) {
        return false;
    }
    char p[CLAW_SESSION_PATH_MAX];
    if (!claw_session_path(id, p, sizeof(p))) {
        return false;
    }

    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return false;
    }
    cJSON_AddStringToObject(o, "role", role);

    if (strlen(content) > CLAW_TURN_MAX) {
        char *cut = malloc(CLAW_TURN_MAX + 32);
        if (!cut) {
            cJSON_Delete(o);
            return false;
        }
        memcpy(cut, content, CLAW_TURN_MAX);
        strcpy(cut + CLAW_TURN_MAX, "\n[truncated]");
        cJSON_AddStringToObject(o, "content", cut);
        free(cut);
    } else {
        cJSON_AddStringToObject(o, "content", content);
    }

    char *line = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!line) {
        return false;
    }

    FILE *f = fopen(p, "ab");
    if (!f) {
        cJSON_free(line);
        return false;
    }
    fputs(line, f);
    fputc('\n', f);
    fclose(f);
    cJSON_free(line);
    return true;
}

long claw_session_mark(void)
{
    char id[CLAW_SESSION_ID_MAX];
    if (!claw_session_current(id, sizeof(id))) {
        return -1;
    }
    char p[CLAW_SESSION_PATH_MAX];
    if (!claw_session_path(id, p, sizeof(p))) {
        return -1;
    }
    struct stat st;
    return (stat(p, &st) == 0) ? (long)st.st_size : -1;
}

bool claw_session_rollback(long mark)
{
    if (mark < 0) {
        return false;
    }
    char id[CLAW_SESSION_ID_MAX];
    if (!claw_session_current(id, sizeof(id))) {
        return false;
    }
    char p[CLAW_SESSION_PATH_MAX];
    if (!claw_session_path(id, p, sizeof(p))) {
        return false;
    }
    struct stat st;
    if (stat(p, &st) != 0 || (long)st.st_size <= mark) {
        return true;                  /* nothing was written; nothing to undo */
    }
    /* No ftruncate on this VFS, so rewrite the kept prefix. */
    char *buf = malloc((size_t)mark + 1);
    if (!buf) {
        return false;
    }
    FILE *f = fopen(p, "rb");
    if (!f) {
        free(buf);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)mark, f);
    fclose(f);

    f = fopen(p, "wb");
    if (!f) {
        free(buf);
        return false;
    }
    fwrite(buf, 1, got, f);
    fclose(f);
    free(buf);
    return true;
}

int claw_session_count(void)
{
    char id[CLAW_SESSION_ID_MAX];
    if (!claw_session_current(id, sizeof(id))) {
        return 0;
    }
    char p[CLAW_SESSION_PATH_MAX];
    claw_session_path(id, p, sizeof(p));

    FILE *f = fopen(p, "rb");
    if (!f) {
        return 0;
    }
    int n = 0, c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            n++;
        }
    }
    fclose(f);
    return n;
}

cJSON *claw_session_replay(size_t budget)
{
    char id[CLAW_SESSION_ID_MAX];
    if (!claw_session_current(id, sizeof(id))) {
        return cJSON_CreateArray();
    }
    char p[CLAW_SESSION_PATH_MAX];
    claw_session_path(id, p, sizeof(p));

    FILE *f = fopen(p, "rb");
    if (!f) {
        return cJSON_CreateArray();
    }

    /* Pass 1: index line offsets and sizes. Only offsets are held, so this
     * costs a few KB regardless of how long the transcript is. */
    static long offs[MAX_INDEXED_TURNS];
    static long lens[MAX_INDEXED_TURNS];
    int n = 0;
    long start = 0, pos = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        pos++;
        if (c == '\n') {
            if (n < MAX_INDEXED_TURNS) {
                offs[n] = start;
                lens[n] = pos - start - 1;
                n++;
            }
            start = pos;
        }
    }

    /* Pass 2: walk backwards until the byte budget is spent, then replay
     * forwards from there so turn order is preserved. */
    int first = n;
    size_t total = 0;
    while (first > 0) {
        size_t len = (size_t)lens[first - 1];
        if (total + len > budget && first < n) {
            break;
        }
        total += len;
        first--;
    }

    cJSON *messages = cJSON_CreateArray();
    if (!messages) {
        fclose(f);
        return NULL;
    }

    char *buf = malloc(CLAW_TURN_MAX + 256);
    if (!buf) {
        fclose(f);
        return messages;
    }

    for (int i = first; i < n; i++) {
        if (lens[i] <= 0 || lens[i] > CLAW_TURN_MAX + 200) {
            continue;
        }
        fseek(f, offs[i], SEEK_SET);
        size_t got = fread(buf, 1, (size_t)lens[i], f);
        buf[got] = '\0';

        cJSON *turn = cJSON_Parse(buf);
        if (!turn) {
            continue;                 /* skip a corrupt line rather than fail */
        }
        cJSON *role = cJSON_GetObjectItemCaseSensitive(turn, "role");
        cJSON *content = cJSON_GetObjectItemCaseSensitive(turn, "content");
        if (cJSON_IsString(role) && cJSON_IsString(content)) {
            cJSON_AddItemToArray(messages, turn);
        } else {
            cJSON_Delete(turn);
        }
    }

    free(buf);
    fclose(f);
    return messages;
}

void claw_session_list(void)
{
    const char *dir = session_dir();
    DIR *d = opendir(dir);
    if (!d) {
        printf("no sessions\n");
        return;
    }
    char cur[CLAW_SESSION_ID_MAX] = {0};
    claw_session_current(cur, sizeof(cur));

    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != NULL) {
        const char *dot = strstr(e->d_name, ".jsonl");
        if (!dot || dot[6] != '\0') {
            continue;
        }
        char id[CLAW_SESSION_ID_MAX];
        size_t idlen = (size_t)(dot - e->d_name);
        if (idlen >= sizeof(id)) {
            continue;
        }
        memcpy(id, e->d_name, idlen);
        id[idlen] = '\0';

        char p[CLAW_SESSION_PATH_MAX];
        claw_session_path(id, p, sizeof(p));
        struct stat st;
        long size = (stat(p, &st) == 0) ? (long)st.st_size : 0;

        printf("  %s%-24s %ld bytes\n", strcmp(id, cur) == 0 ? "* " : "  ", id, size);
        n++;
    }
    closedir(d);
    if (n == 0) {
        printf("no sessions\n");
    }
}

bool claw_session_show(const char *id)
{
    char use[CLAW_SESSION_ID_MAX];
    if (!id || !id[0]) {
        if (!claw_session_current(use, sizeof(use))) {
            return false;
        }
        id = use;
    }
    char p[CLAW_SESSION_PATH_MAX];
    if (!claw_session_path(id, p, sizeof(p))) {
        return false;
    }
    FILE *f = fopen(p, "rb");
    if (!f) {
        printf("no such session: %s\n", id);
        return false;
    }

    char *buf = malloc(CLAW_TURN_MAX + 256);
    if (!buf) {
        fclose(f);
        return false;
    }
    while (fgets(buf, CLAW_TURN_MAX + 256, f)) {
        cJSON *turn = cJSON_Parse(buf);
        if (!turn) {
            continue;
        }
        cJSON *role = cJSON_GetObjectItemCaseSensitive(turn, "role");
        cJSON *content = cJSON_GetObjectItemCaseSensitive(turn, "content");
        if (cJSON_IsString(role) && cJSON_IsString(content)) {
            printf("%s: %s\n", role->valuestring, content->valuestring);
        }
        cJSON_Delete(turn);
    }
    free(buf);
    fclose(f);
    return true;
}

bool claw_session_delete(const char *id)
{
    char p[CLAW_SESSION_PATH_MAX];
    if (!claw_session_path(id, p, sizeof(p))) {
        return false;
    }
    return remove(p) == 0;
}
