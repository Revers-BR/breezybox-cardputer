-- Anthropic Messages API backend.
--
-- Streaming shape: named SSE events (`event: content_block_delta`) whose data
-- is a JSON object. Text arrives as delta.text on text_delta blocks.

local M = {}

M.name = "anthropic"
M.default_model = "claude-sonnet-5"

function M.endpoint(cfg)
  local base = cfg.get("base_url")
  if base ~= nil and base ~= "" then
    return base
  end
  return "https://api.anthropic.com/v1/messages"
end

function M.headers(cfg, key)
  return {
    ["content-type"]      = "application/json",
    ["anthropic-version"] = "2023-06-01",
    ["x-api-key"]         = key,
  }
end

function M.build_body(cfg, messages)
  return {
    model      = cfg.get("model"),
    max_tokens = cfg.get("max_tokens"),
    stream     = true,
    messages   = messages,
  }
end

-- Text to print, or nil.
function M.extract_text(evname, obj)
  if evname == "content_block_delta" or obj.type == "content_block_delta" then
    local d = obj.delta
    if d and d.type == "text_delta" then
      return d.text
    end
  end
  return nil
end

-- An error message carried inside the stream, or nil.
function M.extract_error(obj)
  if obj.type == "error" or obj.error then
    local e = obj.error or {}
    return (e.type or "error") .. ": " .. (e.message or "unknown")
  end
  return nil
end

return M
