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
├── models/           # Modèles GGUF (non inclus dans git)
├── packaging/        # Scripts d'installation (Inno Setup)
└── third_party/      # Dépendances tierces
```

## Licence

MIT — voir [LICENSE](LICENSE)

## Statut

Prototype avancé :

- Phases 0 à 11 validées au niveau prototype.
- Phase 12 / Polissage V1 en cours.
- Smart Search opérationnel : scan guidé, next scan, auto-write vérifié, rollback batch.
- Profils opérationnels : plusieurs cibles, résolution d'adresses, réutilisation par l'Assistant.
- IA locale optionnelle : tool-calls JSON, contrat d'intention structuré, fallback déterministe.
- Tests unitaires : 24/24 au dernier état connu.

Reste principalement : Mode Expert, Paramètres, export logs/debug, packaging, polish async/progression, documentation utilisateur.

Voir `KILLENGINE_PROJECT_SPEC.md` pour le cahier des charges complet.
