# Stratégies de Scan pour les Minéraux dans StarCraft 2

> Guide pratique pour trouver et modifier les minéraux/vespène dans SC2 sans être détecté.

## Pourquoi le scan classique échoue souvent sur SC2

StarCraft 2 utilise plusieurs techniques qui rendent le scan de valeurs simples difficile :

1. **Encodage des ressources** : Les valeurs peuvent être stockées en centièmes (×100), en flottant, ou chiffrées
2. **Copies UI** : La valeur affichée est souvent une copie dérivée, pas la source réelle
3. **Anti-cheat** : Warden (l'anti-cheat de Blizzard) surveille les accès mémoire suspects
4. **Détection de debugger** : SC2 vérifie régulièrement la présence d'un debugger

---

## Stratégie 1: Mode Stealth (Obligatoire)

Avant tout scan, activez le mode stealth SC2 :

```lua
local ke = require("killengine")

-- Activation complète (anti-debug + masquage)
local stealth = ke.apply_stealth("sc2")
-- stealth.modules.antiDebug      -> true/false
-- stealth.modules.processMask    -> true/false  (masque le nom du processus)
-- stealth.modules.dllMask        -> true/false  (masque les DLL injectées)
```

**Profils disponibles :**
- `"sc2"` : Complet - anti-debug + masquage processus + masquage DLL
- `"default"` : Anti-debug seul
- `"minimal"` : Masquage processus seul

---

## Stratégie 2: Scan Multi-Type (Recommandé en premier)

SC2 stocke les ressources dans différents formats. Essayez plusieurs en une fois :

```lua
-- Scan automatique sur tous les types numériques
local result = ke.call_table("startExactScanMultiType", {
  "50",  -- valeur affichée
  "Auto" -- teste Int32, UInt32, Float32, Float64, Int32×100, etc.
})
```

**Types à tester manuellement si Auto échoue :**
- `Int32` : Valeur exacte (50)
- `Int32` avec valeur ×100 : 5000 pour 50 minéraux
- `Float32` : 50.0
- `Float64` : 50.0 (double précision)

---

## Stratégie 3: Scan Chiffré (XOR)

Si les valeurs semblent "aléatoires" ou changent à chaque partie, elles sont peut-être chiffrées :

```lua
local result = ke.call_table("scanEncryptedValue", {
  "50",           -- valeur cherchée
  "Int32",        -- type de base
  {
    maxResults = 100,
    keySearchBits = 8  -- recherche clés XOR sur 8 bits
  }
})
```

**Note** : C'est plus lent mais peut trouver des valeurs cachées.

---

## Stratégie 4: Trace UI String (Quand le scan numérique échoue)

Si aucune valeur numérique ne fonctionne, partez de l'affichage texte :

```lua
-- 1. Scanner la chaîne affichée (ex: "50" ou "50/200")
local strings = ke.call_table("scanUiStrings", {
  "50",
  { writableOnly = false, maxResults = 50 }
})

-- 2. Analyser les sources numériques autour de la string
local sources = ke.call_table("analyzeUiStringSources", {
  strings.matches[1],  -- première string trouvée
  "50",                -- valeur affichée
  { radiusBytes = 4096 } -- rayon de recherche
})

-- 3. Suivre les sources qui changent avec la valeur
-- (dépensez des minéraux, refaites l'analyse)
```

**Pourquoi ça marche** : La string UI est une copie, mais elle pointe vers la vraie source.

---

## Stratégie 5: Changed Pages Consensus (Pour les valeurs dynamiques)

Pour les ressources qui changent fréquemment, utilisez une session multi-rounds :

```lua
-- Démarrer une session
ke.changed_pages_session_start({ maxPages = 100 })

-- Round 1: Capturer à 50 minéraux
ke.changed_pages_round("50", "45")  -- après avoir dépensé 5

-- Round 2: Capturer à 45 minéraux
ke.changed_pages_round("45", "60")  -- après avoir gagné 15

-- Round 3: Capturer à 60 minéraux
ke.changed_pages_round("60", "55")  -- après avoir dépensé 5

-- Obtenir les adresses confirmées par ≥2 transitions
local consensus = ke.changed_pages_consensus({ minTransitions = 2 })

-- Nettoyer
ke.changed_pages_session_stop()
```

**Avantage** : Élimine les copies UI volatiles qui ne suivent pas toutes les transitions.

---

## Stratégie 6: Écriture via Driver Kernel (Plus discret)

Une fois l'adresse trouvée, préférez l'écriture kernel à WriteProcessMemory :

```lua
-- Plus discret que writeMemoryValue standard
local ok = ke.kernel_write_value(
  "0x7FF123456789",
  "Int32",
  "9999"
)
```

**Pourquoi** : Le driver kernel contourne certaines protections user-mode.

---

## Stratégie 7: Freeze Logiciel (Pas Hardware Breakpoint)

Pour maintenir la valeur, évitez les hardware breakpoints qui sont détectables :

```lua
-- Mauvais (détectable)
-- ke.call_table("freezeWithBreakpoint", { ... })

-- Bon (moins détectable mais moins robuste)
ke.call_table("setFreezeEntry", {
  "0x7FF123456789",
  "Int32",
  "9999",
  { mode = "polling", intervalMs = 50 }
})
```

**Compromis** : Le polling est moins "invincible" mais beaucoup moins visible.

---

## Workflow Complet Recommandé

```lua
local ke = require("killengine")

-- 1. Attacher
ke.attach(pid)

-- 2. Stealth mode
ke.apply_stealth("sc2")

-- 3. Scan multi-type
local result = ke.call_table("startExactScanMultiType", { "50", "Auto" })

-- 4. Si échec → Scan chiffré
if not result or #result.matches == 0 then
  result = ke.call_table("scanEncryptedValue", { "50", "Int32", {} })
end

-- 5. Si échec → Trace UI string
if not result or #result.matches == 0 then
  -- Utiliser scanUiStrings + analyzeUiStringSources
end

-- 6. Affiner avec nextScan
-- (dépenser/gagner des ressources entre chaque scan)

-- 7. Écrire via kernel
ke.kernel_write_value(address, "Int32", "9999")

-- 8. Cleanup
ke.restore_stealth()
```

---

## Pièges Courants

| Problème | Cause probable | Solution |
|----------|---------------|----------|
| Aucun résultat | Valeur ×100 | Scanner 5000 au lieu de 50 |
| Valeur trouvée mais ne change pas | Copie UI | Utiliser Trace UI String |
| Crash après écriture | Mauvais type | Vérifier Int32 vs Float32 |
| Détection par Warden | Pas de stealth | Activer `apply_stealth("sc2")` |
| Valeur revient à la normale | Freeze manquant | Utiliser freeze logiciel |

---

## Exemple Complet

Voir `scripts/lua_examples/sc2_minerals_stealth.lua` pour un script fonctionnel complet.

Usage :
```bash
# Lancer depuis KillEngine (Scripting view) ou ligne de commande
lua sc2_minerals_stealth.lua <pid_sc2> 50 9999
```

---

## Notes sur l'Anti-Cheat

- **Warden** scanne la mémoire périodiquement
- Le mode stealth masque les signatures connues
- Évitez les écritures trop fréquentes (>1/sec)
- Préférez les sessions courtes (scan + écriture + détachement)

**Risque** : Toute modification reste détectable théoriquement. Ce guide minimise les chances, ne les élimine pas.
