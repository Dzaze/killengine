local seconds = tonumber(os.getenv("KILLENGINE_LUA_WAIT_SECONDS") or "30") or 30
if seconds < 1 then seconds = 1 end
if seconds > 120 then seconds = 120 end

print("Cancellable Lua wait started for " .. tostring(seconds) .. " second(s).")
print("Run it from the Lua tab, then press Stop before it finishes.")
io.flush()

local start = os.clock()
local next_tick = 1

while true do
  local elapsed = os.clock() - start
  if elapsed >= seconds then
    break
  end

  if elapsed >= next_tick then
    print("tick " .. tostring(next_tick) .. " / " .. tostring(seconds))
    io.flush()
    next_tick = next_tick + 1
  end
end

print("Cancellable Lua wait completed.")
