#include "claw_config.h"

#include "cJSON.h"
#include "claw_util.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "claw_cfg";

#define SD_DIR      "/sd/claw"
#define SD_PATH     SD_DIR "/config.json"
#define FLASH_PATH  "/root/.claw.json"

static char s_path[64];

static bool file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static bool dir_usable(const char *p)
{
    struct stat st;
    if (stat(p, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    if (mkdir(p, 0777) == 0) {
        return true;
    }
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Copy an existing flash config onto the card the first time the card is used,
 * so switching storage does not silently lose settings. */
static void migrate_to_sd(void)
{
    if (file_exists(SD_PATH) || !file_exists(FLASH_PATH)) {
        return;
    }
    char *data = claw_read_file(FLASH_PATH, 8192, NULL);
    if (!data) {
        return;
    }
    FILE *f = fopen(SD_PATH, "wb");
    if (f) {
        fwrite(data, 1, strlen(data), f);
        fclose(f);
        ESP_LOGI(TAG, "migrated config to %s", SD_PATH);
    }
    free(data);
}

void claw_config_reset(void)
{
    s_path[0] = '\0';
}

const char *claw_config_path(void)
{
    if (s_path[0]) {
        return s_path;
    }

    /* `store` lives in flash: it selects which file everything else uses, so it
     * cannot live in the file it selects. */
    char store[16] = {0};
    char *raw = claw_read_file(FLASH_PATH, 8192, NULL);
    if (raw) {
        cJSON *root = cJSON_Parse(raw);
        if (root) {
            cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "store");
            if (cJSON_IsString(v) && v->valuestring) {
                snprintf(store, sizeof(store), "%s", v->valuestring);
            }
            cJSON_Delete(root);
        }
        free(raw);
    }

    if (strcmp(store, "flash") == 0) {
        snprintf(s_path, sizeof(s_path), "%s", FLASH_PATH);
    } else if (dir_usable(SD_DIR)) {
        migrate_to_sd();
        snprintf(s_path, sizeof(s_path), "%s", SD_PATH);
    } else {
        snprintf(s_path, sizeof(s_path), "%s", FLASH_PATH);
    }
    return s_path;
}

static const char *path_for(const char *key)
{
    return (strcmp(key, "store") == 0) ? FLASH_PATH : claw_config_path();
}

bool claw_config_get(const char *key, char *out, size_t out_len, const char *fallback)
{
    bool found = false;
    char *raw = claw_read_file(path_for(key), 8192, NULL);
    if (raw) {
        cJSON *root = cJSON_Parse(raw);
        if (root) {
            cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
            if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
                snprintf(out, out_len, "%s", v->valuestring);
                found = true;
            } else if (cJSON_IsNumber(v)) {
                snprintf(out, out_len, "%d", (int)v->valuedouble);
                found = true;
            }
            cJSON_Delete(root);
        }
        free(raw);
    }
    if (!found) {
        if (fallback) {
            snprintf(out, out_len, "%s", fallback);
        } else if (out_len) {
            out[0] = '\0';
        }
    }
    return found;
}

int claw_config_get_int(const char *key, int fallback)
{
    char buf[32];
    if (!claw_config_get(key, buf, sizeof(buf), NULL) || !buf[0]) {
        return fallback;
    }
    return atoi(buf);
}

bool claw_config_set(const char *key, const char *value)
{
    const char *path = path_for(key);

    cJSON *root = NULL;
    char *raw = claw_read_file(path, 8192, NULL);
    if (raw) {
        root = cJSON_Parse(raw);
        free(raw);
    }
    if (!root) {
        root = cJSON_CreateObject();
    }
    if (!root) {
        return false;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(root, key);
    if (!cJSON_AddStringToObject(root, key, value)) {
        cJSON_Delete(root);
        return false;
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot write %s", path);
        cJSON_free(text);
        return false;
    }
    size_t len = strlen(text);
    size_t wrote = fwrite(text, 1, len, f);
    fclose(f);
    cJSON_free(text);

    if (strcmp(key, "store") == 0) {
        claw_config_reset();
    }
    return wrote == len;
}

bool claw_config_is_secret(const char *key)
{
    const char *suffix = strstr(key, ".key");
    return suffix != NULL && suffix[4] == '\0';
}

void claw_config_mask(const char *key, const char *value, char *out, size_t out_len)
{
    if (!claw_config_is_secret(key) || !value || !value[0]) {
        snprintf(out, out_len, "%s", value ? value : "");
        return;
    }
    size_t n = strlen(value);
    if (n <= 8) {
        snprintf(out, out_len, "****");
        return;
    }
    snprintf(out, out_len, "%.4s...%s", value, value + n - 4);
}

bool claw_config_api_key(char *out, size_t out_len, const char **err)
{
    char backend[32];
    claw_config_get("backend", backend, sizeof(backend), "anthropic");

    char key_name[48];
    snprintf(key_name, sizeof(key_name), "%s.key", backend);

    if (claw_config_get(key_name, out, out_len, NULL) && out[0]) {
        return true;
    }
    static char msg[96];
    snprintf(msg, sizeof(msg), "no API key set. Run: claw config set %s <key>", key_name);
    if (err) {
        *err = msg;
    }
    return false;
}
