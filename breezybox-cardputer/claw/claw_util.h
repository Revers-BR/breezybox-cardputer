/*
 * claw_util.h - small shared helpers.
 *
 * Reading a whole small file was open-coded in five places with slightly
 * different size limits and error handling. One version, one limit.
 */
#pragma once

#include <stddef.h>

/* Read a file into a NUL-terminated buffer the caller frees.
 * Returns NULL when the file is missing, empty or larger than max_bytes.
 * `len_out` may be NULL. */
char *claw_read_file(const char *path, size_t max_bytes, size_t *len_out);
