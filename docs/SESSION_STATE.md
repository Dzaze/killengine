# Session State — Cline

> Fichier de reprise dédié à Cline. Créé le 03/09/2026 après un restart VS Code qui a interrompu la session précédente.
> Rôle de Cline : **chef du contournement** — anti-cheat sur jeux AAA et jeux en ligne, tout ce qui est bypass/circumvention.

## Session précédente — où on s'est arrêté

- **Dernier commit** : `967385a` — "docs(lexicon): add Stealth and Correlation glossary terms"
- **Travail non committé trouvé au restart** :
  - `ui/src/App.vue` : +2 lignes CSS (`min-height: 0; overflow-y: auto;` sur `.nav`) — fix de scroll pour la sidebar, probablement fait pendant la session interrompue.
  - `réponse-claude.md` (racine du repo, **gitignoré — note locale**) : réponse de Claude sur une tâche restante + analyse du blocage EDR (Defender for Endpoint) qui flag la séquence `breakpoint externe → injection CreateRemoteThread` dans la même cible. Root-cause faite le 20/08/2026, mitigation en place (message explicite + `GTEST_SKIP` documenté sur les 4 tests concernés). À garder en local comme référence, pas partagée via git.
- **État du dépôt** : compilable, 402/402 tests unitaires à la dernière validation connue (entrée Stealth Profiler dans PHASE_TRACKER.md).

## Tâche actuelle

- **Objectif** : reprendre le chantier contournement/anti-cheat.
- **Contexte immédiat** :
  - Stealth Profiler livré par Claude le 03/09/2026 (scoring détectabilité 0-100, recommandations, panneau Settings).
  - **Bug `antiDebug`/`dllMask` silencieux : root-causé ET corrigé le 03/09/2026 (Roo, entrée STEALTH-SC2-1 dans PHASE_TRACKER.md).** Cause réelle : `core/debug/anti_debug.cpp` résolvait les adresses anti-debug côté KillEngine et supposait kernel32/ntdll à la même adresse dans la cible — faux avec l'ASLR → INT3 écrits hors module → 0 hook installé. Corrigé avec résolution RVA (offset depuis la base du module côté local, réappliqué sur la base du même module dans la cible, avec gestion du piège kernel32→kernelbase forwarder). `dll_mask.cpp` remonte maintenant la raison précise de chaque échec, et `app.ts` pousse un message Assistant sur échec/warnings stealth (plus silencieux). Build OK, 402/402 tests.
  - `réponse-claude.md` documente le blocage EDR sur `CreateRemoteThread` post-breakpoint — pertinent pour tout contournement qui passe par l'injection.

## Prochaines étapes

- [ ] Committer le fix CSS `ui/src/App.vue` (sidebar scroll)
- [x] Décider du sort de `réponse-claude.md` → **gardé en local, ajouté au `.gitignore`** (note de référence, pas partagée)
- [x] Root-causer le bug `antiDebug`/`dllMask` silencieux (résolution RVA cible, voir STEALTH-SC2-1)
- [ ] Vérifier le fix `antiDebug`/`dllMask` sur une cible réelle (SC2 si dispo) — `applyStealthMode("sc2")` doit maintenant activer les 3 modules, sinon le message Assistant remontera la raison précise
- [ ] Reprendre le chantier contournement anti-cheat AAA/online (à préciser avec le propriétaire)

## Contexte utile

- **Piège EDR connu** : après un cycle `findWhatWrites` (breakpoint externe), toute injection `CreateRemoteThread` dans la même cible peut être refusée (`error=5`) par Defender for Endpoint. Intermittent, heuristique de détection. Voir `réponse-claude.md` + `docs/STRATEGY_ROOM.md` (root-cause 20/08/2026).
- **Pipe d'automatisation** : `KILLENGINE_AUTOMATION_PIPE=1` au lancement de KillEngine.exe pour piloter via `scripts/automation-pipe-call.ps1`.
- **Build** : `.\scripts\build.ps1` puis `.\build\bin\killengine_unit_tests.exe`.