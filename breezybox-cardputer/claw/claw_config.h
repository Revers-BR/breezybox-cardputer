/*
 * claw_config.h - settings for the claw agent.
 *
 * Backed by a single JSON file. The SD card is preferred when present, because
 * `make flash` rewrites the LittleFS partition and would otherwise wipe the API
 * keys; the card survives reflashing. The trade-off is that the card is
 * removable -- `store=flash` forces internal storage for anyone who cares.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define CLAW_CFG_MAX_VALUE 512

/* Path of the config file actually in use. */
const char *claw_config_path(void);

/* Copy the value for `key` into out, or `fallback` (which may be NULL).
 * Returns true when a stored value was found. */
bool claw_config_get(const char *key, char *out, size_t out_len, const char *fallback);

int  claw_config_get_int(const char *key, int fallback);
bool claw_config_set(const char *key, const char *value);

/* True for keys that must never be printed in full. */
bool claw_config_is_secret(const char *key);
/* Render `value` for display, masking secrets. */
void claw_config_mask(const char *key, const char *value, char *out, size_t out_len);

/* Resolved API key for the active backend. Returns false with a hint in `err`
 * when unset. */
bool claw_config_api_key(char *out, size_t out_len, const char **err);

/* Forget the cached path; used after changing `store`. */
void claw_config_reset(void);
