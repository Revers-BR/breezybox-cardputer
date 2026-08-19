/*
 * claw_session.h - conversation transcripts on disk.
 *
 * One session is one JSONL file: a line per turn, {"role":..,"content":..}.
 * Appending a turn is an append, never a rewrite, so cost does not grow with
 * conversation length.
 *
 * Replay is bounded by `context_budget` bytes rather than turn count: the most
 * recent turns that fit are sent, oldest dropped first. That keeps both the
 * request body and the peak heap flat no matter how long the conversation gets,
 * which is the property the whole design exists to preserve.
 */
#pragma once

#include "cJSON.h"

#include <stdbool.h>
#include <stddef.h>

#define CLAW_SESSION_ID_MAX   32
#define CLAW_SESSION_PATH_MAX 96

/* Longest single turn kept in a transcript. Longer content is stored truncated
 * with a marker; the alternative is an unbounded allocation on replay. */
#define CLAW_TURN_MAX 4096

/* Active session id, creating one if none is set. */
bool claw_session_current(char *out, size_t out_len);

/* Start a new session and make it current. */
bool claw_session_new(char *out, size_t out_len);

/* Append one turn to the active session. */
bool claw_session_append(const char *role, const char *content);

/* Build a messages array from the tail of the active session, newest-first
 * truncated to `budget` bytes of content. Caller owns the result. */
cJSON *claw_session_replay(size_t budget);

/* Number of turns in the active session. */
int claw_session_count(void);

bool claw_session_path(const char *id, char *out, size_t out_len);
bool claw_session_set_current(const char *id);
void claw_session_list(void);
bool claw_session_show(const char *id);
bool claw_session_delete(const char *id);
