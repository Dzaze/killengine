> **ATTENTION - Tracker actif allégé (nettoyage 29/08/2026, deuxième passe 31/08/2026 - PHASE 262, troisième passe 31/08/2026 - PHASE 273)**
> L'historique complet détaillé des phases validées a été transféré dans `docs/PHASE_TRACKER_HISTORY.md` pour ne rien perdre tout en gardant ce fichier lisible. Les agents doivent toujours lire `AGENTS.md` puis ce tracker actif avant d'agir ; consulter l'historique quand une ancienne phase, une décision passée ou un détail de validation est nécessaire.

# KillEngine Phase Tracker

Source of truth produit : `KILLENGINE_PROJECT_SPEC.md`
Historique détaillé : `docs/PHASE_TRACKER_HISTORY.md`
Roadmap power-up : `docs/POWER_UP_ROADMAP.md`
Roadmap refactorisation : `docs/REFACTOR_ROADMAP.md`

## État courant

- Phase 11 prototype, Phase 12 / polishing V1, Phase 13 (régression V1) : complètes et closes.
- PHASE 14A/14B, PHASE 17, PHASE 120-A/B/C/D, audit Arsenal 4 agents (PHASE 187 et ses corrections), la clarification `chat_memory_write`/`chat_memory_freeze`, PHASE 205 (branchement InvestigationView.vue) et PHASE 206 (Mode Automation, toggle + doc `docs/AUTOMATION_API.md`) : toutes closes et archivées en détail dans `docs/PHASE_TRACKER_HISTORY.md`.
- Dépôt git consolidé le 29/08/2026 : `main` remis à jour (fast-forward) sur l'unique branche de travail active, branches mortes supprimées.
- Refactor backend C1-C14 et stores frontend S1-S12 clos au 30/08/2026 — détails archivés dans `docs/PHASE_TRACKER_HISTORY.md`, roadmap cochée dans `docs/REFACTOR_ROADMAP.md`.
- **Carnet d'hypothèses (chantier "PHASE 120 v2", 30/08/2026)** : les 4 sous-phases 120-E/F/G/H sont closes — détail archivé. Ne pas relancer un nouveau chantier sur ce sujet sans accord explicite du propriétaire (règle posée le 27/08/2026, jamais levée).
- PHASES 241-261 (30-31/08/2026) closes et archivées : consensus multi-round Changed Pages (SC2 solarite), Page Guard multi-capture même PID, modules stealth (NtCreateThreadEx, process/DLL mask, anti-debug), corrections Assistant (négations Solitaire, pivot DLL/modules), vérifications visuelles live CLR Inspector/kernel driver.
- PHASES 263-270 (31/08/2026) closes et archivées : session de test terrain live du raisonnement Assistant (via pipe, `Solitaire.exe` réel) rejouant les corrections PHASE 263-269 (pivot DLL/modules sans/avec valeur, priorisation modules, suite conversationnelle, réduction explicite, Trace UI string et Changed Pages prioritaires sur négation) ; 3 bugs supplémentaires trouvés et corrigés en conditions réelles (`.exe` sans mot "module", régression `wantsChangedPages` sur conseil général, outils stealth oubliés du schéma LLM). Suite unitaire 291/291 à la clôture.
- PHASES 271-272 (31/08/2026) closes et archivées : décision propriétaire d'exposer tous les outils au LLM sans restriction de schéma (sécurité conservée via RiskGate frontend, pas via censure du schéma), uniformisation du nommage `getStealthStatus()`.
- **Investigation Solitaire XP (31/08/2026, en pause)** : recherche de la véritable adresse XP sur `Solitaire.exe` (PID 3676) via pipe. Pistes natives épuisées : scan exact Int32 (0 candidat, 2x), Float32 exact a convergé à 3 candidats mais aucun n'affecte l'affichage réel (écriture sans effet), unknown-scan générique a plafonné (~42k candidats, trop de bruit "actif"), scan mémoire brut sur les 3 process renderers WebView2 enfants de Solitaire donne 43 hits identiques dans les 3 (quasi certainement du bruit moteur V8/Blink, pas la donnée réelle). Conclusion : la vraie source est probablement soit dans le fichier de sauvegarde local (`.sgi`, cf. précédent similaire sur les Bulles), soit dans l'état JS du WebView2 — d'où le chantier ci-dessous.

## Règle d'utilisation

- **Règle de branche KillEngine (décision propriétaire, 29/08/2026)** : travailler et committer directement sur `main`. Ne pas créer de branche `agent/...` dans ce dépôt.
- Ajouter les nouvelles phases ici, en bas de fichier, avec quoi/pourquoi/comment vérifié.
- Ne pas remettre tout l'historique dans ce fichier : dès qu'une phase est close, transférer le détail complet vers `docs/PHASE_TRACKER_HISTORY.md` et ne garder ici qu'un renvoi court.
- Ne jamais supprimer d'historique sans transfert explicite.
- Pour les gros détails d'une phase future, préférer un document dédié dans `docs/` puis garder ici un résumé avec lien.

## Validations restantes

Aucune.

## Journal actif

### Chantier en cours — Inspection WebView2/JS (CDP)

Démarré le 31/08/2026, suite à l'investigation Solitaire XP ci-dessus (voir "État courant"). Objectif : donner à KillEngine un troisième mode d'investigation, à côté du scan mémoire natif et du CLR Inspector (.NET/Mono) — l'inspection d'état JavaScript pour les cibles hybrides natif+web (apps Store WebView2, Electron, CEF), où la vraie donnée n'est ni en mémoire native brute ni dans un runtime managé .NET.

- **Constat déclencheur** : sur Solitaire (UWP, WebView2 embarqué), un scan mémoire brut sur les process renderers Chromium enfants donne des résultats identiques (43 hits) sur 3 process indépendants — signature classique de bruit moteur (constantes V8/Blink), pas de donnée applicative. V8 stocke les nombres JS de façon taguée/boxée, pas comme un float natif à une adresse prévisible : le scan mémoire classique n'est structurellement pas le bon outil pour cette couche.
- **Approche envisagée** : client Chrome DevTools Protocol (CDP) — le même mécanisme déjà utilisé pour piloter/inspecter l'UI de KillEngine lui-même (voir historique CDP screenshot/gotchas). `Runtime.evaluate` pour lire une variable JS globale directement, `Debugger` pour poser un breakpoint JS (équivalent "find what writes" côté JS), `DOM` pour lire le texte affiché.
- **Blocage à résoudre en premier** : le WebView2 d'une app tierce n'expose pas de port de debug par défaut. Il faut forcer `--remote-debugging-port` (variable d'env `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` ou clé de registre `HKCU\Software\Policies\Microsoft\Edge\WebView2\...`) puis relancer le process cible — pas d'attache possible sur une instance déjà lancée sans redémarrage.
- **Prochaine étape** : sous-phase A en premier (prototype one-off sur Solitaire) pour valider l'approche avant d'investir dans les suivantes. **Bloquée au 31/08/2026, voir résultat détaillé ci-dessous** — reprendre après décision (tenter un reboot complet, ou basculer sur le fichier de sauvegarde).

**WEBVIEW-A — résultat du prototype (31/08/2026, Claude, live sur Solitaire.exe PID variable selon relances)** :
1. ❌ Policy registre `HKCU\SOFTWARE\Policies\Microsoft\Edge\WebView2\AdditionalBrowserArguments` (posée avec accord explicite utilisateur, script créé : `scripts/webview2-remote-debugging.bat on|off`) — relance à froid du process hôte WebView2 confirmée (nouveau PID, `CreationDate` fraîche), mais aucun process de la hiérarchie (browser/gpu/renderers/utility) ne porte `--remote-debugging-port` dans sa ligne de commande. Hypothèse : policy "sensible" non honorée sur machine non-managée (pas de domaine/Azure AD/MDM) — comportement Chromium/WebView2 documenté pour `AdditionalBrowserArguments`.
2. ✅ Variable d'environnement `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9333` (`setx`, scope User) — après relance complète (Solitaire + son hôte WebView2 tués puis rouverts), le flag apparaît bien dans la ligne de commande du browser/main **et** de plusieurs renderers. Pas besoin de déconnexion/reconnexion de session : le broadcast `WM_SETTINGCHANGE` de `setx` a suffi.
3. ✅ Port confirmé en écoute : `Get-NetTCPConnection -LocalPort 9333` renvoie `State=Listen`, `OwningProcess` = le PID du hôte WebView2 fraîchement lancé.
4. ⚠️ Première tentative HTTP (`http://127.0.0.1:9333/json`) : timeout (pas un refus explicite). Hypothèse posée : isolation réseau loopback AppContainer (restriction Windows par défaut empêchant les connexions loopback entrantes vers une app UWP sandboxée).
5. ✅ Exemption posée : `CheckNetIsolation.exe LoopbackExempt -a -n="Microsoft.MicrosoftSolitaireCollection_8wekyb3d8bbwe"` (PackageFamilyName confirmé via `Get-AppxPackage`), vérifiée présente via `CheckNetIsolation.exe LoopbackExempt -s`.
6. ❌ **Toujours bloqué après exemption + relance complète à froid** : `Test-NetConnection 127.0.0.1 -Port 9333` → `TcpTestSucceeded=False`, échec au niveau TCP brut (pas seulement HTTP/CDP) malgré port en `Listen` et exemption confirmée. Cause non identifiée avec certitude — hypothèse principale : pare-feu Windows Defender ou filtre WFP AppContainer supplémentaire au-delà du simple `LoopbackExempt`, potentiellement nécessitant un reboot complet pour se rafraîchir (non testé, jugé disproportionné pour un prototype one-off à ce stade).
- **Conclusion intermédiaire** : l'obtention du flag de debug est résolue (variable d'env, sans policy ni reboot), mais l'accès réseau au port reste bloqué par une couche de sécurité Windows supplémentaire non résolue en session. Chantier mis en pause ; décision à prendre avec le propriétaire avant de continuer (reboot test, règle pare-feu explicite, ou abandon au profit de la piste fichier de sauvegarde pour l'objectif XP Solitaire initial).

**Découpage en sous-phases** (même principe que PHASE 188 : lanes de fichiers disjointes pour permettre un travail multi-agent sans collision — aucune assignation d'agent pour l'instant, à répartir à l'ouverture) :

- **WEBVIEW-A — Prototype relance + port de debug** (bloquant, à faire en premier, pas de parallélisation possible avant que ça marche). Fichiers : nouveau, pas de fichier existant touché (script/manuel d'abord). Sortie attendue : preuve que `Solitaire.exe` relancé avec `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9222` expose bien un endpoint CDP interrogeable (`http://127.0.0.1:9222/json`).
- **WEBVIEW-B — Client CDP WebSocket + gestion registre/relance**. Fichiers : nouveaux uniquement (ex. `apps/desktop/webview_cdp_client.h/.cpp`, `apps/desktop/webview_debug_launcher.h/.cpp`). Dépend de WEBVIEW-A validé.
- **WEBVIEW-C — Outils Assistant (`ToolRegistry` + routage IA)**. Fichiers : `ai/tool_registry.cpp`, `ai/ai_engine.cpp`, `ai/llama_runtime.cpp` (ligne schéma). Dépend de l'API exposée par WEBVIEW-B (peut être stubbé/développé en parallèle sur une interface convenue).
- **WEBVIEW-D — Intégration `ApplicationController` (Q_INVOKABLE + pipe) et classification de risque**. Fichiers : `apps/desktop/application_controller.h/.cpp`. Dépend de WEBVIEW-B.
- **WEBVIEW-E — UI Assistant/Expert**. Fichiers : `ui/src/views/AssistantView.vue`, `ui/src/services/backend.ts`, `ui/src/stores/app.ts` (RiskGate). Dépend de WEBVIEW-C/D côté contrat d'API, peut démarrer sur maquette avant que le backend soit prêt.

**Exigence produit notée le 31/08/2026 (à porter par WEBVIEW-D + WEBVIEW-E)** : la clé de registre `HKCU\SOFTWARE\Policies\Microsoft\Edge\WebView2\AdditionalBrowserArguments` (posée manuellement en terminal pour le prototype WEBVIEW-A, avec accord explicite de l'utilisateur à chaque fois) doit devenir une fonction dans Paramètres — un vrai RiskGate (`confirmRiskAction`, même mécanisme que le reste de l'app) qui explique clairement au moment de l'activation : pourquoi cette clé est nécessaire (exposer un port de debug CDP pour inspecter l'état JS des apps hybrides WebView2/Electron), ce qu'elle fait concrètement (force `--remote-debugging-port` sur **tous** les hôtes WebView2 du user Windows courant, pas seulement la cible visée — portée large à assumer explicitement), et comment la retirer (bouton de désactivation qui supprime la clé). Ne jamais la poser silencieusement.

Règle de collision : chaque sous-phase ne touche que sa propre liste de fichiers ci-dessus ; toute extension hors périmètre se coordonne avant modification (même règle que [[parallel_split_phase188_codex_pointer_map_deps]]).
