-- Host tests for the backend modules.
--
-- The backends deliberately have no `breezy` dependency -- they take a config
-- table and return plain Lua -- so the request shape and the streaming-response
-- parsing can both be checked without a device.
--
-- Run: lua tests/test_backends.lua

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

-- Minimal stand-in for config.lua.
local function fake_cfg(t)
  return {
    get = function(k) return t[k] end,
  }
end

local MESSAGES = {
  { role = "system",    content = "be terse" },
  { role = "user",      content = "hi" },
  { role = "assistant", content = "hello" },
  { role = "user",      content = "again" },
}

-- ------------------------------------------------------------- anthropic ---
print("anthropic")
local anth = require("backend_anthropic")
local acfg = fake_cfg{ model = "claude-sonnet-5", max_tokens = 2048, base_url = "" }

check("endpoint", anth.endpoint(acfg) == "https://api.anthropic.com/v1/messages",
      anth.endpoint(acfg))
check("auth header is x-api-key", anth.headers(acfg, "K")["x-api-key"] == "K")
check("version header pinned",
      anth.headers(acfg, "K")["anthropic-version"] == "2023-06-01")

local abody = anth.build_body(acfg, MESSAGES)
check("streams", abody.stream == true)
check("max_tokens passed", abody.max_tokens == 2048)
check("messages passed through", #abody.messages == 4)

check("extracts text_delta",
      anth.extract_text("content_block_delta",
        { type = "content_block_delta", delta = { type = "text_delta", text = "Hi" } }) == "Hi")
check("ignores non-text deltas",
      anth.extract_text("content_block_delta",
        { type = "content_block_delta", delta = { type = "input_json_delta" } }) == nil)
check("extracts error",
      anth.extract_error({ type = "error", error = { type = "authentication_error", message = "bad key" } })
        == "authentication_error: bad key")
check("no false error", anth.extract_error({ type = "message_stop" }) == nil)

-- ---------------------------------------------------------------- openai ---
print("")
print("openai")
local oai = require("backend_openai")
local ocfg = fake_cfg{ model = "gpt-4o-mini", max_tokens = 2048, base_url = "" }

check("endpoint", oai.endpoint(ocfg) == "https://api.openai.com/v1/chat/completions",
      oai.endpoint(ocfg))
check("bearer auth", oai.headers(ocfg, "K")["authorization"] == "Bearer K")

local obody = oai.build_body(ocfg, MESSAGES)
check("uses max_tokens for gpt-4o", obody.max_tokens == 2048 and obody.max_completion_tokens == nil)

local o5 = oai.build_body(fake_cfg{ model = "gpt-5", max_tokens = 99 }, MESSAGES)
check("gpt-5 uses max_completion_tokens",
      o5.max_completion_tokens == 99 and o5.max_tokens == nil)
local oo1 = oai.build_body(fake_cfg{ model = "o3-mini", max_tokens = 99 }, MESSAGES)
check("o3 uses max_completion_tokens",
      oo1.max_completion_tokens == 99 and oo1.max_tokens == nil)

check("extracts delta content",
      oai.extract_text(nil, { choices = { { delta = { content = "Hi" } } } }) == "Hi")
check("handles empty delta",
      oai.extract_text(nil, { choices = { { delta = {} } } }) == nil)
check("extracts error",
      oai.extract_error({ error = { type = "invalid_request_error", message = "nope" } })
        == "invalid_request_error: nope")

check("base_url overrides endpoint",
      oai.endpoint(fake_cfg{ base_url = "http://box:11434/v1/chat/completions" })
        == "http://box:11434/v1/chat/completions")

-- ---------------------------------------------------------------- gemini ---
print("")
print("gemini")
local gem = require("backend_gemini")
local gcfg = fake_cfg{ model = "gemini-2.5-flash", max_tokens = 2048, base_url = "" }

check("model is in the URL path and sse is requested",
      gem.endpoint(gcfg) ==
      "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:streamGenerateContent?alt=sse",
      gem.endpoint(gcfg))
check("goog api key header", gem.headers(gcfg, "K")["x-goog-api-key"] == "K")
check("no bearer header", gem.headers(gcfg, "K")["authorization"] == nil)

local gbody = gem.build_body(gcfg, MESSAGES)
check("system hoisted to systemInstruction",
      gbody.systemInstruction ~= nil and
      gbody.systemInstruction.parts[1].text == "be terse")
check("system turn excluded from contents", #gbody.contents == 3,
      "got " .. #gbody.contents)
check("assistant role renamed to model", gbody.contents[2].role == "model",
      tostring(gbody.contents[2].role))
check("user role preserved", gbody.contents[1].role == "user")
check("content becomes parts[].text", gbody.contents[1].parts[1].text == "hi")
check("maxOutputTokens set", gbody.generationConfig.maxOutputTokens == 2048)
check("no stream flag in body (it is in the URL)", gbody.stream == nil)

check("extracts candidate text",
      gem.extract_text(nil,
        { candidates = { { content = { parts = { { text = "Hi" } }, role = "model" } } } }) == "Hi")
check("joins multiple parts",
      gem.extract_text(nil,
        { candidates = { { content = { parts = { { text = "a" }, { text = "b" } } } } } }) == "ab")
check("handles candidate with no content",
      gem.extract_text(nil, { candidates = { { finishReason = "STOP" } } }) == nil)
check("extracts error",
      gem.extract_error({ error = { code = 400, status = "INVALID_ARGUMENT", message = "bad" } })
        == "INVALID_ARGUMENT: bad")
check("no false error", gem.extract_error({ candidates = {} }) == nil)

-- ------------------------------------------------------------- interface ---
print("")
print("shared interface")
for _, m in ipairs({ anth, oai, gem }) do
  local name = m.name
  for _, fn in ipairs({ "endpoint", "headers", "build_body", "extract_text", "extract_error" }) do
    check(name .. "." .. fn, type(m[fn]) == "function")
  end
  check(name .. ".default_model", type(m.default_model) == "string")
end

print("")
if failures == 0 then
  print("all backend tests passed")
  os.exit(0)
else
  print(failures .. " test(s) FAILED")
  os.exit(1)
end
