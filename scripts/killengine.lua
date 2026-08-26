local ke = {}

local function is_array(t)
  if type(t) ~= "table" then return false end
  local max = 0
  local count = 0
  for k, _ in pairs(t) do
    if type(k) ~= "number" or k < 1 or k % 1 ~= 0 then return false end
    if k > max then max = k end
    count = count + 1
  end
  return max == count
end

local function json_escape(s)
  s = tostring(s)
  s = s:gsub("\\", "\\\\")
       :gsub('"', '\\"')
       :gsub("\b", "\\b")
       :gsub("\f", "\\f")
       :gsub("\n", "\\n")
       :gsub("\r", "\\r")
       :gsub("\t", "\\t")
  return '"' .. s .. '"'
end

local function json_encode(value)
  local value_type = type(value)
  if value == nil then
    return "null"
  elseif value_type == "boolean" then
    return value and "true" or "false"
  elseif value_type == "number" then
    return tostring(value)
  elseif value_type == "string" then
    return json_escape(value)
  elseif value_type == "table" then
    local out = {}
    if is_array(value) then
      for i = 1, #value do
        out[#out + 1] = json_encode(value[i])
      end
      return "[" .. table.concat(out, ",") .. "]"
    end
    for k, v in pairs(value) do
      out[#out + 1] = json_escape(k) .. ":" .. json_encode(v)
    end
    return "{" .. table.concat(out, ",") .. "}"
  end
  error("JSON unsupported type: " .. value_type)
end

-- Parseur JSON minimal (objets, tableaux, strings, nombres, bool, null),
-- suffisant pour decoder les reponses JSON-RPC du pipe d'automatisation sans
-- dependance externe non packagee. Pas un parseur JSON generique complet
-- (pas de \uXXXX), mais couvre tout ce que le pipe/ApplicationController
-- produisent reellement (voir automation_pipe_server.cpp).
local function json_decode(text)
  local pos = 1
  local len = #text

  local function skip_ws()
    while pos <= len do
      local c = text:sub(pos, pos)
      if c == " " or c == "\t" or c == "\n" or c == "\r" then
        pos = pos + 1
      else
        break
      end
    end
  end

  local parse_value

  local function parse_string()
    pos = pos + 1 -- skip opening quote
    local out = {}
    while pos <= len do
      local c = text:sub(pos, pos)
      if c == '"' then
        pos = pos + 1
        return table.concat(out)
      elseif c == "\\" then
        local nextc = text:sub(pos + 1, pos + 1)
        if nextc == "n" then out[#out + 1] = "\n"
        elseif nextc == "t" then out[#out + 1] = "\t"
        elseif nextc == "r" then out[#out + 1] = "\r"
        elseif nextc == "b" then out[#out + 1] = "\b"
        elseif nextc == "f" then out[#out + 1] = "\f"
        elseif nextc == '"' then out[#out + 1] = '"'
        elseif nextc == "\\" then out[#out + 1] = "\\"
        elseif nextc == "/" then out[#out + 1] = "/"
        else out[#out + 1] = nextc end
        pos = pos + 2
      else
        out[#out + 1] = c
        pos = pos + 1
      end
    end
    error("JSON decode: unterminated string")
  end

  local function parse_number()
    local start = pos
    while pos <= len and text:sub(pos, pos):match("[%d%+%-%.eE]") do
      pos = pos + 1
    end
    return tonumber(text:sub(start, pos - 1))
  end

  local function parse_array()
    pos = pos + 1 -- skip [
    local out = {}
    skip_ws()
    if text:sub(pos, pos) == "]" then
      pos = pos + 1
      return out
    end
    while true do
      skip_ws()
      out[#out + 1] = parse_value()
      skip_ws()
      local c = text:sub(pos, pos)
      if c == "," then
        pos = pos + 1
      elseif c == "]" then
        pos = pos + 1
        break
      else
        error("JSON decode: expected ',' or ']' at position " .. pos)
      end
    end
    return out
  end

  local function parse_object()
    pos = pos + 1 -- skip {
    local out = {}
    skip_ws()
    if text:sub(pos, pos) == "}" then
      pos = pos + 1
      return out
    end
    while true do
      skip_ws()
      if text:sub(pos, pos) ~= '"' then
        error("JSON decode: expected string key at position " .. pos)
      end
      local key = parse_string()
      skip_ws()
      if text:sub(pos, pos) ~= ":" then
        error("JSON decode: expected ':' at position " .. pos)
      end
      pos = pos + 1
      skip_ws()
      out[key] = parse_value()
      skip_ws()
      local c = text:sub(pos, pos)
      if c == "," then
        pos = pos + 1
      elseif c == "}" then
        pos = pos + 1
        break
      else
        error("JSON decode: expected ',' or '}' at position " .. pos)
      end
    end
    return out
  end

  parse_value = function()
    skip_ws()
    local c = text:sub(pos, pos)
    if c == '"' then
      return parse_string()
    elseif c == "{" then
      return parse_object()
    elseif c == "[" then
      return parse_array()
    elseif c == "t" and text:sub(pos, pos + 3) == "true" then
      pos = pos + 4
      return true
    elseif c == "f" and text:sub(pos, pos + 4) == "false" then
      pos = pos + 5
      return false
    elseif c == "n" and text:sub(pos, pos + 3) == "null" then
      pos = pos + 4
      return nil
    elseif c:match("[%d%+%-]") then
      return parse_number()
    end
    error("JSON decode: unexpected character '" .. c .. "' at position " .. pos)
  end

  skip_ws()
  local ok, value = pcall(parse_value)
  if not ok then
    return nil, value
  end
  return value
end

local function quote_arg(value)
  value = tostring(value)
  value = value:gsub('"', '\\"')
  return '"' .. value .. '"'
end

local function read_all(pipe)
  local chunks = {}
  while true do
    local chunk = pipe:read("*l")
    if chunk == nil then break end
    chunks[#chunks + 1] = chunk
  end
  return table.concat(chunks, "\n")
end

local function repo_root()
  return os.getenv("KILLENGINE_ROOT") or "."
end

local function pipe_name()
  return os.getenv("KILLENGINE_AUTOMATION_PIPE_NAME") or "KillEngineAutomationPipe"
end

function ke.params(...)
  return { ... }
end

function ke.json(value)
  return json_encode(value)
end

-- Decode un texte JSON en table/valeur Lua. Retourne nil + message d'erreur
-- si le texte n'est pas du JSON valide (parseur maison, voir json_decode
-- plus haut : couvre objets/tableaux/strings/nombres/bool/null).
function ke.decode_json(text)
  return json_decode(text)
end

-- ATTENTION (verifie en direct le 26/08/2026) : si ce script Lua est lui-meme
-- lance par KillEngine via executeLuaScript (le SYNCHRONE), un appel ke.call
-- ici bloque indefiniment jusqu'au timeout du script -- executeLuaScript
-- bloque le thread qui doit justement servir cette connexion pipe imbriquee.
-- Lance via executeLuaScriptAsync (thread separe, retourne started:true tout
-- de suite), ke.call fonctionne normalement (~1s de latence par appel, cout
-- du powershell.exe imbrique a chaque fois). Donc : tout script qui utilise
-- ke.call/ke.apply_code_patch/etc. doit etre lance en async, jamais en sync.
function ke.call(method, params, options)
  options = options or {}
  params = params or {}
  local root = options.root or repo_root()
  local script = root .. "\\scripts\\automation-pipe-call.ps1"
  local temp = os.tmpname()
  local file = assert(io.open(temp, "wb"))
  file:write(json_encode(params))
  file:close()

  local command = table.concat({
    "powershell.exe",
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", quote_arg(script),
    "-Method", quote_arg(method),
    "-ParamsJsonFile", quote_arg(temp),
    "-PipeName", quote_arg(options.pipeName or pipe_name()),
    "-TimeoutMs", tostring(options.timeoutMs or 5000),
  }, " ")

  local pipe = io.popen(command, "r")
  if not pipe then
    os.remove(temp)
    return nil, "failed to start automation-pipe-call.ps1"
  end
  local output = read_all(pipe)
  local ok, reason, code = pipe:close()
  os.remove(temp)
  if ok then
    return output
  end
  return nil, output ~= "" and output or tostring(reason or code or "pipe call failed")
end

-- Comme ke.call, mais decode la reponse JSON-RPC et retourne directement
-- le champ "result" (table Lua quand le backend renvoie un objet/tableau)
-- au lieu du texte JSON brut. Le champ "error" de la reponse JSON-RPC (s'il
-- existe et est non vide) devient le message d'erreur retourne en 2e valeur,
-- au meme titre qu'un echec de ke.call lui-meme.
function ke.call_table(method, params, options)
  local output, err = ke.call(method, params, options)
  if not output then
    return nil, err
  end
  local decoded, decodeErr = ke.decode_json(output)
  if decoded == nil and decodeErr then
    return nil, "reponse JSON invalide: " .. tostring(decodeErr)
  end
  if type(decoded) == "table" and decoded.error ~= nil and decoded.error ~= "" then
    return nil, tostring(decoded.error)
  end
  if type(decoded) == "table" then
    return decoded.result
  end
  return decoded
end

function ke.ping(message)
  return ke.call("ping", { message or "lua" })
end

function ke.attach(pid)
  return ke.call("attachProcess", { tonumber(pid) })
end

function ke.scan_exact(value, value_type)
  return ke.call("startExactScan", { tostring(value), value_type or "Int32" })
end

function ke.next_scan(mode, value)
  return ke.call("nextScan", { mode or "exact", tostring(value or "") })
end

function ke.candidates(page_index, page_size, filter)
  return ke.call("getCandidates", { page_index or 0, page_size or 50, filter or "" })
end

function ke.kernel_read(address, size)
  return ke.call("readMemoryKernel", { tostring(address), tonumber(size) or 4 })
end

function ke.kernel_write_value(address, value_type, value)
  return ke.call("writeMemoryValueKernel", { tostring(address), value_type or "Int32", tostring(value) })
end

-- PHASE 122 : applyCodePatch/restoreCodePatch sont deja pilotables via
-- ke.call("applyCodePatch", {...}) sans ce wrapper (n'importe quelle methode
-- Q_INVOKABLE l'est, par reflexion QMetaMethod cote automation_pipe_server) --
-- ceci n'est qu'un raccourci de lisibilite, pas une condition d'acces. Le
-- fallback relais PowerShell (EDR bloque VirtualProtectEx depuis
-- KillEngine.exe, voir docs/POWER_UP_ROADMAP.md section O) est transparent
-- ici : options.verify controle juste la relecture de verification.
function ke.apply_code_patch(address_hex, bytes_hex, verify)
  return ke.call_table("applyCodePatch", { tostring(address_hex), tostring(bytes_hex), { verify = verify ~= false } })
end

function ke.restore_code_patch(address_hex)
  return ke.call_table("restoreCodePatch", { tostring(address_hex) })
end

-- Variantes table des wrappers ci-dessus, via ke.call_table : retournent
-- directement la table Lua decodee (candidats, resultat de scan...) au lieu
-- du JSON brut, pour manipuler la reponse sans reparser a la main.
function ke.scan_exact_table(value, value_type)
  return ke.call_table("startExactScan", { tostring(value), value_type or "Int32" })
end

function ke.next_scan_table(mode, value)
  return ke.call_table("nextScan", { mode or "exact", tostring(value or "") })
end

function ke.candidates_table(page_index, page_size, filter)
  return ke.call_table("getCandidates", { page_index or 0, page_size or 50, filter or "" })
end

return ke
