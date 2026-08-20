#!/usr/bin/env python3
"""
Generate the Lua API reference the agent reads before writing scripts.

Extracted from the luaL_Reg tables in cmd/lua.c so it cannot drift from what the
firmware actually exposes -- a stale reference is worse than none, because the
model believes it and writes code that fails.

Run from the repo root after changing any binding:
    python3 tools/gen_lua_api.py
"""
import re
import sys
from pathlib import Path

SRCS = [Path("breezybox-cardputer/cmd/lua.c"),
        Path("breezybox-cardputer/cmd/lua_led.c")]
USAGE_DOC = Path("docs/lua.md")
OUT = Path("packages/espclaw/root/apps/espclaw/lua_api.md")

# Registered outside the s_breezy_* tables.
EXTRA = {"https": ["request"]}

# Signatures worth spelling out, keyed by "module.function".
HINTS = {
    "battery.read_pct":   "() -> 0..100",
    "battery.read_uv":    "() -> microvolts",
    "battery.read_level": "() -> 0..4",
    "read_file":          "(path) -> string",
    "write_file":         "(path, text[, append])",
    "listdir":            "(path) -> table of names",
    "exists":             "(path) -> boolean",
    "exec":               "(command) -> output string",
    "heap":               "() -> free, min_free, largest",
    "write":              "(text)  -- no trailing newline, unlike print()",
    "log":                "(...)  -- print with a timestamp and append to a log file",
    "term_size":          "() -> cols, rows",
    "readkey":            "(timeout_ms) -> char or nil",
    "json.encode":        "(value) -> string",
    "json.decode":        "(string) -> value",
    "storage.sd_mounted": "() -> boolean",
    "network.is_connected": "() -> boolean",
    "network.http_get":   "(url) -> {status=, body=}  -- http:// only",
    "https.request":      "{url=, method=, headers=, body=, body_file=, on_chunk=, on_status=, timeout_ms=} -> status, bytes[, body]  -- or just a URL string; without on_chunk the body is returned",
    "gfx.mode":           '("text"|"150p"|"vga13h")  -- pixel modes need a large contiguous framebuffer and can fail; use breezy.tui for text',
    "sound.tone":         "(hz, ms)",
    "i2c.scan":           "() -> table of addresses",
    "pin.mode":           '(gpio, "in"|"out")',
    "led.open":           "(pin, count[, {brightness=0..100}])  -- WS2812/NeoPixel",
    "led.set":            "(index, r, g, b)  -- index is 1-based",
    "led.fill":           "(r, g, b)",
    "led.show":           "()  -- nothing reaches the strip until this is called",
    "led.brightness":     "([pct]) -> pct",
    "led.close":          "()  -- clears the strip first; it latches otherwise",
    "mkdir":              "(path) -> true  -- succeeds if it already exists",
    "remove":             "(path) -> true  -- file, or empty directory",
    "rename":             "(from, to) -> true",
    "stat":               "(path) -> {size=, dir=, mtime=} or nil",
    "time.now":           "() -> unix seconds",
    "time.date":          '([format[, when]]) -> string, strftime formats',
    "time.set":           "(unix_seconds)",
    "time.is_set":        "() -> boolean  -- false when the clock was never set",
}


def collect_usage() -> dict[str, list[str]]:
    """Usage lines per module, taken from the example block in docs/lua.md.

    That block is curated and already maintained alongside the bindings, so
    sourcing from it keeps a single copy: a hand-written second set of snippets
    here would drift the moment an API changed.
    """
    usage: dict[str, list[str]] = {}
    if not USAGE_DOC.exists():
        return usage

    text = USAGE_DOC.read_text()
    block = re.search(r"```lua\n(.*?)```", text, re.S)
    if not block:
        return usage

    current = None      # module a multi-line call belongs to
    depth = 0           # unclosed brackets, so continuations stay attached
    last_key = None     # module the previous line belonged to

    for raw in block.group(1).splitlines():
        line = raw.rstrip()
        stripped = line.strip()
        if not stripped or stripped.startswith("--"):
            continue

        if depth > 0 and current:
            # Continuation of a call that spans lines.
            usage[current].append(line)
            depth += line.count("(") + line.count("{") - line.count(")") - line.count("}")
            if depth <= 0:
                depth, current = 0, None
            continue

        # Lua allows both f(...) and f{...}, and the table form is common for
        # option arguments -- breezy.https.request{...} among them.
        m = re.search(r"breezy\.(\w+)\.\w+\s*[({]", stripped)
        key = m.group(1) if m else (
            "lib" if re.search(r"breezy\.\w+\s*[({]", stripped) else None)
        if not key:
            # A bare print() right after a breezy call is showing its result,
            # so keep it with that module rather than dropping it.
            if last_key and stripped.startswith("print("):
                usage[last_key].append(line)
            continue

        usage.setdefault(key, []).append(line)
        last_key = key
        depth = line.count("(") + line.count("{") - line.count(")") - line.count("}")
        if depth > 0:
            current = key
        else:
            depth = 0
    return usage


def main() -> int:
    missing = [p for p in SRCS if not p.exists()]
    if missing:
        print(f"error: {missing[0]} not found (run from the repo root)", file=sys.stderr)
        return 1

    src = "\n".join(p.read_text() for p in SRCS)
    mods: dict[str, list[str]] = {}
    for m in re.finditer(r"static const luaL_Reg (s_breezy_\w+)\[\] = \{(.*?)\n\};", src, re.S):
        name = m.group(1).replace("s_breezy_", "").replace("_lib", "")
        mods[name] = re.findall(r'\{\s*"([^"]+)"', m.group(2))
    mods.update(EXTRA)

    all_modules = set(mods) - {"lib"}
    usage = collect_usage()

    if "lib" not in mods:
        print("error: could not find the core breezy table", file=sys.stderr)
        return 1

    lines = [
        "# breezy Lua API",
        "",
        "Generated by tools/gen_lua_api.py from the bindings in cmd/lua.c.",
        "Do not edit by hand; regenerate instead.",
        "",
        "Every script starts with:",
        "",
        "```lua",
        'local breezy = require("breezy")',
        "```",
        "",
        "`breezy` is a module, not a global. There is no `io` or `os` library:",
        "use `print()`, or `breezy.write()` to write without a newline.",
        "",
        "## Core",
        "",
    ]

    for fn in mods.pop("lib"):
        hint = HINTS.get(fn, "")
        lines.append(f"- `breezy.{fn}{hint}`" if hint else f"- `breezy.{fn}()`")

    if usage.get("lib"):
        lines += ["", "```lua"] + usage["lib"] + ["```"]

    for mod in sorted(mods):
        lines += ["", f"## breezy.{mod}", ""]
        for fn in mods[mod]:
            key = f"{mod}.{fn}"
            hint = HINTS.get(key, "")
            lines.append(f"- `breezy.{mod}.{fn}{hint}`" if hint
                         else f"- `breezy.{mod}.{fn}()`")
        if usage.get(mod):
            lines += ["", "```lua"] + usage[mod] + ["```"]

    lines += [
        "",
        "## Grove port",
        "",
        "The Grove connector is HY2.0-4P: GND, 5V, G2, G1.",
        "",
        "`G1` and `G2` are the only user-free GPIOs, and every other pin below is",
        "already committed to something. Accessories connect here.",
        "",
        "```lua",
        'local breezy = require("breezy")',
        "",
        "-- I2C accessory (most M5Stack Units): SDA=G1, SCL=G2",
        "breezy.i2c.open(1, 2, { freq = 400000 })",
        "for _, addr in ipairs(breezy.i2c.scan()) do",
        '  print(string.format("device at 0x%02X", addr))',
        "end",
        "breezy.i2c.close()",
        "",
        "-- Digital out, e.g. a relay or LED unit on G2",
        'breezy.pin.mode(2, "out")',
        "breezy.pin.write(2, 1)",
        "",
        "-- Digital in, e.g. a button unit on G1",
        'breezy.pin.mode(1, "in")',
        "print(breezy.pin.read(1))",
        "",
        "-- Analog in on G1 (ADC1 channel 0)",
        "print(breezy.adc.read(0))",
        "",
        "-- Addressable LED / NeoPixel unit, data on G2",
        "-- These are NOT I2C. Scanning them feeds garbage into the data line,",
        "-- lights the LEDs at random values and can brown out the board.",
        "local led = breezy.led",
        "led.open(2, 3, { brightness = 25 })   -- pin, number of LEDs",
        "led.fill(0, 40, 0)                    -- dim green",
        "led.show()",
        "breezy.sleep(1)",
        "led.close()                           -- clears before releasing",
        "",
        "-- Serial accessory: TX=G1, RX=G2",
        "local h = breezy.uart.open(1, 115200, 1, 2)",
        'breezy.uart.write(h, "hello\\r\\n")',
        "print(breezy.uart.read(h, 64, 100))",
        "breezy.uart.close(h)",
        "```",
        "",
        "## Power",
        "",
        "The Grove `5V` pin shares the board rail. A relay, bright LED or motor",
        "can pull it down enough to trip the brownout detector and reset the",
        "device. Sensors are usually fine; switching loads usually are not. Warn",
        "the user and suggest USB power or a separate supply when a script is",
        "about to switch something on.",
        "",
        "A WS2812 draws up to ~60 mA at full white, *per LED*, from the same",
        "rail as the CPU, screen and radio. A few LEDs at low brightness are",
        "fine; a strip at full white is not. `breezy.led` warns when the",
        "estimate is high. Prefer dim colours, and tell the user when a strip",
        "should have its own supply.",
        "",
        "Release pins when finished (`breezy.pin.mode(n, \"in\")`, `breezy.i2c.close()`)",
        "so nothing is left sinking current.",
        "",
        "## Pins in use",
        "",
        "| Function | Pins |",
        "|---|---|",
        "| LCD (ST7789) | MOSI 35, SCLK 36, CS 37, DC 34, RST 33, backlight 38 |",
        "| microSD | CLK 40, MOSI 14, MISO 39, CS 12 |",
        "| Speaker (I2S) | BCLK 41, WS 43, DOUT 42 |",
        "| Battery sense | GPIO 10 (ADC) |",
        "| Keyboard (v1.1 matrix) | out 8, 9, 11; in 13, 15, 3, 4, 5, 6, 7 |",
        "| Keyboard (ADV, TCA8418) | I2C SDA 8, SCL 9 |",
        "",
        "The keyboard occupies different pins on the two boards, but neither uses",
        "G1 or G2, so Grove accessories work identically on both.",
        "",
        "## Notes",
        "",
        "- Filesystem roots are `/root` (internal flash) and `/sd` (card).",
        "- `breezy.network.http_*` is plain HTTP only; use `breezy.https.request`",
        "  for TLS.",
        "- Graphics calls need `breezy.gfx.mode(...)` first, and `mode(\"text\")`",
        "  afterwards to restore the console.",
        "",
    ]

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(lines))
    total = sum(len(v) for v in mods.values()) + len(mods)
    print(f"wrote {OUT} ({total}+ functions)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
