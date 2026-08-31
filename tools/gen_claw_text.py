#!/usr/bin/env python3
"""
Generate the shipped default text files for claw's SD overrides.

Extracted from the sources so the defaults cannot drift from what the firmware
actually uses -- the same discipline as tools/gen_lua_api.py. A stale defaults
file would be worse than none, because `claw text dump` hands it to the user as
a starting point and they would be editing text the firmware never reads.

Writes into packages/espclaw/root/apps/espclaw/, which ships to
/root/apps/espclaw/ in the firmware image:

    prompt.md       the system prompt
    tools.json      tool and parameter descriptions
    messages.json   the failure messages

Run from the repo root after changing any of that text:
    python3 tools/gen_claw_text.py
"""
import json
import re
import sys
from pathlib import Path

SRC = Path("breezybox-cardputer/claw")
OUT = Path("packages/espclaw/root/apps/espclaw")


def c_unescape(s: str) -> str:
    return (s.replace('\\n', '\n').replace('\\t', '\t')
             .replace('\\"', '"').replace('\\\\', '\\'))


def joined_literals(text: str) -> str:
    """Concatenate adjacent C string literals, as the compiler does."""
    return c_unescape("".join(re.findall(r'"((?:[^"\\]|\\.)*)"', text)))


def extract_prompt() -> str:
    src = (SRC / "claw_prompt.c").read_text()
    m = re.search(r'static const char k_device_prompt\[\] =\s*(.*?);\n', src, re.S)
    if not m:
        sys.exit("error: could not find k_device_prompt")
    return joined_literals(m.group(1))


def extract_tools() -> dict:
    src = (SRC / "claw_tools.c").read_text()
    tools: dict = {}

    # k_tools registry: { "name", "description", schema, run },
    m = re.search(r'static const claw_tool_t k_tools\[\] = \{(.*?)\n\};', src, re.S)
    if m:
        for entry in re.finditer(
                r'\{\s*"(\w+)",\s*((?:"(?:[^"\\]|\\.)*"\s*)+),', m.group(1)):
            name, desc = entry.group(1), joined_literals(entry.group(2))
            tools.setdefault(name, {})["description"] = desc

    # add_prop_id(props, "<tool>", "<prop>", "<type>", "<description>")
    for m2 in re.finditer(
            r'add_prop_id\(props,\s*"(\w+)",\s*"(\w+)",\s*"\w+",\s*'
            r'((?:\s*"(?:[^"\\]|\\.)*"\s*)+)\)', src):
        tool, prop, desc = m2.group(1), m2.group(2), joined_literals(m2.group(3))
        tools.setdefault(tool, {}).setdefault("param", {})[prop] = desc

    return tools


def extract_messages() -> dict:
    msgs: dict = {}
    for name in ("claw_tools.c", "claw_agent.c"):
        src = (SRC / name).read_text()
        for m in re.finditer(
                r'claw_text\("([\w.]+)",\s*((?:\s*"(?:[^"\\]|\\.)*"\s*)+)\)', src):
            msgs[m.group(1)] = joined_literals(m.group(2))
    return dict(sorted(msgs.items()))


def main() -> int:
    if not SRC.is_dir():
        print(f"error: {SRC} not found (run from the repo root)", file=sys.stderr)
        return 1

    OUT.mkdir(parents=True, exist_ok=True)

    prompt = extract_prompt()
    tools = extract_tools()
    msgs = extract_messages()

    (OUT / "prompt.md").write_text(prompt)
    (OUT / "tools.json").write_text(json.dumps(tools, indent=2) + "\n")
    (OUT / "messages.json").write_text(json.dumps(msgs, indent=2) + "\n")

    params = sum(len(t.get("param", {})) for t in tools.values())
    print(f"wrote {OUT}/")
    print(f"  prompt.md      {len(prompt)} bytes")
    print(f"  tools.json     {len(tools)} tools, {params} parameters")
    print(f"  messages.json  {len(msgs)} messages")
    return 0


if __name__ == "__main__":
    sys.exit(main())
