# Backend IA externe (clé API) — chantier de réflexion

Statut : T1-T5 codés et testés (07/09/2026), T6 (vérification terrain) démarré le même jour — premier test live a trouvé et corrigé un bug réel (aucun historique de conversation entre messages, voir `docs/PHASE_TRACKER.md`), reste à re-tester après correctif. Scoping ouvert le 06/09/2026 suite à une
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
en parallèle, tout le reste en découle.

**T1 — Stockage clé API (DPAPI) — fait (07/09/2026, Claude).**
`core/security/dpapi_key_store.h/.cpp` (namespace `killcore`), `CryptProtectData`/
`CryptUnprotectData` derrière `#ifdef _WIN32` (stub explicite non-Windows,
jamais de fallback silencieux), lié à `crypt32` dans `core/CMakeLists.txt`. Ne
touche pas encore QSettings lui-même — c'est une brique de chiffrement pure
(`encrypt(QByteArray) -> QByteArray`/`decrypt(...)`), le stockage réel dans
QSettings viendra avec T4 (`setExternalAiApiKey`). 4 tests unitaires
(`tests/unit/test_dpapi_key_store.cpp`) : round-trip réel (pas de mock, même
esprit que `test_memory_heatmap_collector.cpp`), round-trip chaîne vide, blob
corrompu échoue explicitement, blob vide échoue explicitement. Build + 439/439
tests OK.

**T2 — Schéma d'outils Anthropic — fait (07/09/2026, Claude).**
Décision tranchée par le propriétaire : **option (b)**, enrichir
`tool_registry.cpp` avec de vrais types/descriptions par argument plutôt que du
`string` générique partout. `ArgSpec{name, type, description}` ajouté à
`makeTool()` dans `ai/tool_registry.cpp` (~50 outils, tous les arguments
existants annotés un par un depuis leur usage réel dans
`smart_search_manager.cpp`) ; `requiredArgs` (juste les noms) reste inchangé
pour compat avec les consommateurs existants (`ai_engine.cpp`,
`llama_runtime.cpp`), un nouveau champ `args` (liste typée) s'ajoute en plus.
Nouveau module `ai/anthropic_tool_schema.h/.cpp`
(`killai::toolsToAnthropicSchema(QVariantList) -> QJsonArray`) qui convertit ça
en `input_schema` JSON Schema Anthropic (`type: object`, `properties`,
`required`). 4 tests unitaires (`tests/unit/test_anthropic_tool_schema.cpp`) :
un schéma par outil enregistré, outil avec arguments (types + required
corrects), outil sans arguments (properties vide, pas de required), types
booléen/entier préservés. Le prompt hardcodé de `llama_runtime.cpp` (modèle
local) n'a pas été touché — reste hors scope de T2, piste notée pour plus tard
si utile.

**T3 — Client HTTP Claude (Messages API) — fait (07/09/2026, Claude).**
Réalisé en 3 modules, tous dans `ai/` (pas `apps/desktop/*_manager.*` comme
envisagé initialement — voir écart ci-dessous) :
- `ai/anthropic_messages.h/.cpp` : briques pures sans accès réseau (construction
  des messages user/assistant/tool_result, parsing d'une réponse `/v1/messages`
  en texte + blocs `tool_use`, distinction erreur HTTP explicite ex. 401 vs JSON
  illisible). Entièrement testable sans réseau.
- `ai/claude_backend_client.h/.cpp` (`killai::ClaudeBackendClient`) : boucle
  agentique bornée (`maxToolTurns`, défaut 8, garde-fou anti-boucle infinie) —
  envoie message + tools → reçoit `tool_use` → exécute via un `ToolExecutor`
  injecté → renvoie `tool_result` → répète jusqu'à réponse texte finale.
  Transport HTTP réel via `QNetworkAccessManager` + `QEventLoop` (même patron
  que `core/webview2/cdp_client.cpp::sendCommandSync`), mais **injectable**
  (`HttpPostFn`) pour permettre de tester toute la boucle sans réseau réel.
  Requête non-streamée (pas de SSE), erreur explicite (401, réseau, boucle non
  terminée) — jamais de fallback silencieux. Compteur de requêtes
  (`requestCount()`) incrémenté à chaque appel HTTP réel.
- 15 tests unitaires (`tests/unit/test_anthropic_messages.cpp`,
  `tests/unit/test_claude_backend_client.cpp`), dont la boucle complète
  outil→résultat→réponse finale, l'arrêt après `maxToolTurns`, et la
  propagation d'erreur réseau/401 — tout sans toucher le réseau.

**Écart de conception vs. l'énoncé initial de T3, tranché en cogitant
(07/09/2026)** : le texte d'origine disait "exécute le `Q_INVOKABLE`
correspondant" directement depuis ce module. En creusant, la seule logique
existante qui sait faire "nom d'outil → vrai appel `ApplicationController`"
est celle, très longue (~860 lignes) et non factorisée, embarquée dans
`SmartSearchManager::startSmartSearch` (`apps/desktop/smart_search_manager.cpp`
lignes ~3543-4405) — elle mélange dispatch et contexte local de la fonction
(pas une méthode `dispatch(tool, args)` réutilisable telle quelle). L'extraire
proprement aurait été un refactor invasif d'un fichier chaud, partagé avec
d'autres agents (voir [[parallel_codex_sessions]], [[project_refactor_roadmap_established]])
— hors scope raisonnable d'une tâche T3 sensée être "indépendante". Décision :
`ClaudeBackendClient` ne connaît **aucun** outil KillEngine — le mapping réel
est injecté via `ToolExecutor` (`std::function<QVariantMap(QString, QVariantMap)>`),
explicitement délégué à T4 ("Route le point d'entrée existant..."). Cela
déplace aussi, vers T4, la question de sécurité laissée ouverte : la logique
de dispatch existante refuse déjà d'exécuter un outil `requiresConfirmation=true`
depuis un contexte chat (elle renvoie `requires_confirmation` sans agir, voir
PHASE 140) — T4 devra décider si l'exécuteur injecté au backend Claude adopte
la même politique (probable, cohérent avec PHASE 271-272 : sécurité via
RiskGate à l'exécution, jamais via censure du schéma d'outils) plutôt que
d'exécuter les outils risqués sans confirmation comme le fait le pipe
d'automatisation (mode dev opt-in distinct, voir `docs/AUTOMATION_API.md`).

**T4 — `Q_INVOKABLE` `ApplicationController` + `ToolExecutor` réel — fait (07/09/2026, Claude).**
Portée finalement plus large que prévu : en creusant le mapping tool → appel
réel, ~20 des 57 outils se sont révélés soit asynchrones (résultat livré par
un signal Qt *Finished*, pas par la valeur de retour), soit inexistants côté
`Q_INVOKABLE` (CRUD Trainer, purement Pinia). Décision prise avec le
propriétaire (07/09/2026) : couverture complète tout de suite plutôt qu'une
tranche verticale — détail complet du mécanisme et de chaque mapping outil
dans `docs/PHASE_TRACKER.md`, entrée "EXTERNAL-AI-BACKEND T4".

Résumé de ce qui a été construit :
- `apps/desktop/claude_chat_manager.h/.cpp` (`killengine::ClaudeChatManager`) :
  câble `killai::ClaudeBackendClient` (T3) à `ApplicationController` — clé API
  (DPAPI, QSettings `ai/externalApiKeyBlob`), backend actif (QSettings
  `ai/activeBackend`), et surtout `executeTool()`, le `ToolExecutor` réel pour
  les 57 outils du registre (voir table complète dans PHASE_TRACKER.md).
- Nouveau pont générique "action en attente" (`waitForFrontendAction`,
  `waitForControllerSignal`) qui met en pause la boucle agentique bloquante
  (même thread, `QCoreApplication::processEvents()` pompé en boucle — même
  patron que l'attente 90s du modèle local, `ai/llama_server.cpp`) pour :
  confirmations RiskGate réelles (exécution réelle en C++ après approbation,
  jamais côté frontend), complétion de méthodes `*Async` existantes (signal
  Qt natif, `QEventLoop` bornée), et actions Trainer (round-trip vers Pinia,
  aucun `Q_INVOKABLE` équivalent n'existe).
- `Q_INVOKABLE` ajoutés sur `ApplicationController` :
  `setExternalAiApiKey`/`clearExternalAiApiKey`/`hasExternalAiApiKey` (ce
  dernier ne retourne jamais la clé), `setActiveAiBackend`/`getActiveAiBackend`,
  `getExternalAiRequestCount`, `resolveClaudePendingAction`. Nouveau signal
  `claudePendingActionRequested`.
- `SmartSearchManager::startSmartSearch` (point d'entrée existant du chat)
  court-circuite désormais toute l'heuristique locale et délègue à
  `ClaudeChatManager::sendMessage` quand le backend actif est `"claude"`.
- Câblage frontend minimal mais fonctionnel dans `ui/src/stores/app.ts`
  (`claudePendingActionRequested?.connect(...)`) : route les confirmations
  vers `confirmRiskAction` existant (RiskGate déjà en place, aucune nouvelle
  UX inventée) et les actions Trainer vers les fonctions Pinia existantes
  (`createTrainerFeature`/`deleteTrainerFeature`/`applyTrainerFeature`/...).
  Types ajoutés dans `ui/src/services/backend.ts`. `npm run type-check` et
  `npm run build` OK.
- 3 outils toujours explicitement bloqués en exécution autonome, quelle que
  soit la confirmation (`find_what_writes`, `test_candidate_fields`,
  `patch_file_bytes`) — même politique de sécurité que le chat local (PHASE
  140) : tâche de fond ~1 min avec suivi visuel, ou édition de fichier réel
  sans confirmation cliquable équivalente. `prepare_write_checkpoint`
  également bloqué (dépend de l'historique de scan interne au modèle local,
  jamais peuplé par ce chemin).

Build complet + 454/454 tests unitaires (aucune régression). Pas de nouveaux
tests unitaires pour `ClaudeChatManager` lui-même — dépend trop fortement
d'`ApplicationController`/Qt event loop pour être testé isolément sans mock
lourd ; la vérification réelle est T6 (test terrain).

**T5 — UI Réglages — fait (07/09/2026, Claude).**
Nouvelle section "Backend IA externe (Claude)" dans `SettingsView.vue`, juste
après le panneau "IA locale" existant (même zone de réglages IA) : sélecteur
de backend (`<select>` Local/Claude), champ clé API (`type="password"`,
boutons Enregistrer/Supprimer), compteur de requêtes affiché, clé jamais
renvoyée en clair. Bascule vers `"claude"` gardée par `confirmRiskAction`
(risk `'injection'`, texte explicite sur le compromis confidentialité — même
asymétrie que Stealth/CDP WebView2 : repasser en local ne demande aucune
confirmation). Wrappers minces dans `ui/src/stores/app.ts`
(`refreshExternalAiStatus`/`setExternalAiApiKey`/`clearExternalAiApiKey`/
`setActiveAiBackend`), types déjà ajoutés dans `backend.ts` pendant T4.
`npm run type-check`/`npm run build` OK, build C++ complet + 454/454 tests
OK (aucune régression). **Non fait cette passe** : vérification visuelle live
via CDP (`QTWEBENGINE_REMOTE_DEBUGGING`, technique habituelle du projet) —
les classes CSS réutilisées (`settings-grid`/`model-path-row`/
`model-status-grid`/`status-pill`) sont bien déjà stylées ailleurs dans le
même fichier, mais le rendu réel n'a pas été capturé en écran ; à faire à
l'occasion de T6 ou d'un rapide coup d'œil manuel.

**T6 — Vérification terrain** — dépend de tout ce qui précède.
Build complet + suite de tests unitaires. Puis test réel : activer le backend
Claude, poser une vraie question complexe (ex. reprendre l'investigation
Solitaire XP comme cas de test), vérifier que les bons outils sont appelés et
que le raisonnement multi-tours fonctionne réellement, pas seulement que l'API
répond.
