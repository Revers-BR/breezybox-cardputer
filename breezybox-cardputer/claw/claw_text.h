/*
 * claw_text.h - model-facing text, overridable from the SD card.
 *
 * Every string that steers the model -- the system prompt, tool and parameter
 * descriptions, and the messages tools return when something fails -- can be
 * replaced by a file on the card, so tuning the wording needs no rebuild.
 *
 * Most of this project's bugs were fixed by rewording these strings rather than
 * by changing logic, and each reword cost a flash.
 *
 * Files are optional. When one is absent the compiled default is used, which is
 * the point: a firmware update carrying better text must still take effect.
 * Nothing is auto-seeded onto the card for that reason -- `claw text dump`
 * copies the shipped defaults when you want a starting point.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Override for `id`, or `fallback` when there is none.
 *
 * `fallback` is also the safety contract: if the override's printf conversion
 * specifiers do not match it exactly, in type and order, the override is
 * refused and `fallback` is returned. A string like "wrote %u bytes to %s" is
 * an agreement with its call site, and an override reading "wrote %s bytes"
 * is not a wording change but undefined behaviour.
 *
 * The returned pointer is owned by this module and stays valid until
 * claw_text_reload().
 */
const char *claw_text(const char *id, const char *fallback);

/* Drop the cache so the next claw_text() re-reads the card. */
void claw_text_reload(void);

/* Print which overrides are active, and which were refused and why. */
void claw_text_status(void);

/* Copy the shipped default files onto the card as a starting point.
 * Returns the number written. */
int claw_text_dump(void);

/* --- exposed for the host tests ------------------------------------------ */

/*
 * Write the conversion specifiers of `fmt` into `out` as a compact string,
 * e.g. "wrote %u bytes to %-8s" -> "us". Returns false if `out` is too small.
 * "%%" is an escape, not a conversion.
 */
bool claw_text_specifiers(const char *fmt, char *out, size_t out_len);

/* True when `candidate` may safely replace `fallback` as a format string. */
bool claw_text_compatible(const char *fallback, const char *candidate);
