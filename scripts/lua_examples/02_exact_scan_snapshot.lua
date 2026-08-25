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

local value = os.getenv("KILLENGINE_SCAN_VALUE") or "40"
local value_type = os.getenv("KILLENGINE_SCAN_TYPE") or "Int32"
local page_size = 10

local function brief(value)
  if type(value) ~= "table" then
    return tostring(value)
  end

  local fields = {}
  for _, key in ipairs({ "address", "addressHex", "value", "currentValue", "type", "valueType", "confidence" }) do
    if value[key] ~= nil then
      fields[#fields + 1] = key .. "=" .. tostring(value[key])
    end
  end
  if #fields == 0 then
    for k, v in pairs(value) do
      fields[#fields + 1] = tostring(k) .. "=" .. tostring(v)
      if #fields >= 4 then break end
    end
  end
  return table.concat(fields, ", ")
end

print("Exact scan: value=" .. value .. " type=" .. value_type)

local scan, scan_err = ke.scan_exact_table(value, value_type)
if not scan then
  error("startExactScan failed. Attach a process first. Details: " .. tostring(scan_err))
end

print("Scan result: " .. brief(scan))

local candidates, cand_err = ke.candidates_table(0, page_size, "")
if not candidates then
  error("getCandidates failed: " .. tostring(cand_err))
end

local rows = candidates.items or candidates.candidates or candidates.rows or candidates
local total = candidates.total or candidates.totalCount or #rows or 0
print("Candidates shown: " .. tostring(#rows) .. " / total=" .. tostring(total))

for i = 1, math.min(#rows, page_size) do
  print(string.format("%02d  %s", i, brief(rows[i])))
end
