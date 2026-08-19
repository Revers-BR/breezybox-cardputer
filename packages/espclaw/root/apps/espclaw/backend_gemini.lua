-- Google Gemini (generativelanguage) backend.
--
-- Differs from the other two in several ways that matter:
--   * the model is part of the URL path, not the body
--   * auth is the x-goog-api-key header
--   * messages are `contents` with `parts`, not `messages` with `content`
--   * the assistant role is called "model"
--   * ?alt=sse is required, otherwise the response is a JSON array, not a stream
--
-- Streaming shape is otherwise OpenAI-like: unnamed `data:` events. There is no
-- [DONE] terminator; the stream simply ends.
--
-- Gemini also exposes an OpenAI-compatible endpoint. If you would rather use
-- that, keep backend=openai and set:
--   base_url https://generativelanguage.googleapis.com/v1beta/openai/chat/completions

local M = {}

M.name = "gemini"
M.default_model = "gemini-2.5-flash"

-- The IDF certificate bundle cannot verify Google's cross-signed chain, so pin
-- the actual root instead. Resolved relative to the package by main.lua.
-- See ca/README.md.
M.ca_file = "ca/gts_root_r1.pem"

function M.endpoint(cfg)
  local base = cfg.get("base_url")
  if base ~= nil and base ~= "" then
    return base
  end
  local model = cfg.get("model")
  return "https://generativelanguage.googleapis.com/v1beta/models/"
      .. model .. ":streamGenerateContent?alt=sse"
end

function M.headers(cfg, key)
  return {
    ["content-type"]   = "application/json",
    ["x-goog-api-key"] = key,
  }
end

-- {role="user"|"assistant"|"system", content="..."} -> Gemini contents/parts.
-- System turns are hoisted into systemInstruction, which is where Gemini wants
-- them.
function M.build_body(cfg, messages)
  local contents = {}
  local system_text = nil

  for _, m in ipairs(messages) do
    if m.role == "system" then
      system_text = system_text and (system_text .. "\n" .. m.content) or m.content
    else
      contents[#contents + 1] = {
        role  = (m.role == "assistant") and "model" or "user",
        parts = { { text = m.content } },
      }
    end
  end

  local body = {
    contents = contents,
    generationConfig = {
      maxOutputTokens = cfg.get("max_tokens"),
    },
  }
  if system_text then
    body.systemInstruction = { parts = { { text = system_text } } }
  end
  return body
end

function M.extract_text(_, obj)
  local cand = obj.candidates and obj.candidates[1]
  if not cand then
    return nil
  end
  local content = cand.content
  if not content or not content.parts then
    return nil
  end
  -- A chunk can carry more than one part.
  local out = {}
  for _, part in ipairs(content.parts) do
    if part.text then
      out[#out + 1] = part.text
    end
  end
  if #out == 0 then
    return nil
  end
  return table.concat(out)
end

function M.extract_error(obj)
  local e = obj.error
  if not e then
    return nil
  end
  if type(e) ~= "table" then
    return tostring(e)
  end
  local status = e.status or e.code or "error"
  return tostring(status) .. ": " .. (e.message or "unknown")
end

return M
