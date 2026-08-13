## Ressources StarCraft 2 : type à chercher dans KillEngine

### ⚠️ IMPORTANT : Profondeur de scan (la cause #1 d'échec)

SC2 consomme **2 à 3 Go de RAM**. Le snapshot unknown est plafonné par défaut à **128 Mo**, ce qui ne couvre qu'environ **5 %** de la mémoire du jeu. Si tu ne trouves pas les ressources, c'est presque toujours à cause de ça.

**Solution** : Dans la vue Expert → section Unknown, règle la **Profondeur** sur :
- **Auto** (recommandé) — KillEngine mesure la mémoire writable du jeu et calcule la profondeur optimale automatiquement
- **1024 Mo** minimum pour SC2, idéalement **2048 Mo**
- Vérifie après la capture que le message **"limite atteinte"** ne s'affiche plus

Le snapshot compresse en LZ4 (~30-50 % de la taille brute), donc 2048 Mo capturés ≈ 600-1000 Mo sur disque temporairement.

### Type de valeur : **Int32** (entier signé 32-bit) — le plus fiable

Dans SC2, les ressources sont stockées en mémoire comme des **entiers 32-bit** :

| Ressource | Type | Notes |
|-----------|------|-------|
| **Minéraux** | Int32 | Entier pur, jamais négatif |
| **Gaz Vespene** | Int32 | Idem |
| **Approvisionnement (supply/pop)** | Int32 | Used + Cap |
| **Terrazine / Custom** | Int32 | Idem |

> ⚠️ Ne **pas** utiliser Float32/Float64 pour les ressources de base SC2 — ce sont des nombres entiers. Les floats apparaissent plutôt pour les HP/énergie/cooldowns des unités.

### Méthodologie de scan unknown (la plus sûre)

1. **Premier scan** → `Unknown initial value` → type **Int32**
   - Ne PAS sélectionner "stable/unchanged" en premier (ça saturerait le scan avec ~millions d'adresses).

2. **Comparaisons suivantes** (le plus efficace pour SC2) :
   - Tu récoltes des minéraux → bouton **`ça augmente` (Increased)**
   - Tu dépenses → bouton **`ça diminue` (Decreased)**
   - Tu ne fais rien de tes ressources pendant quelques secondes → **`stable` (Unchanged)** *(ok en 2e comparaison et après, pour éliminer le bruit)*

3. **Répéter 4–6 fois** jusqu'à réduire à une dizaine de candidats.

4. **Vérifier** : la valeur affichée en jeu doit correspondre exactement au nombre lu dans KillEngine (ex : `150` minéraux = `150` en mémoire, pas 1500 ni 15.0).

### Astuces spécifiques SC2

- **Double stockage possible** : SC2 garde parfois une valeur "displayed" ET une "real" (notamment le gaz qui se vide). Si tu vois deux candidats, freeze-les et regarde lequel colle à l'UI.
- **Ne pas se fier au "Unknown + Stable" seul** : les compteurs internes (animations, timers) restent stables aussi — tu te retrouverais avec du bruit. Alterne avec **Increased/Decreased** basé sur tes actions concrètes en jeu.
- **Approvisionnement** : cherche Int32 aussi. `Supply used` et `Supply cap` sont deux adresses séparées — augmente ta pop pour cibler `used`, construis des Supply Depots/Overlords/Pylons pour cibler `cap`.
- **Fast Scan** : laisse activé (alignement auto sur 4 octets pour Int32 = parfait).
- **Région** : tu peux cocher `writable only` pour accélérer — les ressources vivent en heap writable.

### Résumé court

```yaml
Type        : Int32
1er scan    : Unknown initial value
Comparaisons: Increased (récolte) / Decreased (dépense) / Changed
Stable      : OK en 2e passe et après, JAMAIS en premier
Vérif       : valeur mémoire == valeur affichée à l'écran
```

Avec ça tu devrais locker les minéraux/gaz/supply de SC2 en quelques passes, même sans connaître la valeur initiale.

---

## ⚠️ IMPORTANT : Pourquoi tes écritures/freezes ne tiennent pas sur SC2

StarCraft 2 est un jeu AAA qui pose des défis spécifiques au scan mémoire classique. Si tes valeurs "reviennent" après écriture ou freeze, voici les causes et solutions :

### Cause 1 : Le jeu réécrit plus vite que KillEngine
SC2 met à jour ses valeurs ~60 fois par seconde (toutes les ~16 ms). Le freeze par défaut de KillEngine tourne à **100 ms** → le jeu réécrit 6 fois entre deux écritures.

**Solution** : Réduis l'intervalle de freeze via l'API `setFreezeInterval(16)` (16 ms = une frame).
- Bornes : 10 ms (très agressif) à 2000 ms (économique)
- Valeur recommandée pour SC2 : **16 ms**

### Cause 2 : La page mémoire est protégée
`WriteProcessMemory` peut échouer silencieusement si la page n'est pas `writable`.

**Solution** : KillEngine (Phase 16) fait maintenant un **retry automatique avec `VirtualProtectEx`** → rend la page `PAGE_EXECUTE_READWRITE` le temps de l'écriture, puis restaure la protection. Ça aide quand l'échec vient de la protection mémoire, mais ça ne remplace pas la recherche de la vraie source gameplay.

### Cause 3 : Tu as trouvé la valeur "displayed", pas la "real"
SC2 stocke souvent une valeur d'affichage ET une valeur réelle. Si tu freezes l'affichage, le jeu la réécrit depuis la vraie source.

**Solution** : Il faut remonter à l'instruction qui écrit (find what writes), puis à la source gameplay. Voir `docs/SC2_IMPROVEMENT_ANALYSIS.md` pour les détails techniques.

### Cause 4 : Profondeur de scan insuffisante
Voir la section en haut de ce document : SC2 consomme 2–3 Go de RAM, le snapshot unknown par défaut (128 Mo) ne couvre que ~5 % de la mémoire.

---

## 📋 Checklist de dépannage SC2

```yaml
1. Profondeur scan  : Auto ou 2048 Mo minimum
2. Type             : Int32 (pas Float pour les ressources)
3. Intervalle freeze : 16 ms (setFreezeInterval) si la valeur clignote
4. VirtualProtectEx : automatique (Phase 16), vérifie les logs
5. Double stockage  : freeze les deux candidats si tu en trouves plusieurs
6. Find what writes : (Phase 17 future) pour remonter à la vraie source
```

Pour l'analyse technique complète des approches alternatives (hardware breakpoints, page guards, DLL injection), voir **`docs/SC2_IMPROVEMENT_ANALYSIS.md`**.
