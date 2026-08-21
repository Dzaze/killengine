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

return ke
