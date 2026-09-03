-- Live Lua REPL (PROPOSITIONS-1 #4) -- driver execute par LuaReplManager
-- (apps/desktop/lua_repl_manager.cpp) dans un process lua.exe garde vivant
-- entre chaque ligne, pour que les variables/require persistent d'une ligne
-- a l'autre (contrairement a executeLuaScript qui relance un process a
-- chaque appel). Protocole : une ligne lue sur stdin = une commande ; toute
-- la sortie (print(), erreurs) de cette commande est ecrite sur stdout,
-- suivie d'un marqueur sentinelle sur sa propre ligne pour que le cote C++
-- sache ou s'arrete la sortie de cette commande (sans dependre du prompt
-- "> " de l'interpreteur interactif standard, plus difficile a parser
-- de facon fiable depuis un pipe).
--
-- Meme module ke.* que les scripts fichier (require("killengine"), LUA_PATH
-- deja positionne par le C++ appelant, meme convention que
-- runLuaScriptProcess) : le comportement des appels ke.* est identique en
-- REPL et en script fichier, aucune surprise entre les deux modes.

local ok_ke, ke_or_err = pcall(require, "killengine")
if ok_ke then
  _G.ke = ke_or_err
else
  io.stderr:write("warning: module killengine introuvable (" .. tostring(ke_or_err) .. ") -- ke.* indisponible\n")
end

local SENTINEL = "\1KE_REPL_END\1"
local STOP_COMMAND = "\1KE_REPL_STOP\1"

while true do
  local line = io.read("*l")
  if line == nil then
    break -- stdin ferme (process C++ parent termine) -- sortie propre.
  end
  if line == STOP_COMMAND then
    break
  end

  if line:match("^%s*$") then
    -- Ligne vide : rien a executer, juste renvoyer le marqueur pour ne pas
    -- bloquer le cote C++ qui attend une reponse par ligne envoyee.
    io.write(SENTINEL .. "\n")
    io.flush()
  else
    -- Meme technique que l'interpreteur Lua interactif standard (lua.c) :
    -- essaie d'abord comme instruction, puis comme expression ("return ...")
    -- si le chargement direct echoue -- permet de taper "1+1" aussi bien que
    -- "local x = 1+1" sans distinction pour l'utilisateur.
    local chunk, loadErr = load(line, "=repl")
    if not chunk then
      local exprChunk, exprErr = load("return " .. line, "=repl")
      if exprChunk then
        chunk, loadErr = exprChunk, nil
      end
    end

    if not chunk then
      io.write("error: " .. tostring(loadErr) .. "\n")
    else
      local results = { pcall(chunk) }
      local success = results[1]
      if not success then
        io.write("error: " .. tostring(results[2]) .. "\n")
      else
        local parts = {}
        for i = 2, #results do
          parts[#parts + 1] = tostring(results[i])
        end
        if #parts > 0 then
          io.write(table.concat(parts, "\t") .. "\n")
        end
      end
    end
    io.write(SENTINEL .. "\n")
    io.flush()
  end
end
