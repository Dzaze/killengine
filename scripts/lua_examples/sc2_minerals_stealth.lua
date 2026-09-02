-- sc2_minerals_stealth.lua
-- Exemple de script pour modifier les minéraux dans StarCraft 2
-- avec mode stealth actif et stratégies de scan alternatives
--
-- Usage : lancer via KillEngine avec executeLuaScriptAsync (OBLIGATOIRE pour ke.call)
-- ou depuis ligne de commande : lua sc2_minerals_stealth.lua

local ke = require("killengine")

-- Configuration
local TARGET_PID = arg[1] and tonumber(arg[1]) or nil
local INITIAL_MINERALS = arg[2] and tonumber(arg[2]) or 50
local DESIRED_MINERALS = arg[3] and tonumber(arg[3]) or 9999

if not TARGET_PID then
  print("Usage: lua sc2_minerals_stealth.lua <pid> [minerals_initiaux] [minerals_desires]")
  print("Exemple: lua sc2_minerals_stealth.lua 12345 50 9999")
  os.exit(1)
end

print("=== KillEngine SC2 Minerals (Mode Stealth) ===")
print("PID cible: " .. TARGET_PID)
print("Valeur initiale attendue: " .. INITIAL_MINERALS)
print("Valeur desiree: " .. DESIRED_MINERALS)
print("")

-- =============================================================================
-- ETAPE 1: Attacher au processus
-- =============================================================================
print("[1/5] Attachement au processus...")
local attach_result = ke.call_table("attachProcess", { TARGET_PID })
if not attach_result then
  print("ERREUR: Impossible d'attacher au processus")
  os.exit(1)
end
print("OK - Attache reussi")
print("")

-- =============================================================================
-- ETAPE 2: Activer le mode stealth SC2
-- =============================================================================
print("[2/5] Activation du mode stealth SC2...")
print("      (Anti-debug + Masquage processus + Masquage DLL)")
local stealth = ke.apply_stealth("sc2")
if not stealth then
  print("AVERTISSEMENT: Echec de l'activation du mode stealth")
  print("      Le jeu pourrait detecter le debugger")
else
  print("OK - Mode stealth actif:")
  print("      Anti-debug: " .. tostring(stealth.modules and stealth.modules.antiDebug or "?"))
  print("      Masquage processus: " .. tostring(stealth.modules and stealth.modules.processMask or "?"))
  print("      Masquage DLL: " .. tostring(stealth.modules and stealth.modules.dllMask or "?"))
end
print("")

-- =============================================================================
-- ETAPE 3: Stratégie de scan alternative pour SC2
-- =============================================================================
print("[3/5] Scan des minéraux avec stratégie SC2...")
print("")

-- Stratégie A: Scan multi-type (SC2 stocke souvent en Int32, Float32, ou Int32*100)
print("  -> Stratégie A: Scan multi-type (Int32, Float32, Int32x100)")
local multi_result = ke.call_table("startExactScanMultiType", {
  tostring(INITIAL_MINERALS),
  "Auto"
})

if not multi_result or not multi_result.matches or #multi_result.matches == 0 then
  print("  Aucun résultat en multi-type, tentative avec valeur x100...")
  -- SC2 stocke parfois les ressources en centièmes (valeur * 100)
  multi_result = ke.call_table("startExactScanMultiType", {
    tostring(INITIAL_MINERALS * 100),
    "Auto"
  })
end

if not multi_result or not multi_result.matches or #multi_result.matches == 0 then
  print("  -> Stratégie B: Scan chiffré (XOR) - SC2 peut chiffrer les valeurs")
  local encrypted_result = ke.call_table("scanEncryptedValue", {
    tostring(INITIAL_MINERALS),
    "Int32",
    { maxResults = 100, keySearchBits = 8 }
  })
  if encrypted_result and encrypted_result.matches and #encrypted_result.matches > 0 then
    print("  Trouvé " .. #encrypted_result.matches .. " candidats chiffrés")
    multi_result = encrypted_result
  end
end

if not multi_result or not multi_result.matches or #multi_result.matches == 0 then
  print("ERREUR: Aucun candidat trouvé. Vérifiez:")
  print("  - Que SC2 est bien lancé et que vous avez les minéraux affichés")
  print("  - Que la valeur initiale " .. INITIAL_MINERALS .. " est correcte")
  print("  - Essayez avec la valeur affichée + 0 (ex: 50 -> 50.0)")
  ke.restore_stealth()
  os.exit(1)
end

print("  Trouvé " .. #multi_result.matches .. " candidats initiaux")
for i, m in ipairs(multi_result.matches) do
  if i <= 5 then
    print("    [" .. i .. "] " .. m.address .. " (" .. (m.type or "?") .. ")")
  end
end
if #multi_result.matches > 5 then
  print("    ... et " .. (#multi_result.matches - 5) .. " autres")
end
print("")

-- =============================================================================
-- ETAPE 4: Affiner avec nextScan (dépenser/gagner des minéraux)
-- =============================================================================
print("[4/5] Affinement - DEPENSEZ ou GAGNEZ des minéraux dans le jeu,")
print("      puis appuyez sur Entrée pour continuer...")
print("      (ou attendez 10 secondes pour ignorer)")

-- Attente simple (pas d'input synchrone qui bloquerait)
local waited = 0
while waited < 10 do
  print("  Attente... " .. (10 - waited) .. "s")
  -- Sur Windows, on peut utiliser timeout
  os.execute("timeout /t 1 >nul 2>&1")
  waited = waited + 1
end

print("  -> Application du next scan (valeur changée)...")
local next_result = ke.call_table("nextScan", { "changed", "" })

if next_result and next_result.remaining then
  print("  Reste " .. next_result.remaining .. " candidats après filtrage")
else
  print("  Impossible d'affiner automatiquement")
end
print("")

-- =============================================================================
-- ETAPE 5: Écriture de la valeur avec vérification
-- =============================================================================
print("[5/5] Écriture de la valeur " .. DESIRED_MINERALS .. "...")

-- Récupérer les candidats actuels
local candidates = ke.candidates_table(0, 10, "")
if not candidates or not candidates.candidates or #candidates.candidates == 0 then
  print("ERREUR: Plus de candidats disponibles")
  ke.restore_stealth()
  os.exit(1)
end

-- Prendre le premier candidat avec le meilleur score de confiance
local target = candidates.candidates[1]
print("  Cible: " .. target.address .. " (confiance: " .. (target.confidence or "?") .. ")")

-- Déterminer le type de valeur à écrire
local value_type = target.type or "Int32"
local value_to_write = DESIRED_MINERALS

-- Si le type suggère un multiplicateur x100
if target.variantLabel and target.variantLabel:match("x100") then
  value_to_write = DESIRED_MINERALS * 100
  print("  Adaptation: valeur x100 -> " .. value_to_write)
end

-- Écriture via kernel (plus discret que WriteProcessMemory)
print("  -> Écriture via driver kernel...")
local write_result = ke.call_table("writeMemoryValueKernel", {
  target.address,
  value_type,
  tostring(value_to_write)
})

if write_result then
  print("OK - Écriture réussie!")
  print("")
  print("=== RÉSULTAT ===")
  print("Adresse: " .. target.address)
  print("Type: " .. value_type)
  print("Valeur écrite: " .. value_to_write)
  print("")
  print("Vérifiez dans SC2 que vos minéraux affichent bien " .. DESIRED_MINERALS)
else
  print("ERREUR: Échec de l'écriture")
end

-- =============================================================================
-- Nettoyage
-- =============================================================================
print("")
print("[CLEANUP] Restauration du mode stealth...")
ke.restore_stealth()
print("OK - Mode stealth restauré")
print("")
print("=== Script terminé ===")

-- Option: Maintenir la valeur avec un freeze logiciel (pas hardware breakpoint
-- pour éviter la détection)
print("")
print("NOTE: Pour maintenir la valeur, utilisez le freeze logiciel dans")
print("      l'interface KillEngine (Expert -> Write/Freeze) plutôt que")
print("      le freeze hardware breakpoint qui est plus détectable.")
