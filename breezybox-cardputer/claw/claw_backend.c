#include "claw_backend.h"
#include "claw_config.h"
#include "claw_models.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const claw_backend_t *const k_backends[] = {
    &claw_backend_anthropic,
    &claw_backend_openai,
    &claw_backend_gemini,
};

const claw_backend_t *claw_backend_find(const char *name)
{
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(k_backends) / sizeof(k_backends[0]); i++) {
        if (strcmp(k_backends[i]->name, name) == 0) {
            return k_backends[i];
        }
    }
    return NULL;
}

const claw_backend_t *claw_backend_active(void)
{
    char name[32];
    claw_config_get("backend", name, sizeof(name), "anthropic");
    const claw_backend_t *b = claw_backend_find(name);
    return b ? b : &claw_backend_anthropic;
}

size_t claw_backend_count(void)
{
    return sizeof(k_backends) / sizeof(k_backends[0]);
}

const claw_backend_t *claw_backend_at(size_t i)
{
    return (i < claw_backend_count()) ? k_backends[i] : NULL;
}

void claw_backend_model(const claw_backend_t *b, char *out, size_t out_len)
{
    char key[48];
    snprintf(key, sizeof(key), "model.%s", b->name);
    if (claw_config_get(key, out, out_len, NULL) && out[0]) {
        return;
    }
    /* Older configs stored a single `model` for whichever backend was active. */
    if (claw_config_get("model", out, out_len, NULL) && out[0]) {
        return;
    }
    /* Otherwise the catalogue decides, so a new model needs only a file edit. */
    if (claw_models_default(b->name, out, out_len)) {
        return;
    }
    snprintf(out, out_len, "%s", b->default_model);
}

bool claw_backend_set_model(const claw_backend_t *b, const char *model)
{
    char key[48];
    snprintf(key, sizeof(key), "model.%s", b->name);
    return claw_config_set(key, model);
}
