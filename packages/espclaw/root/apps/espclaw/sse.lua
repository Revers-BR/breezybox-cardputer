-- Incremental Server-Sent Events parser.
--
-- Chunks arrive from breezy.https on_chunk at arbitrary byte boundaries, so a
-- line -- or an entire event -- can be split across calls. Only the trailing
-- partial line is retained between feeds, which is what keeps peak memory
-- independent of response length.
--
-- Pure Lua with no breezy dependency, so it is testable on the host:
--   lua tests/test_sse.lua

local M = {}
M.__index = M

-- on_event(event_name, data_string). event_name may be nil if the stream sent
-- only `data:` lines.
function M.new(on_event)
  return setmetatable({
    tail = "",
    ev = nil,
    data = {},
    on_event = on_event,
  }, M)
end

function M:_dispatch()
  if #self.data == 0 then
    self.ev = nil
    return
  end
  -- Multiple data: lines in one event are joined with newlines, per the spec.
  local payload = table.concat(self.data, "\n")
  local ev = self.ev
  self.data = {}
  self.ev = nil
  self.on_event(ev, payload)
end

function M:_line(line)
  if line == "" then
    self:_dispatch()
    return
  end
  -- ":" in column 1 is a comment/keep-alive.
  if line:sub(1, 1) == ":" then
    return
  end
  local field, value = line:match("^([^:]*):?%s?(.*)$")
  if not field then
    return
  end
  if field == "event" then
    self.ev = value
  elseif field == "data" then
    self.data[#self.data + 1] = value
  end
  -- id/retry are ignored; we do not resume streams.
end

function M:feed(chunk)
  if not chunk or chunk == "" then
    return
  end
  local s = self.tail .. chunk
  local pos = 1
  while true do
    local i = s:find("\n", pos, true)
    if not i then
      break
    end
    local line = s:sub(pos, i - 1)
    if line:sub(-1) == "\r" then
      line = line:sub(1, -2)
    end
    self:_line(line)
    pos = i + 1
  end
  self.tail = s:sub(pos)
end

-- Flush anything the server left without a trailing blank line.
function M:finish()
  if self.tail ~= "" then
    local line = self.tail
    if line:sub(-1) == "\r" then
      line = line:sub(1, -2)
    end
    self.tail = ""
    self:_line(line)
  end
  self:_dispatch()
end

return M
