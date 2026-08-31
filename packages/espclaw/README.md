# ESP-Claw for Cardputer

An on-device AI agent, in the spirit of [espressif/esp-claw](https://github.com/espressif/esp-claw):
the model runs in the cloud, but the agent loop, tool dispatch and memory run on
the Cardputer.

Upstream ESP-Claw requires 8 MB of PSRAM. The Cardputer and Cardputer ADV are
both ESP32-S3FN8 — 8 MB flash, **no PSRAM** — so this is a clean-room, much
leaner implementation rather than a port.

## Why it fits without PSRAM

Nothing scales with conversation length in RAM:

- the transcript is JSONL on the SD card, appended a line at a time
- the request body is assembled on SD and streamed to the API by
  `breezy.https.request{ body_file = ... }`
- the response is streamed back through `on_chunk` and parsed one SSE event at
  a time, never buffered

Measured on the `cardputer-claw` firmware profile: ~97 KB internal heap free at
rest, ~12.5 KB transient cost for a TLS request, and about -456 bytes drift per
request.

## Where settings live

`claw config` writes to **`/sd/claw/config.json`** when an SD card is present,
and falls back to `/root/.claw.json` otherwise.

SD is preferred because `make flash` rewrites the entire LittleFS partition,
wiping `/root` and the API keys with it. The card survives reflashing, so keys
are entered once. An existing `/root/.claw.json` is copied to the card the first
time the card is used, so nothing is lost when you switch.

The trade-off is that the card is removable, so whoever holds it holds the keys.
To keep them on internal flash instead:

```sh
claw config set store flash
```

`store` itself always lives in flash -- it selects which file everything else
uses, so it cannot live in the file it selects. `claw config show` and
`claw stats` both print the path actually in use.

## Install

Copy `root/` onto the device (`/root` or `/sd`), per `manifest.json`. Then:

```sh
claw config set backend anthropic
claw config set model claude-sonnet-5
claw config set anthropic.key sk-ant-...
claw ask "hello"
claw                 # interactive
```

Requires the `cardputer-claw` firmware profile, which provides `breezy.https`
and `breezy.json` and frees the internal SRAM the TLS session needs.

## Status

Early. See `docs/claw.md` in the repo root for the build-out order.
