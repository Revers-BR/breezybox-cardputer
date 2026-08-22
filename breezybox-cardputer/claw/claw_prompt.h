/*
 * claw_prompt.h - what the model is told about this device, every turn.
 *
 * Without this the model assumes a Linux box: it reaches for bash, curl, a
 * package manager and a `fs` module, fails, and reasons from the failure --
 * usually concluding a capability is missing when it is merely spelled
 * differently here. Every one of those was a wasted round trip.
 *
 * Kept deliberately short. It ships on every request, so it earns its tokens by
 * covering only what the model cannot discover cheaply and would otherwise get
 * wrong.
 *
 * A user can append their own instructions in /sd/claw/system.md.
 */
#pragma once

#include <stddef.h>

/* Device prompt + the runtime command list + whatever the user appended in
 * /sd/claw/system.md. */
#define CLAW_PROMPT_MAX 3072

/* Write the system prompt. Returns the length written. */
size_t claw_prompt_build(char *out, size_t out_len);
