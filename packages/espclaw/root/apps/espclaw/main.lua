-- ESP-Claw for Cardputer - entry point.
--
-- Phase 2: config + a single-turn `claw ask`. No tools, no history yet.

local breezy = require("breezy")

-- Find our sibling modules wherever this script happens to live: installed
-- under /root/apps/espclaw, or a working copy dropped anywhere on the SD card.
-- arg[0] is the script path (see set_arg_table in cmd/lua.c).
local SCRIPT_DIR   -- set below; backends reference CA files relative to it

local function script_dir()
  local self = (type(arg) == "table") and arg[0] or nil
  if type(self) == "string" then
    local dir = self:match("^(.*)/[^/]*$")
    if dir and dir ~= "" then
      return dir
    end
  end
  return "/root/apps/espclaw"
end

SCRIPT_DIR = script_dir()

package.path = table.concat({
  SCRIPT_DIR .. "/?.lua",
  "/root/apps/espclaw/?.lua",
  "/sd/apps/espclaw/?.lua",
  package.path,
}, ";")

local config = require("config")
local sse    = require("sse")

-- The Lua sandbox here opens neither `io` nor `os`, so there is no os.exit().
-- Code paths that want to bail print a message and unwind with this sentinel,
-- which the dispatcher at the bottom swallows.
local ABORT = {}

local BACKENDS = {
  anthropic = "backend_anthropic",
  openai    = "backend_openai",
  gemini    = "backend_gemini",
}

local function load_backend()
  local name = config.get("backend")
  local mod = BACKENDS[name]
  if not mod then
    print("claw: unknown backend '" .. tostring(name) .. "'")
    print("      supported: anthropic, openai, gemini")
    error(ABORT)
  end
  return require(mod)
end

local VERSION = "0.1.0"

local function die(msg)
  print("claw: " .. msg)
  error(ABORT)
end

-- print() always appends a newline; breezy.write does not, which is what
-- streaming a reply token by token needs.
local emit
if type(breezy.write) == "function" then
  emit = breezy.write
else
  emit = function(text) print(text) end   -- older firmware: line per chunk
end

-- The console is backed by the LCD, and every write triggers a redraw. Pushing
-- each individual token straight through is painfully slow, so coalesce into
-- roughly a display line before flushing.
local OUT_FLUSH_AT = 64
local out_parts, out_len = {}, 0

local function out_write(text)
  out_parts[#out_parts + 1] = text
  out_len = out_len + #text
  if out_len >= OUT_FLUSH_AT or text:find("\n", 1, true) then
    emit(table.concat(out_parts))
    out_parts, out_len = {}, 0
  end
end

local function out_flush()
  if out_len > 0 then
    emit(table.concat(out_parts))
    out_parts, out_len = {}, 0
  end
end

-- ---------------------------------------------------------------- helpers --

-- breezybox's mkdir has no -p flag and there is no breezy.mkdir binding, so
-- build the path one segment at a time. Returns true if the directory exists
-- afterwards.
local function ensure_dir(path)
  if breezy.exists(path) then
    return true
  end
  local cur = ""
  for seg in path:gmatch("[^/]+") do
    cur = cur .. "/" .. seg
    if not breezy.exists(cur) then
      breezy.exec("mkdir " .. cur)
    end
  end
  return breezy.exists(path)
end

-- breezy.write_file raises on failure rather than returning false, so any
-- fallback has to go through pcall. Returns ok, err.
local function try_write(path, data)
  local ok, err = pcall(breezy.write_file, path, data)
  if ok then
    return true
  end
  return false, err
end

-- ------------------------------------------------------------------- ask ---

local function cmd_ask(args)
  local verbose = false
  local words = {}
  for _, a in ipairs(args) do
    if a == "-v" or a == "--verbose" then
      verbose = true
    else
      words[#words + 1] = a
    end
  end
  args = words

  local prompt = table.concat(args, " ")
  if prompt == "" then
    die('usage: claw ask "your question"')
  end

  local key, err = config.api_key()
  if not key then
    die(err)
  end

  if not breezy.network.is_connected() then
    die("no network. Run: wifi connect <ssid> <password>")
  end

  local backend = load_backend()
  local messages = { { role = "user", content = prompt } }
  local body = breezy.json.encode(backend.build_body(config, messages))

  -- Write the request to disk and stream it from there. Trivial at this size,
  -- but it is the path multi-turn conversations will use, so exercise it now.
  local tmp_dir = config.get("tmp_dir")
  local req_path = tmp_dir .. "/req.json"
  local wrote, werr = false, nil
  if ensure_dir(tmp_dir) then
    wrote, werr = try_write(req_path, body)
  end
  if not wrote then
    -- No SD card, or it is not writable. Internal flash still works.
    req_path = "/root/.claw_req.json"
    wrote, werr = try_write(req_path, body)
    if not wrote then
      die("cannot write request file (tried " .. tmp_dir ..
          " and /root): " .. tostring(werr))
    end
  end

  local http_status = nil
  local error_body = {}      -- collected only when the status is not 2xx
  local api_error = nil
  local got_text = false
  local n_chunks, n_events, n_bytes = 0, 0, 0
  local started = breezy.now_ms()

  local function heap_line(label)
    if not verbose or type(breezy.heap) ~= "function" then
      return
    end
    local free, minf, largest = breezy.heap()
    print(string.format("[heap %s: free %d, min %d, largest %d]",
                        label, free, minf, largest))
  end

  -- A TLS handshake needs a contiguous block, so `largest` matters as much as
  -- `free`; MBEDTLS_ERR_SSL_ALLOC_FAILED usually means fragmentation.
  collectgarbage("collect")
  heap_line("before")

  local parser = sse.new(function(evname, data)
    n_events = n_events + 1
    if data == "[DONE]" then
      return
    end
    local obj = breezy.json.decode(data)
    if not obj then
      return
    end
    local e = backend.extract_error(obj)
    if e then
      api_error = e
      return
    end
    local text = backend.extract_text(evname, obj)
    if text and text ~= "" then
      out_write(text)
      got_text = true
    end
  end)

  -- An explicit CA overrides the IDF bundle. Config wins over the backend's
  -- default so a user can point at their own PEM.
  local ca_file = config.get("ca_file")
  if ca_file == nil or ca_file == "" then
    ca_file = backend.ca_file and (SCRIPT_DIR .. "/" .. backend.ca_file) or nil
  end
  if ca_file and not breezy.exists(ca_file) then
    -- Always report this. Falling back from a pinned root to the IDF bundle
    -- changes what we trust, and for Gemini the bundle simply cannot verify
    -- the chain -- so a silent downgrade looks like an unrelated TLS error.
    print("claw: warning - CA file missing, using cert bundle instead")
    print("      " .. ca_file)
    ca_file = nil
  end

  local status, total = breezy.https.request{
    url        = backend.endpoint(config),
    method     = "POST",
    headers    = backend.headers(config, key),
    body_file  = req_path,
    ca_file    = ca_file,
    timeout_ms = config.get("timeout_ms"),
    on_status  = function(code)
      http_status = code
    end,
    on_chunk   = function(chunk)
      -- An HTTP error body is plain JSON, not SSE, so parsing it as a stream
      -- would silently yield nothing and lose the API's explanation. Buffer it
      -- instead -- bounded, because we only do this on the error path.
      if http_status and (http_status < 200 or http_status >= 300) then
        if #error_body < 8 then
          error_body[#error_body + 1] = chunk
        end
        return
      end
      n_chunks = n_chunks + 1
      n_bytes = n_bytes + #chunk
      parser:feed(chunk)
    end,
  }
  parser:finish()
  out_flush()

  if got_text then
    print("")
  end

  heap_line("after")

  if verbose then
    print(string.format(
      "[%d chunks, %d events, %d bytes, %d ms, lua %d KB]",
      n_chunks, n_events, n_bytes,
      breezy.now_ms() - started,
      math.floor(collectgarbage("count"))))
  end

  if status == nil then
    die("request failed: " .. tostring(total))
  end

  if status < 200 or status >= 300 then
    -- Prefer the API's own message over a bare status code.
    local raw = table.concat(error_body)
    local obj = raw ~= "" and breezy.json.decode(raw) or nil
    local msg = obj and backend.extract_error(obj) or nil
    if msg then
      die("HTTP " .. status .. " - " .. msg)
    elseif raw ~= "" then
      die("HTTP " .. status .. " - " .. raw:sub(1, 200))
    else
      die("HTTP " .. status)
    end
  end

  if api_error then
    die(api_error)
  end
  if not got_text then
    print("(no text in response)")
  end
end

-- ---------------------------------------------------------------- config ---

local function cmd_config(args)
  local sub = args[1]

  if sub == nil or sub == "show" then
    local where = config.path()
    print("config file: " .. where ..
          (where:sub(1, 4) == "/sd/" and "  (survives reflash)" or "  (wiped by make flash)"))
    for _, row in ipairs(config.list()) do
      local v = row.value
      if v == nil or v == "" then
        v = "(unset)"
      end
      print(string.format("  %-16s %s", row.key, tostring(v)))
    end
    return
  end

  if sub == "get" then
    local k = args[2] or die("usage: claw config get <key>")
    local v = config.get(k)
    print(v == nil and "(unset)" or tostring(config.mask(k, v)))
    return
  end

  if sub == "set" then
    local k = args[2]
    if not k then die("usage: claw config set <key> <value>") end
    local v = table.concat(args, " ", 3)
    if v == "" then die("usage: claw config set <key> <value>") end
    -- Numeric settings should round-trip as numbers.
    local n = tonumber(v)
    if n and (k == "max_tokens" or k == "timeout_ms") then
      v = math.floor(n)
    end
    if config.set(k, v) then
      print(k .. " = " .. tostring(config.mask(k, v)))
    else
      die("could not write " .. config.path())
    end
    return
  end

  die("usage: claw config <show|get|set>")
end

-- ----------------------------------------------------------------- stats ---

local function cmd_stats()
  print("claw " .. VERSION)
  if type(breezy.build) == "function" then
    local b = breezy.build()
    print("  firmware   " .. b.profile ..
          ", bluetooth " .. (b.bluetooth and "on" or "off"))
    print("  built      " .. b.built)
  end
  print("  backend    " .. tostring(config.get("backend")))
  print("  model      " .. tostring(config.get("model")))
  local ok, be = pcall(load_backend)
  if ok then
    print("  endpoint   " .. be.endpoint(config))
  end
  print("  network    " .. (breezy.network.is_connected() and "connected" or "offline"))
  print("  sd card    " .. (breezy.storage.sd_mounted() and "mounted" or "absent"))
  print("  config     " .. config.path())
  print("  script     " .. SCRIPT_DIR ..
        (SCRIPT_DIR:sub(1, 4) == "/sd/" and "  (SD working copy)" or "  (flashed)"))
  print("  lua memory " .. math.floor(collectgarbage("count")) .. " KB")
  if type(breezy.heap) == "function" then
    local free, minf, largest = breezy.heap()
    print(string.format("  heap       free %d, min %d, largest %d", free, minf, largest))
  end
end

-- ------------------------------------------------------------------ main ---

local function usage()
  print("claw " .. VERSION .. " - on-device AI agent")
  print("")
  print("  claw ask [-v] \"question\"     one-shot question (-v: stream stats)")
  print("  claw config show             list settings")
  print("  claw config get <key>")
  print("  claw config set <key> <val>")
  print("  claw stats                   status and memory")
  print("")
  print("Setup (OpenAI):")
  print("  claw config set backend openai")
  print("  claw config set openai.key sk-...")
  print("")
  print("Setup (Anthropic):")
  print("  claw config set backend anthropic")
  print("  claw config set anthropic.key sk-ant-...")
  print("")
  print("Setup (Gemini):")
  print("  claw config set backend gemini")
  print("  claw config set gemini.key AIza...")
  print("")
  print("Any OpenAI-compatible endpoint works via base_url:")
  print("  claw config set base_url http://192.168.1.10:11434/v1/chat/completions")
end

-- Script arguments arrive as varargs; fall back to the global `arg` table.
local argv = { ... }
if #argv == 0 and type(arg) == "table" then
  for i = 1, #arg do
    argv[i] = arg[i]
  end
end

local cmd = argv[1]
local rest = {}
for i = 2, #argv do
  rest[#rest + 1] = argv[i]
end

local function dispatch()
  if cmd == nil or cmd == "help" or cmd == "-h" or cmd == "--help" then
    usage()
  elseif cmd == "ask" then
    cmd_ask(rest)
  elseif cmd == "config" then
    cmd_config(rest)
  elseif cmd == "stats" then
    cmd_stats()
  else
    print("claw: unknown command '" .. tostring(cmd) .. "'")
    usage()
  end
end

local ok, err = pcall(dispatch)
if not ok and err ~= ABORT then
  error(err, 0)   -- a genuine fault: let the interpreter report it
end
