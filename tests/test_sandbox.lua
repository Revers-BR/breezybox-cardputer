-- Guards against using Lua stdlib that the device sandbox does not provide.
--
-- cmd/lua.c opens only: base, package, coroutine, table, string, math, utf8.
-- There is no io, os or debug library, so `io.write` / `os.exit` compile fine
-- on the host and then fail at runtime on the Cardputer. This test catches
-- that without a device.
--
-- Run: lua tests/test_sandbox.lua

local SRC_DIR = "packages/espclaw/root/apps/espclaw/"
local FILES = {
  "main.lua",
  "config.lua",
  "sse.lua",
  "backend_anthropic.lua",
  "backend_openai.lua",
  "backend_gemini.lua",
}

-- Globals the device does NOT provide.
local FORBIDDEN = {
  "io", "os", "debug",
}

-- Base-library functions that exist but are unusable without a filesystem lib.
local FORBIDDEN_FN = {
  "dofile", "loadfile",
}

local failures = 0
local function fail(msg)
  failures = failures + 1
  print("  FAIL " .. msg)
end

-- Strip comments and string literals so we do not flag prose or messages.
local function strip(src)
  src = src:gsub("%-%-%[%[.-%]%]", " ")     -- long comments
  src = src:gsub("%-%-[^\n]*", " ")         -- line comments
  src = src:gsub('%[%[.-%]%]', '" "')       -- long strings
  src = src:gsub('"[^"\n]*"', '" "')        -- double-quoted
  src = src:gsub("'[^'\n]*'", '" "')        -- single-quoted
  return src
end

print("sandbox conformance")
for _, name in ipairs(FILES) do
  local f = io.open(SRC_DIR .. name, "r")   -- host-side io is fine
  if not f then
    fail(name .. ": not found")
  else
    local src = strip(f:read("a"))
    f:close()

    for _, g in ipairs(FORBIDDEN) do
      -- `g.` as a table access, not preceded by an identifier char or dot.
      local pat = "[^%w_.]" .. g .. "%s*%."
      local line = 0
      for chunk in (" " .. src):gmatch("[^\n]*\n?") do
        line = line + 1
        if chunk:find(pat) then
          fail(("%s:%d uses '%s.' which the device sandbox does not provide")
               :format(name, line, g))
        end
      end
    end

    for _, fn in ipairs(FORBIDDEN_FN) do
      if (" " .. src):find("[^%w_.]" .. fn .. "%s*%(") then
        fail(("%s uses '%s()' which is unavailable on device"):format(name, fn))
      end
    end

    if failures == 0 then
      print("  ok   " .. name)
    end
  end
end

print("")
if failures == 0 then
  print("all files conform to the device sandbox")
  os.exit(0)
else
  print(failures .. " violation(s)")
  os.exit(1)
end
