-- Phase 0 check for breezy.https and breezy.json.
-- Run: lua /root/lua/https_test.lua
--
-- A 401 from the API is a PASS: it proves TLS, the cert bundle, the streamed
-- request body and the streamed response all work.

-- `breezy` is a preloaded module, not a global (see register_breezy_preload).
local breezy = require("breezy")

print("== breezy.json ==")
local enc = breezy.json.encode({ model = "claude-sonnet-5", max_tokens = 16 })
print("encode:", enc)
local dec = breezy.json.decode(enc)
print("decode: model=" .. tostring(dec.model) .. " max_tokens=" .. tostring(dec.max_tokens))

local bad, err = breezy.json.decode("{not json")
print("bad input ->", tostring(bad), tostring(err))

print("")
print("== breezy.https: inline body ==")
if not breezy.network.is_connected() then
  print("no network; run 'wifi connect <ssid> <pass>' first")
  return
end

local body = breezy.json.encode({
  model = "claude-sonnet-5",
  max_tokens = 16,
  stream = true,
  messages = { { role = "user", content = "hi" } },
})

local chunks, bytes = 0, 0
local first = nil
local status, total = breezy.https.request{
  url = "https://api.anthropic.com/v1/messages",
  method = "POST",
  headers = {
    ["content-type"] = "application/json",
    ["anthropic-version"] = "2023-06-01",
    ["x-api-key"] = "sk-ant-probe-invalid",
  },
  body = body,
  on_chunk = function(s)
    chunks = chunks + 1
    bytes = bytes + #s
    if not first then first = s end
  end,
}
print("status=" .. tostring(status) .. " total=" .. tostring(total) ..
      " chunks=" .. chunks .. " bytes=" .. bytes)
if first then print("first chunk: " .. first:sub(1, 120)) end

print("")
print("== breezy.https: body_file (the path that matters) ==")
local path = "/sd/claw_req_test.json"
local ok = breezy.write_file(path, body)
if not ok then
  path = "/root/claw_req_test.json"
  ok = breezy.write_file(path, body)
end
if not ok then
  print("could not write a request file; skipping body_file test")
  return
end
print("wrote " .. path .. " (" .. #body .. " bytes)")

local fchunks = 0
local fstatus, ftotal = breezy.https.request{
  url = "https://api.anthropic.com/v1/messages",
  method = "POST",
  headers = {
    ["content-type"] = "application/json",
    ["anthropic-version"] = "2023-06-01",
    ["x-api-key"] = "sk-ant-probe-invalid",
  },
  body_file = path,
  on_chunk = function(s) fchunks = fchunks + 1 end,
}
print("status=" .. tostring(fstatus) .. " total=" .. tostring(ftotal) ..
      " chunks=" .. fchunks)

print("")
print("== abort from on_chunk ==")
local seen = 0
local astatus = breezy.https.request{
  url = "https://api.anthropic.com/v1/messages",
  method = "POST",
  headers = {
    ["content-type"] = "application/json",
    ["anthropic-version"] = "2023-06-01",
    ["x-api-key"] = "sk-ant-probe-invalid",
  },
  body = body,
  on_chunk = function(s) seen = seen + 1; return false end,
}
print("status=" .. tostring(astatus) .. " chunks before abort=" .. seen)

print("")
print("done. Expect status 401 on all three requests.")
