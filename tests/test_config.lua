-- Host tests for config storage selection.
--
-- config.lua is the one module that does depend on `breezy`, so we install a
-- fake in package.preload before requiring it. What matters here is *which
-- file* gets used: SD when available (so `make flash` does not wipe the API
-- keys), internal flash otherwise.
--
-- Run: lua tests/test_config.lua

package.path = "packages/espclaw/root/apps/espclaw/?.lua;" .. package.path

local failures = 0
local function check(name, ok, detail)
  if ok then
    print("  ok   " .. name)
  else
    failures = failures + 1
    print("  FAIL " .. name .. (detail and ("  -- " .. detail) or ""))
  end
end

-- A fake device: an in-memory filesystem plus a settable SD state.
local function make_fake(opts)
  local files = {}
  for k, v in pairs(opts.files or {}) do files[k] = v end
  local dirs = {}
  for _, d in pairs(opts.dirs or {}) do dirs[d] = true end

  local fake = {
    _files = files,
    _dirs = dirs,
    exists = function(p) return files[p] ~= nil or dirs[p] == true end,
    read_file = function(p)
      if not files[p] then error("cannot open file: " .. p) end
      return files[p]
    end,
    write_file = function(p, data) files[p] = data; return true end,
    exec = function(cmd)
      local d = cmd:match("^mkdir%s+(.+)$")
      if d and opts.sd_writable ~= false then dirs[d] = true end
      return ""
    end,
    storage = { sd_mounted = function() return opts.sd_mounted == true end },
    -- Minimal JSON-backed config store, keyed by file path.
    config = {
      get = function(key, default, path)
        local store = files["::cfg::" .. tostring(path)]
        if store and store[key] ~= nil then return store[key] end
        return default
      end,
      set = function(key, value, path)
        local k = "::cfg::" .. tostring(path)
        files[k] = files[k] or {}
        files[k][key] = value
        return true
      end,
    },
  }
  return fake
end

local function load_config(fake)
  package.loaded["config"] = nil
  package.preload["breezy"] = function() return fake end
  package.loaded["breezy"] = nil
  return require("config"), fake
end

print("storage selection")

do -- SD present -> config goes to the card
  local cfg = load_config(make_fake{ sd_mounted = true })
  check("uses SD when mounted", cfg.path() == "/sd/claw/config.json", cfg.path())
end

do -- No SD -> internal flash
  local cfg = load_config(make_fake{ sd_mounted = false })
  check("falls back to flash when no SD", cfg.path() == "/root/.claw.json", cfg.path())
end

do -- SD mounted but directory cannot be created
  local cfg = load_config(make_fake{ sd_mounted = true, sd_writable = false })
  check("falls back when /sd/claw cannot be made",
        cfg.path() == "/root/.claw.json", cfg.path())
end

do -- Explicit override wins over an available card
  local fake = make_fake{ sd_mounted = true }
  fake.config.set("store", "flash", "/root/.claw.json")
  local cfg = load_config(fake)
  check("store=flash forces internal", cfg.path() == "/root/.claw.json", cfg.path())
end

print("")
print("migration")

do -- An existing flash config is copied to the card on first SD use
  local fake = make_fake{
    sd_mounted = true,
    files = { ["/root/.claw.json"] = '{"backend":"openai"}' },
  }
  local cfg = load_config(fake)
  local p = cfg.path()
  check("resolves to SD", p == "/sd/claw/config.json", p)
  check("flash config copied to SD",
        fake._files["/sd/claw/config.json"] == '{"backend":"openai"}',
        tostring(fake._files["/sd/claw/config.json"]))
end

do -- An existing SD config is never overwritten by the flash copy
  local fake = make_fake{
    sd_mounted = true,
    files = {
      ["/root/.claw.json"]      = '{"backend":"anthropic"}',
      ["/sd/claw/config.json"]  = '{"backend":"gemini"}',
    },
  }
  local cfg = load_config(fake)
  cfg.path()
  check("existing SD config preserved",
        fake._files["/sd/claw/config.json"] == '{"backend":"gemini"}',
        fake._files["/sd/claw/config.json"])
end

print("")
print("read/write routing")

do
  local fake = make_fake{ sd_mounted = true }
  local cfg = load_config(fake)
  cfg.set("openai.key", "sk-test-123")
  check("key written to the SD store",
        fake._files["::cfg::/sd/claw/config.json"]["openai.key"] == "sk-test-123")
  check("key not written to flash",
        fake._files["::cfg::/root/.claw.json"] == nil)
  check("reads back", cfg.get("openai.key") == "sk-test-123")
end

do -- `store` itself must live in flash, or it could not select the file
  local fake = make_fake{ sd_mounted = true }
  local cfg = load_config(fake)
  cfg.set("store", "flash")
  check("store persisted to flash",
        fake._files["::cfg::/root/.claw.json"]["store"] == "flash")
  check("store change takes effect immediately",
        cfg.path() == "/root/.claw.json", cfg.path())
end

print("")
print("secret masking")
do
  local cfg = load_config(make_fake{ sd_mounted = false })
  check("long key masked", cfg.mask("openai.key", "sk-proj-ABCDEFGH1234") == "sk-p...1234",
        cfg.mask("openai.key", "sk-proj-ABCDEFGH1234"))
  check("short key fully masked", cfg.mask("gemini.key", "abc") == "****")
  check("non-secret untouched", cfg.mask("model", "gpt-4o-mini") == "gpt-4o-mini")
end

print("")
if failures == 0 then
  print("all config tests passed")
  os.exit(0)
else
  print(failures .. " test(s) FAILED")
  os.exit(1)
end
