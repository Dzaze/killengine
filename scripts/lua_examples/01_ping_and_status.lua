local function configure_package_path()
  local root = os.getenv("KILLENGINE_ROOT")
  if root and root ~= "" then
    package.path = root .. "\\scripts\\?.lua;" .. package.path
    return
  end

  package.path = ".\\scripts\\?.lua;..\\?.lua;..\\scripts\\?.lua;" .. package.path
end

configure_package_path()

local ke = require("killengine")

local function print_table(title, value)
  print(title)
  if type(value) ~= "table" then
    print("  " .. tostring(value))
    return
  end
  for k, v in pairs(value) do
    print("  " .. tostring(k) .. " = " .. tostring(v))
  end
end

local pong, ping_err = ke.call_table("ping", { "lua example: ping" })
if not pong then
  error("KillEngine automation pipe unavailable: " .. tostring(ping_err))
end

print_table("Ping", pong)

local status, status_err = ke.call_table("getLuaScriptingStatus", {})
if not status then
  error("getLuaScriptingStatus failed: " .. tostring(status_err))
end

print_table("Lua scripting status", status)
