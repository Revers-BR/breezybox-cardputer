-- Configuration for ESP-Claw.
--
-- Everything lives in a single JSON file on internal flash, not on the SD
-- card: the card is removable, and the API key should not walk off with it.
-- breezy.config already gives us JSON-backed get/set with an explicit path.

local breezy = require("breezy")

local M = {}

-- Where the config lives.
--
-- SD is preferred because `make flash` rewrites the whole LittleFS partition,
-- which wipes /root and takes the API keys with it. The SD card survives
-- reflashing, so keys only have to be entered once.
--
-- The trade-off: the card is removable, so anyone holding it holds the keys.
-- If that matters for your threat model, force internal flash with:
--   claw config set store flash
local SD_DIR     = "/sd/claw"
local SD_PATH    = SD_DIR .. "/config.json"
local FLASH_PATH = "/root/.claw.json"

local resolved_path = nil

local function sd_usable()
  if not breezy.storage.sd_mounted() then
    return false
  end
  if breezy.exists(SD_DIR) then
    return true
  end
  breezy.exec("mkdir " .. SD_DIR)
  return breezy.exists(SD_DIR)
end

-- Copy an existing flash config onto the card the first time we use it, so
-- switching storage does not silently lose settings.
local function migrate_to_sd()
  if breezy.exists(SD_PATH) or not breezy.exists(FLASH_PATH) then
    return
  end
  local ok, data = pcall(breezy.read_file, FLASH_PATH)
  if ok and data and data ~= "" then
    pcall(breezy.write_file, SD_PATH, data)
  end
end

function M.path()
  if resolved_path then
    return resolved_path
  end
  -- `store` is read from flash first: it decides where everything else lives,
  -- so it cannot itself live in the file it selects.
  local forced = breezy.config.get("store", nil, FLASH_PATH)
  if forced == "flash" then
    resolved_path = FLASH_PATH
  elseif sd_usable() then
    migrate_to_sd()
    resolved_path = SD_PATH
  else
    resolved_path = FLASH_PATH
  end
  return resolved_path
end

-- Kept for compatibility with older callers; prefer M.path().
M.PATH = FLASH_PATH

-- Model default depends on the active backend, so it is resolved in M.get
-- rather than sitting in this table.
local MODEL_DEFAULT = {
  anthropic = "claude-sonnet-5",
  openai    = "gpt-4o-mini",
  gemini    = "gemini-2.5-flash",
}

local DEFAULTS = {
  backend     = "anthropic",
  model       = nil,
  max_tokens  = 2048,
  base_url    = "",           -- set for openai-compatible / local endpoints
  ca_file     = "",           -- pin a root CA; blank uses the IDF bundle
  timeout_ms  = 60000,
  session_dir = "/sd/claw/sessions",
  tmp_dir     = "/sd/claw/tmp",
}

-- Keys that must never be echoed back in full.
local SECRET = {
  ["anthropic.key"] = true,
  ["openai.key"]    = true,
  ["gemini.key"]    = true,
}

function M.is_secret(key)
  return SECRET[key] == true
end

function M.get(key)
  if key == "store" then
    return breezy.config.get("store", nil, FLASH_PATH) or "auto"
  end
  local v = breezy.config.get(key, nil, M.path())
  if v ~= nil and v ~= "" then
    return v
  end
  if key == "model" then
    local backend = breezy.config.get("backend", nil, M.path()) or DEFAULTS.backend
    return MODEL_DEFAULT[backend] or MODEL_DEFAULT.anthropic
  end
  return DEFAULTS[key]
end

function M.model_defaults()
  return MODEL_DEFAULT
end

function M.set(key, value)
  -- `store` always lives in flash, and changing it invalidates the cache.
  if key == "store" then
    resolved_path = nil
    return breezy.config.set("store", value, FLASH_PATH)
  end
  return breezy.config.set(key, value, M.path())
end

function M.defaults()
  return DEFAULTS
end

-- The API key for the active backend, or nil plus a hint.
function M.api_key()
  local backend = M.get("backend")
  local key = M.get(backend .. ".key")
  if key == nil or key == "" then
    return nil, ("no API key set. Run: claw config set %s.key <key>"):format(backend)
  end
  return key
end

function M.mask(key, value)
  if not M.is_secret(key) or value == nil or value == "" then
    return value
  end
  local s = tostring(value)
  if #s <= 8 then
    return "****"
  end
  return s:sub(1, 4) .. "..." .. s:sub(-4)
end

-- Ordered list of {key, value} for `claw config show`, secrets masked.
function M.list()
  local keys = { "model" }
  for k in pairs(DEFAULTS) do
    if k ~= "model" then
      keys[#keys + 1] = k
    end
  end
  table.sort(keys)
  keys[#keys + 1] = "anthropic.key"
  keys[#keys + 1] = "openai.key"
  keys[#keys + 1] = "gemini.key"
  keys[#keys + 1] = "store"

  local out = {}
  for _, k in ipairs(keys) do
    local v = M.get(k)
    out[#out + 1] = { key = k, value = M.mask(k, v), set = (v ~= nil and v ~= "") }
  end
  return out
end

return M
