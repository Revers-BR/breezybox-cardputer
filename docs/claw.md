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

Other subcommands: `claw session`, `claw skills`, `claw memory`, `claw text`,
`claw model`, `claw backend`, `claw config`, `claw stats`. Each is covered
below; `claw help` lists them.

In a session, `/` commands control the agent and everything else is a question:

| Command | Does |
|---|---|
| `/model`, `/model <name>` | show or change the model |
| `/models` | list models from the catalogue |
| `/backend`, `/backend <name>` | show or switch provider |
| `/new` | start a fresh session |
| `/show` | print the current transcript |
| `/skills` | list saved Lua skills |
| `/memory` | list what claw remembers |
| `/reload` | re-read the prompt and tool text from SD |
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
| `lua_api` | Look up the device's Lua API, a section at a time, with usage examples |
| `i2c_scan` | Probe the Grove port for I2C devices |
| `memory_save` | Remember something for future conversations |
| `memory_read` | Read a stored memory in full |
| `memory_forget` | Delete a memory (asks first) |
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

### Addressable LED units (NeoPixel / WS2812)

These are **not** I2C devices. They take data on `G2`, and pointing `i2c_scan`
at one drives 128 addresses of garbage into that line, lighting the LEDs at
arbitrary values -- which spikes current, browns the board out, and leaves the
strip lit, because WS2812s latch their last value.

Drive them with `breezy.led` instead:

```lua
local breezy = require("breezy")
local led = breezy.led

led.open(2, 3, { brightness = 25 })   -- pin G2, 3 LEDs
led.fill(0, 40, 0)                    -- dim green
led.show()                            -- nothing reaches the strip until this
breezy.sleep(1)
led.close()                           -- clears before releasing
```

Timing is handled by the RMT peripheral, so it is not affected by what the CPU
is doing. Brightness is scaled and defaults to 25%: full white on the Grove rail
is exactly what causes a brownout.

```
claw> set the light to dim blue
[tool: run_lua]
```

If a strip is stuck on, `led.open(...)` then `led.close()` clears it.

### Powering accessories

The Grove `5V` pin shares the board's rail. An accessory that draws real
current -- a relay, a bright LED, a motor, anything with a coil -- can pull the
supply down far enough to trip the ESP32-S3 brownout detector, which resets the
device instantly:

```
E BOD: Brownout detector was triggered
rst:0x3 (RTC_SW_SYS_RST)
```

If that happens:

- **Run from USB, not battery.** The battery path has less headroom, and a
  scan or a switch-on is a current spike.
- **Power the accessory separately** and share only ground plus the signal
  lines, for anything beyond a sensor.
- Sensors (I2C temperature, pressure, IMU) are typically fine; switching and
  lighting units usually are not.

This is a hardware limit, not something firmware can work around. `i2c_scan`
releases the pins when it finishes so nothing is left sinking current, but it
cannot conjure supply headroom.

## What the model is told

Every request carries a short system prompt describing the device: that it is an
ESP32-S3 and not Linux, that `run_shell` is an embedded shell with no bash or
curl, that Lua is sandboxed with no `io` or `os`, where files live, and that
answers are read on a 40-column screen.

The available shell commands are listed in it too, read from the live command
registry rather than written down -- so the list cannot go stale, and reflects
the build actually running (the slim profile has no Bluetooth commands, and says
so). About 100 tokens to save the model discovering them by trial.

It exists because without it the model assumes a Linux box -- reaching for
`curl`, shell scripts and a `breezy.fs` module that does not exist, then
reasoning from the failure and usually concluding a capability is missing when
it is only spelled differently here. Roughly 300 tokens per request to avoid
several wasted round trips.

Add your own instructions in `/sd/claw/system.md` and they are appended:

```sh
echo "Prefer metric units. I am building a weather station." > /sd/claw/system.md
```

## Tuning what the model is told

Every string that steers the model can be replaced by a file on the SD card, so
rewording needs no rebuild and no flash:

```
/sd/claw/prompt.md      replaces the device description
/sd/claw/tools.json     tool and parameter descriptions
/sd/claw/messages.json  the messages tools return when something fails
/sd/claw/system.md      appended to the prompt (unchanged)
```

Start from the shipped defaults:

```sh
claw text dump      # writes the three files to /sd/claw
claw text status    # shows which are active
```

Then edit and reload without leaving a session:

```
claw> /reload
reloaded text overrides
```

**Nothing is seeded automatically.** An absent file means the compiled text, so
a firmware update carrying better wording still takes effect. Overriding is a
deliberate act, and deleting a file returns that text to the built-in version.

**Fallback is per key, not per file.** A `tools.json` missing a tool, or a
`messages.json` missing an id, falls back for that key alone. A malformed file
falls back entirely and says so in `claw text status`.

### Format specifiers are checked

Many messages contain `%s`, `%d` or `%u` and are filled in at the call site.
`"wrote %u bytes to %s"` is a contract: an override reading `"wrote %s bytes"`
would make the code read a number as a pointer.

So an override is only accepted if its conversion specifiers match the built-in
text exactly, in type and order. Reword freely, add or remove `%%`, change width
and flags — but change a `%d` to a `%s`, reorder them, or drop one, and the
override is refused, the built-in text is used, and `claw text status` reports
it.

The message ids are derived from their own text (`tools.read_file.error_cannot_open`),
so they stay stable when messages are added or reordered.

Regenerate the shipped defaults after changing any of this text in C:

```sh
python3 tools/gen_claw_text.py
```

## Memory

Sessions are what was said; memory is what the device should still know next
week. Memory persists across sessions and reboots.

```
claw> the sensor on my grove port is a BMP280 at 0x76, remember that

[tool: memory_save]
Noted.

claw> /new
claw> what's on my grove port?

[tool: memory_read]
A BMP280 barometric sensor at address 0x76.
```

Stored on the card as one file per memory, plus an index:

```
/sd/claw/memory/MEMORY.md      index: one line per memory
/sd/claw/memory/<name>.md      the memory itself
```

**Only the index is sent with each request**, capped at 768 bytes. Sending
every memory would spend the context budget on facts that are usually
irrelevant; sending nothing means the model never knows to look. The index is
small enough to carry always and specific enough to prompt a `memory_read` when
something matters.

Inspect it yourself:

```sh
claw memory list
claw memory show grove-sensor
claw memory rm grove-sensor
```

`/memory` does the same inside a session. `claw stats` shows how many memories
are stored. `memory_forget` asks before deleting, like other destructive
actions; `claw memory rm` does not, since you are the one typing it.

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

## Choosing a model

```sh
claw models                  # list the catalogue, marking the current model
claw model                   # show the current backend and model
claw model gemini-2.5-pro    # change it
claw backend openai          # switch provider
```

The same as `/models`, `/model` and `/backend` inside a session. The model is
stored per backend, so switching provider cannot leave a Gemini model pointed at
OpenAI.

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

## Graphics and the agent share memory

A pixel mode needs one contiguous 36 KB framebuffer, and so does the agent for
its stream parser and request serialisation. On a board with no PSRAM there is
one such block, so they take turns.

The framebuffer is reserved at boot, while the heap is still whole. `claw`
releases it on entry and takes it back on exit, so both work in a single
session:

```sh
claw ask "..."                    # the agent has the block
lua /sd/claw/skills/rainbow.lua   # graphics has it back
```

Reclaiming can fail, because the network stack retains a few hundred bytes per
request and that is enough to stop 36 KB coalescing. ESP-IDF cannot compact a
heap, so there is no fix beyond rebooting. `claw` says so on exit when it
happens:

```
note: the graphics framebuffer could not be reclaimed (largest block 31744
      of 36000 needed).
      Reboot before running a graphics script.
```

In practice: run graphics scripts before a long agent session, or reboot
between. Writing a graphics script with the agent and then running it is the
awkward case, and a reboot is the honest answer.

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

**The device resets when touching a Grove accessory** — `E BOD: Brownout
detector was triggered`. The accessory is drawing more than the rail can
supply. Run from USB rather than battery, or power the accessory separately.
See [Powering accessories](#powering-accessories).

**Answers ignore earlier turns** — the transcript is trimmed to
`context_budget`. Raise it, or `/new` for a fresh session.

**It forgot something you told it to remember** — check `claw memory list`. If
it is there but unused, the index line may be too vague to prompt a
`memory_read`; ask it to save again with a clearer description. Only the index
travels with each request, not the contents.

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
