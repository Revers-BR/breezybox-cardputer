# AGENTS

## Project Split

What’s still in `breezybox-firmware/`:

- Project/build files:
  - `breezybox-firmware/CMakeLists.txt`
  - `breezybox-firmware/Makefile`
  - `breezybox-firmware/partitions.csv`
  - `breezybox-firmware/sdkconfig`
  - `breezybox-firmware/sdkconfig.defaults`
  - `breezybox-firmware/dependencies.lock`
- App-specific firmware code in `breezybox-firmware/main`:
  - `breezybox-firmware/main/main.c`
  - Cardputer keyboard/input:
    - `breezybox-firmware/main/cardputer_keyboard.c`
    - `breezybox-firmware/main/cardputer_keyboard.h`
  - Console/display bridge:
    - `breezybox-firmware/main/my_console_io.c`
    - `breezybox-firmware/main/my_console_io.h`
  - Built-in app wrappers:
    - `breezybox-firmware/main/cmd_vi_builtin.c`
    - `breezybox-firmware/main/cmd_plasma_builtin.c`
    - `breezybox-firmware/main/cmd_termbench_builtin.c`
    - `breezybox-firmware/main/cmd_wget_builtin.c`
    - `breezybox-firmware/main/cmd_gzip_builtin.c`
    - `breezybox-firmware/main/cmd_gunzip_builtin.c`
    - `breezybox-firmware/main/cmd_testgfx.c`
    - `breezybox-firmware/main/cmd_app_compat.c`
  - Loader symbol table patch:
    - `breezybox-firmware/main/all_my_symbols.c`
- Local overridden components that are still Cardputer-specific or patched:
  - `breezybox-firmware/components/valdanylchuk__breezy_rgb_lcd`
  - `breezybox-firmware/components/valdanylchuk__breezy_term`
  - `breezybox-firmware/components/espressif__elf_loader`
- Managed dependencies still fetched normally:
  - `breezy_bt`
  - `littlefs`
  - `zlib`
  - cmake utilities
  - under `breezybox-firmware/managed_components`
- Runtime/build artifacts:
  - `breezybox-firmware/fs_image`
  - `breezybox-firmware/build`

What is no longer in `breezybox-firmware/` as the active shell core:

- `components/valdanylchuk__breezybox`
- that moved to `breezybox-cardputer`

So the split is now:

- `breezybox-cardputer/` = shell core
- `breezybox-firmware/` = Cardputer firmware app, board glue, built-in app wrappers, patched display/term/loader components

Built-in app source ownership:

- `breezybox-cardputer/apps/` now holds the local app sources that the firmware wrappers compile in:
  - `vi`
  - `plasma`
  - `termbench`
  - `wget`
  - `gzip`
  - `gunzip`

## Scripting Direction

The old dedicated MicroPython runtime path is no longer part of the active
product configuration.

The active replacement path is now:

- embedded Lua inside the main `breezybox-firmware/` image
- no second app partition
- no boot handoff between runtimes
- lower RAM and flash overhead than the old dual-runtime MicroPython model

Current active Lua surface:

- `lua`
- `lua shell`
- `lua -e <chunk>`
- `lua <script.lua> [args...]`
- a built-in `breezy` Lua module for shell/filesystem basics
- `breezy.gfx` for simple graphics-mode drawing from Lua

Treat `micropython-cardputer/`, `MicroPythonShell-main/`, and
`Cardputer-MicroHydra/` as inactive reference material unless a future task
explicitly revives that path.

## ELF Loader Note

Original Waveshare `breezydemo` could run external ELF apps because its memory layout and board assumptions were more favorable to the loader, including PSRAM-oriented expectations.

This Cardputer ADV port is different:

- `CONFIG_SPIRAM` is not enabled in the active firmware build
- runtime testing showed `EXEC free=0 largest=0` before ELF launch
- when the loader relocates into normal SRAM/DRAM instead, that code path is not executable here
- jumping to the relocated ELF entry causes `InstructionFetchError`

So external ELF apps are currently not a reliable execution model on this Cardputer build. Built-in apps are the supported path unless the loader/memory strategy is redesigned.


## claw (AI agent)

`claw` is an on-device AI agent. User documentation is `docs/claw.md`; the
design record, including the measurements behind it, is
`docs/claw-architecture.md`.

Where things live:

- `breezybox-cardputer/claw/` agent core, C:
  - `claw_agent.c` request/response round plus the tool loop
  - `claw_backend{,_anthropic,_openai,_gemini}.c` one vtable, three providers
  - `claw_config.c` settings, SD-preferred with flash fallback
  - `claw_memory.c` long-term memory: index injected per request, bodies on demand
  - `claw_models.c` model catalogue, read from JSON not compiled in
  - `claw_session.c` JSONL transcripts, replayed under a byte budget
  - `claw_sse.c` incremental SSE parser, fixed buffers
  - `claw_tools.c` capability registry, path confinement, destructive-action guard
- `breezybox-cardputer/cmd/claw.c` the `claw` console command and REPL
- `breezybox-cardputer/cmd/lua_https.c` `breezy.https` binding (Lua-side TLS)
- `packages/espclaw/root/apps/espclaw/` data staged into the firmware image:
  `ca/gts_root_r1.pem`, `models.json`, `lua_api.md`
- `tools/gen_lua_api.py` regenerates `lua_api.md` from the bindings in
  `cmd/lua.c` and `cmd/lua_led.c`, and folds in the usage block from
  `docs/lua.md`. Run it after changing any Lua binding or that block. The
  reference is generated rather than written so it cannot drift; a stale
  reference is worse than none, because the model believes it.
- `tests/c/` host tests for the C core; `sh tests/c/run.sh`

Things worth knowing before changing it:

- The agent requires the `cardputer-claw` build profile. On the stock
  `cardputer` build it compiles and runs but TLS handshakes fail for want of
  contiguous memory.
- `claw_session_replay()` returns neutral `{role, content}` turns so the
  transcript stays provider-agnostic. Backends append tool turns in their own
  native shape into the same array, so **every `build_body` must pass native
  turns through untouched**. Getting this wrong makes the model repeat a tool
  call forever; there is a guard for exactly that.
- Tool failures must say what would have worked. The model cannot consult docs
  it does not have, so an error message *is* the documentation at that moment: a
  missing section lists the real ones, an unknown shell command lists the real
  ones, a nil module names the real ones. Several wasted rounds came from tools
  that reported failure without direction.
- `claw_prompt.c` describes the device on every request, and lists the shell
  commands by enumerating the live registry rather than repeating them, so the
  prompt matches the build. Anything the model
  reliably gets wrong belongs there rather than in a tool's error path, which
  only fires after a turn has already been spent.
- Only the memory *index* is injected into requests (capped at
  `CLAW_MEMORY_INJECT_MAX`); bodies are fetched with `memory_read`. Injecting
  everything would spend the context budget on usually-irrelevant facts.
- Anthropic takes the system prompt as a top-level field and rejects
  `role: "system"` inside messages, so its `build_body` hoists it out. Gemini
  hoists it to `systemInstruction`. OpenAI takes it as a message.
- Nothing may scale with conversation length in RAM. The request body is staged
  to disk and streamed; responses are parsed per SSE event and never buffered.
- `packages/espclaw/` also contains a superseded Lua implementation of the
  agent, kept as the reference the C was ported from. Only `ca/`, `models.json`
  and `lua_api.md` are shipped.

## Repo Layout

- `breezybox-firmware/` active Cardputer ADV firmware
- `breezybox-cardputer/apps/` built-in app sources and related assets
- `breezybox-cardputer/claw/` AI agent core (see above)
- `packages/espclaw/` agent data files, and the superseded Lua prototype

## Build Profiles

- `cardputer` universal image for Cardputer and Cardputer ADV
- `cardputer-adv` as above; kept for compatibility, the keyboard is detected at runtime
- `cardputer-claw` the agent profile: Bluetooth, SSH and the ELF loader
  dropped, no graphics-framebuffer preallocation, 8 KB TLS record buffer, one
  virtual terminal. Frees roughly 100 KB of internal SRAM, which is what makes
  a TLS handshake possible.
- `sticks3` M5StickC S3




## Current Limits

Important current limits:

- terminal geometry is fixed at `40x16`
- on `cardputer-claw` there is one virtual terminal and no Bluetooth or SSH
- claw tool support is implemented for all three providers, but only the Gemini
  path has been exercised on hardware
- runtime font scaling is not implemented
- external ELF app execution is not the primary supported path
- `ln` supports hard links only
- `sed` supports simple substitution form only:
  - `s/old/new/`
  - `s/old/new/g`
