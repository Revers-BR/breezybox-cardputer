-- Host tests for the SSE parser. Run: lua tests/test_sse.lua
-- The important property is that parsing is independent of how the byte
-- stream is chopped up, so the main test replays each fixture at every
-- possible split point.

package.path = "packages/espclaw/root/apps/espclaw/?.lua;" .. package.path
local sse = require("sse")

local failures = 0
local function check(name, ok, detail)
  if ok then
    print("  ok   " .. name)
  else
    failures = failures + 1
    print("  FAIL " .. name .. (detail and ("  -- " .. detail) or ""))
  end
end

local function collect(stream, chunk_size)
  local events = {}
  local p = sse.new(function(ev, data)
    events[#events + 1] = { ev = ev, data = data }
  end)
  if chunk_size then
    for i = 1, #stream, chunk_size do
      p:feed(stream:sub(i, i + chunk_size - 1))
    end
  else
    p:feed(stream)
  end
  p:finish()
  return events
end

local function same(a, b)
  if #a ~= #b then return false, ("count %d vs %d"):format(#a, #b) end
  for i = 1, #a do
    if a[i].ev ~= b[i].ev then
      return false, ("event %d: %s vs %s"):format(i, tostring(a[i].ev), tostring(b[i].ev))
    end
    if a[i].data ~= b[i].data then
      return false, ("data %d: %q vs %q"):format(i, a[i].data, b[i].data)
    end
  end
  return true
end

-- A realistic Anthropic streaming response.
local ANTHROPIC = table.concat({
  "event: message_start",
  'data: {"type":"message_start","message":{"id":"msg_01"}}',
  "",
  "event: content_block_delta",
  'data: {"type":"content_block_delta","delta":{"type":"text_delta","text":"Hello"}}',
  "",
  "event: content_block_delta",
  'data: {"type":"content_block_delta","delta":{"type":"text_delta","text":", world"}}',
  "",
  "event: message_stop",
  'data: {"type":"message_stop"}',
  "",
  "",
}, "\n")

print("basic parsing")
local base = collect(ANTHROPIC)
check("event count", #base == 4, "got " .. #base)
check("first event name", base[1].ev == "message_start", tostring(base[1].ev))
check("text delta present", base[2].data:find("Hello", 1, true) ~= nil)
check("last event", base[4].ev == "message_stop", tostring(base[4].ev))

print("")
print("split invariance (every chunk size 1..#stream)")
local bad = nil
for size = 1, #ANTHROPIC do
  local got = collect(ANTHROPIC, size)
  local ok, detail = same(base, got)
  if not ok then
    bad = ("chunk_size=%d: %s"):format(size, detail)
    break
  end
end
check("identical events at every split", bad == nil, bad)

print("")
print("edge cases")
check("CRLF line endings",
  (function()
    local e = collect("event: ping\r\ndata: {}\r\n\r\n")
    return #e == 1 and e[1].ev == "ping" and e[1].data == "{}"
  end)())

check("comment / keep-alive lines ignored",
  (function()
    local e = collect(": keep-alive\n\nevent: x\ndata: 1\n\n")
    return #e == 1 and e[1].ev == "x"
  end)())

check("data with no event name",
  (function()
    local e = collect("data: bare\n\n")
    return #e == 1 and e[1].ev == nil and e[1].data == "bare"
  end)())

check("multi-line data joined with newline",
  (function()
    local e = collect("data: a\ndata: b\n\n")
    return #e == 1 and e[1].data == "a\nb"
  end)())

check("colons inside the value survive",
  (function()
    local e = collect('data: {"url":"https://x.test/y"}\n\n')
    return #e == 1 and e[1].data == '{"url":"https://x.test/y"}'
  end)())

check("trailing event without blank line is flushed",
  (function()
    local e = collect("event: last\ndata: 9")
    return #e == 1 and e[1].ev == "last" and e[1].data == "9"
  end)())

check("empty feeds are harmless",
  (function()
    local e = collect("")
    return #e == 0
  end)())

print("")
print("openai stream shape (data-only events, [DONE] terminator)")

local OPENAI = table.concat({
  'data: {"choices":[{"delta":{"role":"assistant","content":""}}]}',
  "",
  'data: {"choices":[{"delta":{"content":"Hel"}}]}',
  "",
  'data: {"choices":[{"delta":{"content":"lo"}}]}',
  "",
  'data: {"choices":[{"delta":{},"finish_reason":"stop"}]}',
  "",
  "data: [DONE]",
  "",
  "",
}, "\n")

local oa = collect(OPENAI)
check("openai event count", #oa == 5, "got " .. #oa)
check("openai events are unnamed", oa[1].ev == nil, tostring(oa[1].ev))
check("openai [DONE] surfaces as data", oa[5].data == "[DONE]", oa[5].data)

local obad = nil
for size = 1, #OPENAI do
  local got = collect(OPENAI, size)
  local ok, detail = same(oa, got)
  if not ok then
    obad = ("chunk_size=%d: %s"):format(size, detail)
    break
  end
end
check("openai split invariance", obad == nil, obad)

-- Reassembling the text is the property that actually matters.
local text = {}
for _, e in ipairs(oa) do
  local c = e.data:match('"content":"([^"]*)"')
  if c then text[#text + 1] = c end
end
check("openai text reassembles", table.concat(text) == "Hello", table.concat(text))

print("")
if failures == 0 then
  print("all tests passed")
  os.exit(0)
else
  print(failures .. " test(s) FAILED")
  os.exit(1)
end
