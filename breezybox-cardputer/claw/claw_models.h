/*
 * claw_models.h - the model catalogue, as data rather than code.
 *
 * Providers add and retire models far faster than this firmware gets reflashed,
 * so the list lives in JSON on the SD card:
 *
 *     /sd/claw/models.json          user-editable, survives `make flash`
 *     /root/apps/espclaw/models.json  shipped default, used to seed the above
 *
 * Shape:
 *   { "gemini": ["gemini-2.5-flash", ...], "openai": [...], ... }
 *
 * The first entry for a backend is its default model. Editing the file is
 * enough to add a model the firmware has never heard of.
 */
#pragma once

#include "cJSON.h"

#include <stdbool.h>
#include <stddef.h>

/* Path of the catalogue in use. Copies the shipped default onto the card the
 * first time, so the editable copy exists without the user creating it. */
const char *claw_models_path(void);

/* Models for one backend. Caller must cJSON_Delete the parsed root, which is
 * returned via `root_out`. NULL when the catalogue has no entry. */
cJSON *claw_models_for(const char *backend, cJSON **root_out);

/* First listed model for a backend, i.e. its default.
 * Returns false when the catalogue has nothing for it. */
bool claw_models_default(const char *backend, char *out, size_t out_len);
