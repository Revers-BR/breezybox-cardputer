# ESP-Claw on Cardputer: implementation-language decision

**Status:** RESOLVED 2026-08-18 — option B implemented and working on hardware.
The agent core is C (`breezybox-cardputer/claw/`); Lua remains as the skill
runtime. The history below is kept because the measurements are the reason for
the design, and because they are easy to re-litigate otherwise.

## The question

The agent currently lives in `packages/espclaw/` as a Lua package running on
breezybox's embedded Lua. Should the core be rewritten in C and linked into the
firmware instead?

## What is already settled

**Shipping with the firmware is not a reason to choose C.** `fs_image/` is
baked into the flash image by `littlefs_create_partition_image(spiffs ... 
FLASH_IN_PROJECT)` in `breezybox-firmware/CMakeLists.txt`, onto a 3 MB LittleFS
partition mounted at `/root`. Staging the package there means `make flash`
writes it and `make final-package` folds it into the single distributable
`.bin`. This is done today (see "Current state").

**Speed is not a reason to choose C.** The measured costs are network and
display, not interpretation:

- a full streamed request takes on the order of seconds, dominated by TLS and
  the API itself
- every console write triggers an LCD redraw; per-token flushing was the
  visible bottleneck, fixed by coalescing output to ~64 bytes in `main.lua`
- Lua spends microseconds per SSE event against milliseconds of network

The freeze observed on 2026-08-18 was a blocking-read bug in `cmd/lua_https.c`
(no `esp_http_client_is_complete_data_received()` check, so the final read
blocked for the full `timeout_ms`). C would not have prevented it.

## The real trade-off: peak RAM

This is the one argument that survives scrutiny.

The `cardputer-claw` profile leaves **~97 KB** internal heap free at rest, and a
TLS request costs **~12.5 KB** transient.

**Measured 2026-08-18:** `claw ask -v` reports **`lua 55 KB`** — the Lua state
plus our modules, resident for the duration of a request. That is at the high
end of what was anticipated, and it changes the picture:

```
  ~97 KB  free at rest (cardputer-claw)
  -55 KB  Lua state during a request
  -12 KB  TLS transient
  =~30 KB headroom, before Phase 4 adds tool schemas and multi-turn replay
```

Note the 55 KB was measured on a *failed* request (0 chunks, TLS handshake
rejected), so it is close to a floor for the Lua runtime itself rather than a
peak including response parsing. Re-measure on a successful streamed reply.

**Measured again 2026-08-18, same firmware and same boot — this is the decisive
data point:**

| | free internal | largest contiguous | result |
|---|---|---|---|
| `clawprobe` (C, no Lua resident) | 97,500 | **57,344** | TLS 401 PASS, low-water 84,900 |
| `claw ask` (Lua resident) | 27,584 | **7,680** | handshake fails, `alloc(1365)` |

The transport layer is fine. The Lua runtime costs ~70 KB of real heap (it
reports 51 KB of its own managed memory; the rest is allocator overhead,
interned strings and C-side structures) **and fragments what remains**: the
largest contiguous block collapses from 57 KB to 7.7 KB. mbedTLS needs
contiguous memory, which is why a 1,365-byte allocation fails with 27 KB
nominally free.

Fragmentation, not total free memory, is the binding constraint — and it is
inherent to how Lua allocates. Trimming modules would recover some total but
would not restore contiguity.

Things ruled out along the way, each by measurement rather than argument:

- the certificate bundle (`DEFAULT_CMN` -> `DEFAULT_FULL`, then pinning a root)
- Bluetooth (removed: 33 KB of static DIRAM, no change to the runtime failure)
- the 36 KB graphics framebuffer (already skipped on this profile)

### It works, on a 6 KB margin

Two further mitigations got a request through end to end (Gemini, streamed SSE,
2026-08-18):

- `MBEDTLS_SSL_IN_CONTENT_LEN` 16384 -> 8192
- one virtual terminal with 32 lines of scrollback instead of two with 96
  (a vterm cell is 2 bytes, so this is ~14 KB)

```
[heap before: free 41900, min 25684, largest 19456]
Hello! How can I help you today?
[heap after:  free 31356, min 6344,  largest 7680]
[2 chunks, 2 events, 799 bytes, 4341 ms, lua 58 KB]
```

**A single trivial request consumes ~35 KB and leaves 6,344 bytes.** The gate in
the original plan was >= 40 KB of retained margin; this is about a sixth of it.

Phase 3 (multi-turn replay) and Phase 4 (tool schemas, tool results) both grow
the Lua heap and the request body. There is no headroom for either. The
remaining Lua-independent levers have been used up:

| Consumer | Status |
|---|---|
| Bluetooth / NimBLE | removed (33 KB static) |
| SSH / libssh | removed (~10 KB static) |
| graphics framebuffer | preallocation skipped (36 KB) |
| second vterm + scrollback | reduced (~14 KB) |
| TLS record buffer | 16 KB -> 8 KB |
| LCD panel framebuffer, 65 KB | **required** |
| console task stack, 16 KB | **Lua runs on it** |

**Conclusion: option B.** Option A is proven viable as a prototype and is worth
keeping until the C core replaces it, but it cannot carry Phases 3-4. The
constraint is not any single component -- it is that a ~70 KB Lua runtime and a
~35 KB TLS request do not both fit in what remains after the display, Wi-Fi and
console take their share.

## Options

### A. Lua, staged into `fs_image/` — current state

- Ships in the `.bin`; no rewrite; keeps the edit → `wget` → run loop.
- Keeps the existing host test suite (`sh tests/run.sh`, 3 suites).
- Lua memory is elastic: allocated on demand, released when `claw` exits.
- Cost: highest peak RAM of the three; bound by the sandbox in `cmd/lua.c`
  (no `io`, `os`, or `debug` — see `tests/test_sandbox.lua`).

### B. Hybrid: C core + Lua skills — matches upstream

Port the agent core to C as a native `claw` console command; keep Lua as the
`run_lua` tool so the model can still write and persist skills.

```
breezybox-cardputer/claw/
  claw_cmd.c        `claw` console command, registered like cmd/ssh.c
  claw_http.c       streaming POST + SSE reader
  claw_backend_*.c  anthropic / openai / gemini
  claw_agent.c      agent loop, tool-iteration cap
  claw_tools.c      capability registry + dispatch
```

- This is exactly upstream ESP-Claw's shape: C core (`claw_modules/`,
  `claw_capabilities/`) with Lua as the capability/skill layer
  (`lua_modules/`).
- Lowest peak RAM; no sandbox limits; tab-completion and `help` integration
  come free from `esp_console`.
- Cost: roughly 1-2 days. The SSE parser and three backends get re-derived in
  C — but the Lua versions serve as executable specs, and
  `tests/test_sse.lua` / `tests/test_backends.lua` fixtures port directly.
- Iteration slows: every change needs a rebuild and reflash (~1-2 min) rather
  than a `wget`.

### C. Full C, no Lua in the agent

Smallest footprint, single artifact. **Not recommended:** it gives up "the model
writes a Lua skill that persists and re-runs later", which is ESP-Claw's
headline feature and the main reason a Cardputer port is interesting at all.
Option B keeps that capability at nearly the same RAM cost.

## Outcome

Ported. `claw 0.2.0 (native)` streams a reply end to end with no Lua resident.

What shipped in `breezybox-cardputer/claw/`:

| File | Role |
|---|---|
| `claw_sse.c` | incremental SSE parser; fixed buffers, no allocation while streaming |
| `claw_config.c` | JSON config, SD-preferred with flash fallback and migration |
| `claw_backend_{anthropic,openai,gemini}.c` | one vtable, three providers |
| `claw_agent.c` | request staged to disk and streamed; response parsed per event |
| `cmd/claw.c` | native console command |

Host tests: `sh tests/c/run.sh` (22 assertions, `-Wall -Wextra -Werror`),
including split-invariance at every byte offset for both stream shapes and
bounded truncation of oversized fields — the last of which the Lua version
could not express.

`packages/espclaw/` is retained as the reference the C was ported from, and
still supplies `ca/gts_root_r1.pem`, which `claw_agent.c` reads at runtime.
Only the CA is staged into the firmware image now.

## Original recommendation (for the record)

**Port the core to option B before starting Phase 3.**

The trigger set out here -- min-free internal heap below ~40 KB during a request
-- was not merely crossed but missed by a factor of six (6,344 bytes), on the
simplest possible request with no history and no tools.

What carries over rather than being thrown away:

- `cmd/lua_https.c` (streaming TLS, `body_file`, `on_chunk`, `on_status`,
  `ca_file`) is already C and needs no rewrite
- the pinned-root work and the whole slim profile are unaffected
- `sse.lua` and the three backends are executable specifications for their C
  equivalents, and `tests/test_sse.lua` / `test_backends.lua` fixtures port
  directly
- Lua stays as the skill runtime, which is the point of option B

## Related finding: cert bundle

`MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN` was tried on the slim profile to save
memory and had to be reverted. Google's `generativelanguage.googleapis.com`
chain fails against the trimmed root set with "Certificate matched but signature
verification failed" (mbedTLS `0x4290`), the usual cross-signed-root symptom.
ESP-IDF 5.5 offers no `MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY` escape
hatch. The profile now uses `DEFAULT_FULL`.

This does not change the language decision, but it does mean the RAM budget
above is measured with the full bundle in place.

## Current state

- `packages/espclaw/` is the canonical source.
- `breezybox-firmware/CMakeLists.txt` copies `packages/espclaw/root/` into
  `fs_image/` at configure time, so the package ships in the firmware image
  without the files being duplicated in git.
- Under option B this staging still applies, for Lua skills rather than the
  agent core. Under option C it would be dropped.
