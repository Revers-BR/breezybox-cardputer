#include "claw_models.h"
#include "claw_util.h"

#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "claw_models";

#define MODELS_SD      "/sd/claw/models.json"
#define MODELS_SHIPPED "/root/apps/espclaw/models.json"
/* Development copies, matching the CA search order in claw_agent.c. */
#define MODELS_DEV1    "/sd/espclaw/models.json"
#define MODELS_DEV2    "/sd/apps/espclaw/models.json"

static char s_path[64];

static bool exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static bool copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb");
    if (!in) {
        return false;
    }
    FILE *out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    char buf[256];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    fclose(in);
    fclose(out);
    return ok;
}

const char *claw_models_path(void)
{
    if (s_path[0]) {
        return s_path;
    }

    /* A working copy next to the sources wins, as it does for the CA. */
    const char *dev[] = { MODELS_DEV1, MODELS_DEV2 };
    for (size_t i = 0; i < 2; i++) {
        if (exists(dev[i])) {
            snprintf(s_path, sizeof(s_path), "%s", dev[i]);
            return s_path;
        }
    }

    if (exists(MODELS_SD)) {
        snprintf(s_path, sizeof(s_path), "%s", MODELS_SD);
        return s_path;
    }

    /* Seed the card from the shipped copy so the editable file exists without
     * the user having to create it. */
    struct stat st;
    if (stat("/sd/claw", &st) == 0 && S_ISDIR(st.st_mode) && exists(MODELS_SHIPPED)) {
        if (copy_file(MODELS_SHIPPED, MODELS_SD)) {
            ESP_LOGI(TAG, "seeded %s", MODELS_SD);
            snprintf(s_path, sizeof(s_path), "%s", MODELS_SD);
            return s_path;
        }
    }

    snprintf(s_path, sizeof(s_path), "%s", MODELS_SHIPPED);
    return s_path;
}

static cJSON *load_root(void)
{
    const char *path = claw_models_path();
    char *buf = claw_read_file(path, 8192, NULL);
    if (!buf) {
        return NULL;
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "%s is not valid JSON", path);
    }
    return root;
}

cJSON *claw_models_for(const char *backend, cJSON **root_out)
{
    if (root_out) {
        *root_out = NULL;
    }
    cJSON *root = load_root();
    if (!root) {
        return NULL;
    }
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, backend);
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return NULL;
    }
    if (root_out) {
        *root_out = root;    /* caller frees; arr points into it */
    } else {
        cJSON_Delete(root);
        return NULL;
    }
    return arr;
}

bool claw_models_default(const char *backend, char *out, size_t out_len)
{
    cJSON *root = NULL;
    cJSON *arr = claw_models_for(backend, &root);
    bool ok = false;
    if (arr) {
        cJSON *first = cJSON_GetArrayItem(arr, 0);
        if (cJSON_IsString(first) && first->valuestring[0]) {
            snprintf(out, out_len, "%s", first->valuestring);
            ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}
