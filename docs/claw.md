# claw — on-device AI agent

`claw` is an AI agent that runs on the Cardputer itself. The model runs in the
cloud; the agent loop, tool dispatch and conversation memory run on the device.
Ask it a question and it answers. Ask it to do something and it reads files,
runs shell commands, writes Lua, or drives hardware on the Grove port.

It is a clean-room implementation in the spirit of
[espressif/esp-claw](https://github.com/espressif/esp-claw), which requires 8 MB
of PSRAM the Cardputer does not have. See
[claw-architecture.md](claw-architecture.md) for how it fits anyway.

```
$ claw
claw 0.3.0 - gemini / gemini-2.5-flash
session 20260819-104233 (0 turns). Ctrl-D or 'exit' to leave, /help for commands.

claw> what files are on the sd card?

[tool: list_dir]
You have claw/ and espclaw/ directories, plus haiku.txt (142 bytes).

claw> exit
```

## Requirements

The `cardputer-claw` firmware profile. The default `cardputer` build has claw
compiled in, but not the memory savings that let a TLS handshake complete, so
requests will fail. Build and flash it with:

```sh
source ~/esp/esp-idf/export.sh
make build BOARD=cardputer-claw
make flash BOARD=cardputer-claw PORT=/dev/cu.usbmodem1101
```

`BOARD=` is needed on the flash command too, not just the build.

An SD card is recommended but not required: it holds settings, transcripts and
skills, and survives reflashing. Without one, everything falls back to `/root`
in internal flash, which `make flash` erases.

## Setup

```sh
wifi connect MySSID mypassword

claw config set backend gemini
claw config set gemini.key AIza...
claw stats
```

`claw stats` should show your backend, model, endpoint, and `tools yes`.

Three providers are supported, each with its own key, so they can coexist:

```sh
claw config set backend openai     && claw config set openai.key sk-...
claw config set backend anthropic  && claw config set anthropic.key sk-ant-...
claw config set backend gemini     && claw config set gemini.key AIza...
```

Any OpenAI-compatible endpoint works, including a local model on your network:

```sh
claw config set backend openai
claw config set base_url http://192.168.1.10:11434/v1/chat/completions
```

## Using it

Run `claw` with no arguments for an interactive session, or ask one-off
questions from the shell:

```sh
claw ask "how much free memory is there?"
claw ask -v "list the sd card"       # -v adds heap and transport statistics
```

In a session, `/` commands control the agent and everything else is a question:

| Command | Does |
|---|---|
| `/model`, `/model <name>` | show or change the model |
| `/models` | list models from the catalogue |
| `/backend <name>` | switch provider |
| `/new` | start a fresh session |
| `/show` | print the current transcript |
| `/skills` | list saved Lua skills |
| `/stats` | status and memory |
| `/verbose` | toggle transport statistics |
| `/help` | this list |
| `exit`, Ctrl-D | leave |

## Tools

The model can call these. Filesystem access is confined to `/root` and `/sd`,
enforced in the tool layer rather than trusted to the model.

| Tool | Does |
|---|---|
| `read_file` | Read a text file |
| `write_file` | Write or append |
| `list_dir` | List a directory |
| `run_shell` | Run any BreezyBox command and capture its output |
| `run_lua` | Run a Lua script, optionally saving it as a skill |
| `lua_api` | Look up the device's Lua API |
| `i2c_scan` | Probe the Grove port for I2C devices |
| `device_info` | Board, memory and storage |

### Destructive actions ask first

```
claw> delete the test file

[tool: run_shell]

claw wants to run a command that deletes or overwrites data:
  rm /sd/test.txt
allow? [y/N]
```

Prompts appear for `rm`, `rmdir`, `mv`, `format`, `erase`, `mkfs`, `dd`,
`fullclean`, for any command containing `>` or `>>`, and for `write_file` when
the target already exists. Creating a new file does not prompt.

Anything other than a clear `y` declines, and the refusal goes back to the model
as a tool result, so it explains what it could not do instead of failing.

With no interactive prompt available — a non-interactive caller, say —
destructive actions are **refused**, not silently performed.

`claw config set auto_approve true` disables the prompts. It appears in
`claw config show` so it is visible when set.

## Skills

`run_lua` can keep what it writes:

```
claw> write a lua script that shows the battery percentage, save it as battery

[tool: lua_api]
[tool: run_lua]
Battery: 87%
[saved as /sd/claw/skills/battery.lua]
```

A skill is an ordinary Lua file. Run it later with no agent involved:

```sh
lua /sd/claw/skills/battery.lua
```

`claw skills` lists them, `claw skills rm <name>` deletes one. Scripts are
written to disk before they run, so one that crashes is still there to inspect.

## Hardware and the Grove port

`G1` and `G2` on the Grove connector are the only user-free GPIOs; everything
else is committed to the LCD, SD card, speaker, battery sense or keyboard. The
model knows this — `lua_api` serves it a pin table and worked examples.

```
claw> what's plugged into the grove port?

[tool: i2c_scan]
0x68
(scanned SDA=G1 SCL=G2)

claw> read the temperature from it every second for 10 seconds
```

I2C, GPIO, ADC and UART accessories all work. Neither keyboard variant touches
G1 or G2, so accessories behave the same on Cardputer and Cardputer ADV.

## Sessions

Every exchange is appended to a JSONL transcript on the card:

```
/sd/claw/sessions/<id>.jsonl
```

Replay is bounded by **bytes**, not turns: `context_budget` (default 6144) sends
the most recent turns that fit and drops the oldest. This is what keeps peak
memory flat however long a conversation runs — watch `claw ask -v` and the
`heap before` figure stays put as the transcript grows.

```sh
claw session list
claw session show
claw session new
claw session rm <id>
```

## Settings

`claw config show` lists everything. Settings live in `/sd/claw/config.json`
when a card is present, `/root/.claw.json` otherwise.

SD is preferred because `make flash` rewrites the LittleFS partition and would
otherwise wipe your API keys. The trade-off is that the card is removable, so
whoever holds it holds the keys; `claw config set store flash` forces internal
storage. An existing flash config is copied to the card the first time it is
used, so nothing is lost when switching.

| Key | Default | Meaning |
|---|---|---|
| `backend` | `anthropic` | `anthropic`, `openai` or `gemini` |
| `model.<backend>` | first in catalogue | model, stored per backend |
| `max_tokens` | 2048 | reply length cap |
| `context_budget` | 6144 | bytes of transcript replayed per request |
| `base_url` | — | override the endpoint |
| `ca_file` | — | pin a root CA |
| `timeout_ms` | 60000 | request timeout |
| `auto_approve` | false | skip destructive-action prompts |
| `store` | auto | `flash` to keep settings off the card |
| `<backend>.key` | — | API key, masked in output |

Models are stored per backend, so switching providers cannot leave a Gemini
model pointed at OpenAI.

## The model catalogue

`/sd/claw/models.json` lists the models offered by `/models`. It is data, not
code — add a model the firmware has never heard of by editing the file:

```json
{
  "gemini":    ["gemini-2.5-flash", "gemini-2.5-pro"],
  "openai":    ["gpt-4o-mini", "gpt-4o"],
  "anthropic": ["claude-sonnet-5", "claude-opus-5"]
}
```

The first entry for a backend is its default. Any model name is accepted
whether or not it is listed; the catalogue is a convenience, not a whitelist.
It is seeded from the shipped copy on first run.

## Memory budget

The `cardputer-claw` profile exists because a TLS handshake needs contiguous
memory the stock build does not have. It drops Bluetooth, SSH and the ELF
loader, skips a 36 KB graphics framebuffer, halves the TLS record buffer, and
runs one virtual terminal instead of two.

Measured on device:

```
heap before: free 110784, min 78136, largest 54272
```

Against a 40 KB target for min-free during a request. The full derivation,
including why the Lua prototype left only 6 KB, is in
[claw-architecture.md](claw-architecture.md).

`claw ask -v` prints these figures per request. If `min` approaches zero,
lower `context_budget` or `max_tokens` first.

## Troubleshooting

**`no API key set`** — `claw config set <backend>.key <key>`.

**`no network`** — `wifi connect <ssid> <password>`, check with `wifi status`.

**Handshake fails with an allocation error** — you are probably on the stock
`cardputer` build. Check `claw stats`: it prints the profile and build time.

**`warning - CA file missing, using cert bundle instead`** on Gemini — the
pinned root is not installed and the bundle cannot verify Google's chain.
Reflash, or copy `ca/gts_root_r1.pem` next to the other files.

**The model calls the same tool repeatedly** — claw stops and says so. It means
tool results are not reaching the model. Run `/verbose` to see what is being
sent back.

**Answers ignore earlier turns** — the transcript is trimmed to
`context_budget`. Raise it, or `/new` for a fresh session.

## Design notes

Three properties shape the implementation, all following from having no PSRAM:

- **The request body is staged to disk and streamed**, so conversation length is
  bounded by the SD card rather than the heap.
- **The response is parsed one SSE event at a time** and never buffered. A long
  reply costs no more memory than a short one.
- **Tool arguments accumulate into a fixed buffer**, and overflow is reported
  rather than truncated — half a JSON object parses into something plausible and
  wrong.

The agent core is C, in `breezybox-cardputer/claw/`. It was prototyped in Lua;
that version worked but left 6 KB of headroom because the runtime cost ~70 KB
and fragmented the heap. Lua remains as the *skill* runtime, which is the
arrangement upstream ESP-Claw uses too.

Host tests, no device needed:

```sh
sh tests/c/run.sh
```

They cover the SSE parser (including identical output at every possible chunk
boundary) and the destructive-command classifier and path confinement — the two
places where a mistake costs data rather than a retry.
