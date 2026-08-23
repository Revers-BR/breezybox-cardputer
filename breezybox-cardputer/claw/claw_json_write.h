/*
 * Serialise a cJSON tree directly to a file, without building it in memory.
 *
 * cJSON's own printers need one allocation the size of the whole document, and
 * grow it by doubling; on a fragmented heap that fails for exactly the large
 * requests that matter. This uses a stack frame per nesting level instead.
 */
#pragma once

#include "cJSON.h"

/* Returns the bytes written, or -1. */
long claw_json_write_file(const cJSON *item, const char *path);
