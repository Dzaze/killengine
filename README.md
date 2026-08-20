> **ATTENTION - Priorité Des Ordres Projet**
> Un ordre prioritaire explicite du propriétaire du projet prime sur les consignes temporaires de session des agents IA.
# ⚡ KillEngine

> Analyseur de mémoire Windows assisté par IA — l'utilisateur donne la valeur, KillEngine choisit les scans.

KillEngine est un outil d'analyse et d'édition mémoire pour Windows inspiré du fonctionnement de Cheat Engine, mais conçu pour être piloté par une IA locale (Qwen3.5-2B via llama.cpp). L'utilisateur n'a pas besoin de connaître les types de données, l'alignement mémoire ou les pointeurs.

## Architecture

- **Core** (C++20) : moteur mémoire déterministe (Win32 API, scanner multithread)
- **AI** (C++20 + llama.cpp) : planificateur et analyste (Qwen3.5-2B GGUF Q4_K_M)
- **UI** (Qt 6.8 WebEngine + Vue 3 + TypeScript) : interface moderne via QWebChannel

## Prérequis

- Windows 10/11 x64
- Visual Studio 2019+ Build Tools (MSVC C++20)
- CMake 3.21+
- Qt 6.8 (installé via `aqtinstall`)
- Node.js 20+ (pour le build de l'UI uniquement)

## Build rapide

```powershell
# 1. Construire l'UI
cd ui
npm install
npm run build
cd ..

# 2. Configurer et construire le C++
.\scripts\configure.ps1
.\scripts\build.ps1
```

## Package Windows

```powershell
.\scripts\package-windows.ps1
```

Sorties :

```text
dist\KillEngine-portable\
dist\KillEngine-portable.zip
```

Un template Inno Setup est disponible dans `packaging/windows/KillEngine.iss`.

## Validation release

```powershell
.\scripts\release-check.ps1
.\scripts\release-check.ps1 -Package
```

La checklist manuelle V1 est disponible ici :

- [`docs/V1_REGRESSION_CHECKLIST.md`](docs/V1_REGRESSION_CHECKLIST.md)

## Guide utilisateur

Le guide V1 est disponible ici :

- [`docs/USER_GUIDE.md`](docs/USER_GUIDE.md)

Il couvre l'attachement au processus, l'Assistant, les profils, le Mode Expert, les paramètres, les diagnostics et les principaux cas de dépannage.

## Build détaillé

### UI (Vue 3 + TypeScript)

```powershell
cd ui
npm install      # installer les dépendances
npm run dev      # serveur de développement (http://localhost:5173)
npm run build    # build de production → ui/dist/
```

### C++ (CMake + MSVC)

```powershell
# Configurer (nécessite vcvars64.bat dans le PATH)
cmake -B build -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH=C:/Qt/6.8.1/msvc2022_64 ^
  -DKILLENGINE_BUILD_TESTS=ON

# Construire
cmake --build build --config Release

# Lancer les tests
ctest --test-dir build --output-on-failure
```

## Structure du dépôt

```
KillEngine/
├── apps/desktop/     # Application Qt (main.cpp, controller)
├── core/             # KillCore — moteur C++20 (process, memory, scanner...)
├── ai/               # KillAI — couche IA (llama.cpp, tool calling)
├── ui/               # Frontend Vue 3 + TypeScript
├── tests/            # Tests unitaires et d'intégration (Google Test)
├── docs/             # Documentation
├── scripts/          # Scripts de build et configuration
├── model/            # IA embarquées: agents + poids GGUF partagés
├── packaging/        # Scripts d'installation (Inno Setup)
└── third_party/      # Dépendances tierces
```

## Licence

MIT — voir [LICENSE](LICENSE)

## Les quatre vues

KillEngine s'articule autour de quatre vues, du plus guidé au plus manuel :

| Vue | Rôle | Pour qui |
| --- | --- | --- |
| **Assistant** | Langage naturel : tu décris la valeur, KillEngine choisit et enchaîne les scans safe | Flux recommandé, débutant comme expert |
| **Investigation** | Timeline de l'enquête : stratégie choisie, étapes exécutées, hypothèses, confiance, checkpoints à confirmer | Comprendre *pourquoi* l'IA a fait ce qu'elle a fait |
| **Expert** | Outils manuels : scans exact/unknown/chiffré, Trace UI string, pointer scanner, write/freeze, find what writes, AOB, patches | Reprendre la main sur chaque étape |
| **Trainer** | Features nommées, toggles ON/OFF, apply all / restore all, rollback, persistance par profil | Le résultat réutilisable |

Détail de la direction produit dans [`docs/ULTIMATE_PRODUCT_GUIDELINE.md`](docs/ULTIMATE_PRODUCT_GUIDELINE.md).

## Statut

Prototype avancé, en cours de montée en gamme produit :

- Phases 0 à 12 validées, checklist V1 complète.
- Phases 13 à 18 : scalabilité, profils, pointer chains, freeze, hardware breakpoints, AOB → patch → trainer.
- Smart Search opérationnel : scan guidé, next scan, auto-write confirmé/restauré/vérifié, rollback batch.
- Profils opérationnels : plusieurs cibles, résolution d'adresses, patches de code persistés, réutilisation par l'Assistant.
- Gros volumes : candidats stockés en fichier temporaire compact, next scan streaming, métriques perf et restauration de réduction.
- IA locale embarquée : tool-calls JSON, contrat d'intention structuré, agents sous `model/<nom_ia>/`, état dégradé si modèle/runtime manquant.
- Phases ultimes (U1 Assistant proactif, U2 Investigation, U3 Trainer Builder) : livrées au niveau workflow, validation terrain en cours.
- Guide utilisateur dans [`docs/USER_GUIDE.md`](docs/USER_GUIDE.md), avancement détaillé dans [`docs/PHASE_TRACKER.md`](docs/PHASE_TRACKER.md).

Pour l'état des tests, se référer à `docs/PHASE_TRACKER.md` plutôt qu'à ce README : les compteurs figés se périment vite.

Voir [`KILLENGINE_PROJECT_SPEC.md`](KILLENGINE_PROJECT_SPEC.md) pour le cahier des charges complet.

## Contribution

Trois agents IA (Codex, Cline + z.ai, Claude) contribuent en parallèle à ce dépôt.
Lire [`AGENTS.md`](AGENTS.md) — section « Équipe d'agents » — avant toute modification. Ce même fichier contient en tête une carte de tous les `.md` du dépôt (qui documente quoi, quand le lire) ; les hypothèses/pistes en cours de réflexion vivent dans [`docs/STRATEGY_ROOM.md`](docs/STRATEGY_ROOM.md).
