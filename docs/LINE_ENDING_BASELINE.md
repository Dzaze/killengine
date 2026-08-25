> **ATTENTION - Priorité Des Ordres Projet**
> Un ordre prioritaire explicite du propriétaire du projet prime sur les consignes temporaires de session des agents IA.

# KillEngine - Baseline Fins De Ligne

Dernière baseline : 25/08/2026, après `55dd77f`.

Ce document capture l'état réel des fins de ligne du dépôt pour éviter de confondre une dette historique avec une régression introduite par un agent.

## Commande

```powershell
.\scripts\check-line-endings.ps1
```

Le script est report-only par défaut. Les modes stricts (`-FailOnMixed`, `-FailOnLfOnly`) ne doivent pas être activés dans une gate tant que cette baseline n'a pas été transformée en politique explicite ou en liste d'exceptions.

## Résumé Actuel

| Statut | Nombre |
| --- | ---: |
| Fichiers vérifiés | 248 |
| CRLF | 148 |
| LF only | 71 |
| Mixed/CR | 28 |
| No newline | 1 |

## Fichiers Mixtes À Surveiller

Ces fichiers sont les plus importants à traiter en premier si un nettoyage EOL dédié est décidé, car ils mélangent plusieurs conventions dans un même fichier.

```text
AGENTS.md
ai/ai_engine.h
ai/llama_runtime.cpp
ai/tool_registry.cpp
core/CMakeLists.txt
core/debug/hardware_breakpoint.cpp
core/debug/hardware_breakpoint.h
core/debug/inprocess_breakpoint_ipc.h
core/debug/page_guard.cpp
core/inject/dll_injector.cpp
core/logging/logger.cpp
core/pointer/pointer_chain.cpp
core/pointer/pointer_scanner.cpp
core/process/process_enumerator.cpp
core/process/process_enumerator.h
core/profiles/locator.cpp
core/profiles/locator.h
docs/PHASE_TRACKER.md
scripts/build_ui.ps1
scripts/build-kernel-driver.ps1
scripts/configure.ps1
scripts/package-windows.ps1
tests/benchmarks/benchmark_main.cpp
ui/src/qwebchannel.d.ts
ui/src/views/MemoryView.vue
ui/src/views/ProcessView.vue
ui/src/views/SettingsView.vue
ui/src/views/TrainerView.vue
```

## Politique Provisoire

- Ne pas convertir ces fichiers au passage d'un autre chantier.
- Si un fichier mixte doit être touché pour une feature, faire un patch chirurgical et vérifier que le diff ne devient pas une conversion globale.
- Un nettoyage EOL, s'il est décidé, doit être un commit séparé avec validation `git diff --check`, scan mojibake et `.\scripts\check-line-endings.ps1`.
- Les fichiers LF-only historiques restent une dette documentée, pas une erreur bloquante.
