# Backend IA externe (clé API) — chantier de réflexion

Statut : proposition, pas encore de code. Scoping ouvert le 06/09/2026 suite à une
session d'investigation live (Solitaire XP, voir `PHASE_TRACKER.md` entrée
`INVESTIGATION-SOLITAIRE-XP-2` et la mémoire `solitaire_memory_editing_technique.md`)
qui a servi de cas d'école.

## D'où ça vient

Pendant la session Solitaire du 06/09/2026, l'utilisateur et Claude Code (agent
externe, pas l'Assistant intégré) ont mené ensemble une investigation multi-étapes
(scan → narrowing → `findWhatWrites` → `disassembleBackward` → `suggestCodePatches`
→ `applyCodePatch`) impliquant : adaptation de stratégie après plusieurs échecs de
capture, interprétation de messages humains ambigus/fautifs en direct, diagnostic
d'une cause de crash à partir d'indices indirects, et une inférence finale non
triviale (un compteur affiché à 5555 mais un gain crédité de 15 → le jeu revalide
son calcul indépendamment de l'affichage).

Question posée ensuite : l'Assistant intégré de KillEngine (modèle local embarqué,
Qwen3.5-2B quantifié Q4_K_M, choisi pour tourner en local/hors-ligne sans setup)
aurait-il pu arriver seul au même résultat ?

Réponse donnée : probablement pas — pas par manque d'outils (l'Assistant a accès
au même surface `Q_INVOKABLE` sans restriction de schéma, décision PHASE 271-272),
mais par manque de capacité de raisonnement multi-tours. Un modèle embarqué de
cette taille est dimensionné pour du triage NLU et de l'exécution d'outils simples,
pas pour de la révision d'hypothèses répétée face à des échecs, ni pour des
inférences à partir d'un écart de chiffres indirect.

## Idée

Ajouter un **second backend IA optionnel**, choisi par l'utilisateur dans les
Réglages, à côté du modèle local embarqué (qui reste le défaut, aucun setup
requis) : une connexion **"bring your own key"** vers **Claude uniquement** pour
commencer (voir décision ci-dessous) pour les tâches qui le justifient.

Ce n'est pas un remplacement du modèle local — c'est un renfort optionnel pour
les cas où la profondeur de raisonnement compte plus que la latence/le coût/la
confidentialité.

## Pourquoi c'est faisable sans complexité excessive

- KillEngine expose déjà tout son `ApplicationController` en `Q_INVOKABLE` sans
  liste blanche (décision propriétaire PHASE 271-272) — le tool-calling natif
  des API Claude/OpenAI peut s'y brancher directement, même schéma d'outils que
  l'Assistant local utilise déjà.
- Le point d'entrée existe déjà côté UI : `SettingsView.vue` a déjà un champ
  "Chemin personnalisé GGUF" (override avancé du modèle local) — le sélecteur de
  backend externe se pose naturellement juste à côté, même zone de réglages IA.
- Le pattern de confirmation existe déjà : `confirmRiskAction`/RiskGate est déjà
  utilisé pour toute action qui change la surface de confiance de l'app (Stealth,
  débogage CDP WebView2) — le même mécanisme peut porter l'avertissement
  confidentialité (voir plus bas) sans inventer une nouvelle UX.

## Le vrai compromis à trancher

Le modèle local embarqué est très probablement un choix délibéré de
confidentialité/hors-ligne — cohérent avec un outil qui fait aussi de l'évasion
EDR/stealth. Dès qu'un backend externe est actif, le contexte des appels d'outils
(adresses mémoire, nom du process, parfois le nom du jeu ciblé, éventuellement des
extraits de code désassemblé) part vers un tiers. Ça doit être :
- strictement opt-in (jamais activé par défaut) ;
- clairement affiché comme tel au moment de l'activation (pas juste dans une
  case à cocher discrète) ;
- facile à désactiver / revenir au modèle local à tout moment.

## Décisions prises (06/09/2026, cogitation propriétaire + Claude)

1. **Un seul provider pour commencer : Claude.** OpenAI/Codex ou tout autre
   provider **ne sera ajouté que sur demande explicite du propriétaire** — ne
   pas généraliser/abstraire vers un système multi-provider par anticipation.
2. **Bascule manuelle globale, pas de routage automatique.** Un routage
   automatique par tâche demanderait que le modèle local (Qwen, qui fait déjà
   le triage NLU en première ligne) apprenne à juger lui-même qu'une tâche
   dépasse ses capacités et passe la main — un vrai sous-problème de
   classification, pas fiable avec un modèle de cette taille. La v1 se limite à
   un choix explicite de l'utilisateur ("backend actif : local / Claude"),
   pas d'auto-évaluation par Qwen.
3. **Stockage de la clé : le plus simple et le plus efficace** — DPAPI Windows
   (`CryptProtectData`, lié au compte Windows de l'utilisateur), blob chiffré
   dans QSettings. Pas de coffre-fort dédié, pas de mot de passe supplémentaire
   à gérer.
4. **Tous les outils disponibles pour le backend Claude, sans restriction** —
   même surface `Q_INVOKABLE` complète que le modèle local (cohérent avec la
   décision PHASE 271-272 de ne pas censurer le schéma), mais via un vrai
   schéma d'outils typé (tool-calling natif Claude) plutôt que la ligne "Schema
   obligatoire" bricolée du modèle local.
5. **Coût géré par l'utilisateur, compteur de transparence seulement** — aucune
   limite imposée par KillEngine, juste un compteur "N requêtes cette session"
   affiché, pas de warning bloquant.
6. **Erreur explicite si la clé est invalide/expirée, jamais de fallback
   silencieux** — tout doit rester transparent pour l'utilisateur : si le
   backend Claude échoue, l'utilisateur doit le savoir clairement, pas basculer
   sans prévenir sur le modèle local en lui faisant croire qu'il a toujours le
   backend puissant actif.

## Découpage en tâches d'implémentation (06/09/2026)

Ordre de dépendance ci-dessous ; T1 et T2 sont indépendantes et peuvent démarrer
en parallèle, tout le reste en découle. Rien n'est commencé.

**T1 — Stockage clé API (DPAPI)** — indépendant.
Petit module dédié (`CryptProtectData`/`CryptUnprotectData`, Win32, même pattern
`#ifdef Q_OS_WIN` que le reste du code), blob chiffré stocké dans QSettings, lié
au compte Windows de l'utilisateur. Tests unitaires : round-trip encrypt/decrypt
(faisable dans le process de test lui-même, aucun besoin d'UI réelle).

**T2 — Schéma d'outils Anthropic** — indépendant de T1.
Fonction de conversion `ToolRegistry::availableTools()` → format `tools` de
l'API Messages Anthropic (`name`/`description`/`input_schema` JSON Schema).
**Point dur identifié en cogitant (06/09/2026)** : `ai/tool_registry.cpp`
(`makeTool()`) ne stocke aujourd'hui que `{name, description, requiredArgs
(juste les noms d'arguments, pas de types), risk, requiresConfirmation}` — pas
de type par argument (int/string/bool/etc.), pas de description par argument.
Anthropic attend un vrai JSON Schema typé. **À trancher avant de coder** :
(a) tout passer en `string` générique dans `input_schema` (rapide, mais Claude
doit deviner les types depuis le contexte, plus de risque d'appels malformés),
ou (b) enrichir `tool_registry.cpp` avec de vrais types/descriptions par
argument (plus de travail up-front, meilleure fiabilité des appels — et ça
profiterait aussi potentiellement au modèle local). Tests unitaires : chaque
outil enregistré produit un schéma valide.

**T3 — Client HTTP Claude (Messages API)** — dépend de T2, indépendant de T1.
Nouveau module (manager dédié, même patron que les autres managers
`apps/desktop/*_manager.*`) utilisant `QNetworkAccessManager` (déjà présent
dans le projet via `core/webview2/cdp_client.cpp` — aucune nouvelle
dépendance). Boucle agentique : envoie message + tools → reçoit un bloc
`tool_use` → exécute le `Q_INVOKABLE` correspondant → renvoie `tool_result` →
répète jusqu'à réponse texte finale. Commencer par une requête simple
non-streamée (pas de SSE) pour la v1 — plus simple, le streaming pourra venir
plus tard si la latence perçue le justifie. Erreur explicite (401 clé
invalide, erreurs réseau) — jamais de fallback silencieux (décision ci-dessus).
Compteur de requêtes incrémenté à chaque appel API.

**T4 — `Q_INVOKABLE` `ApplicationController`** — dépend de T1 + T3.
`setExternalAiApiKey(key)` / `clearExternalAiApiKey()` / `hasExternalAiApiKey()`
(ce dernier ne retourne jamais la clé elle-même, juste un booléen),
`setActiveAiBackend("local"|"claude")` / `getActiveAiBackend()`,
`getExternalAiRequestCount()`. Route le point d'entrée existant du chat
Assistant vers le bon backend selon le choix actif de l'utilisateur.

**T5 — UI Réglages** — dépend de T4.
Nouvelle section dans `SettingsView.vue`, juste à côté du champ "Chemin
personnalisé GGUF" existant (même zone de réglages IA) : sélecteur de backend
(radio "Local" / "Claude"), champ clé API (masqué, boutons Enregistrer/
Supprimer), compteur de requêtes affiché, confirmation `confirmRiskAction`/
RiskGate à l'activation (texte explicite sur le compromis confidentialité, même
pattern que Stealth/CDP WebView2). Wrappers minces dans `backend.ts`/`app.ts`,
même convention que les autres blocs Settings.

**T6 — Vérification terrain** — dépend de tout ce qui précède.
Build complet + suite de tests unitaires. Puis test réel : activer le backend
Claude, poser une vraie question complexe (ex. reprendre l'investigation
Solitaire XP comme cas de test), vérifier que les bons outils sont appelés et
que le raisonnement multi-tours fonctionne réellement, pas seulement que l'API
répond.
