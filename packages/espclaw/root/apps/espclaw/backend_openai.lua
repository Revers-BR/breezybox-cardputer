-- OpenAI-compatible /v1/chat/completions backend.
--
-- Also covers anything speaking the same protocol -- OpenRouter, Ollama,
-- llama.cpp, LM Studio -- by setting base_url.
--
-- Streaming shape differs from Anthropic: unnamed events (`data:` only, no
-- `event:` line), terminated by a literal `data: [DONE]`. Text arrives as
-- choices[1].delta.content.

local M = {}

M.name = "openai"
M.default_model = "gpt-4o-mini"

function M.endpoint(cfg)
  local base = cfg.get("base_url")
  if base ~= nil and base ~= "" then
    return base
  end
  return "https://api.openai.com/v1/chat/completions"
end

function M.headers(cfg, key)
  return {
    ["content-type"]  = "application/json",
    ["authorization"] = "Bearer " .. key,
  }
end

-- Reasoning-era models reject `max_tokens` and require `max_completion_tokens`.
-- Guessing from the model name avoids a confusing 400 on a first run.
local function wants_max_completion_tokens(model)
  if type(model) ~= "string" then
    return false
  end
  local m = model:lower()
  return m:match("^o%d") ~= nil        -- o1, o3, o4...
      or m:match("^gpt%-5") ~= nil
end

function M.build_body(cfg, messages)
  local body = {
    model    = cfg.get("model"),
    stream   = true,
    messages = messages,
  }
  local limit = cfg.get("max_tokens")
  if wants_max_completion_tokens(body.model) then
    body.max_completion_tokens = limit
  else
    body.max_tokens = limit
  end
  return body
end

function M.extract_text(_, obj)
  local choice = obj.choices and obj.choices[1]
  if choice and choice.delta and choice.delta.content then
    return choice.delta.content
  end
  return nil
end

function M.extract_error(obj)
  if obj.error then
    local e = obj.error
    if type(e) == "table" then
      return (e.type or "error") .. ": " .. (e.message or "unknown")
    end
    return tostring(e)
  end
  return nil
end

return M
