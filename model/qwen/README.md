# Modèles IA KillEngine

Ce dossier contient les modèles GGUF utilisés par KillEngine.

## Modèle standard V1

- **Modèle**: Qwen3.5-2B
- **Quantification**: GGUF Q4_K_M
- **Taille**: ~1.27 Go
- **Licence**: Apache-2.0

## Génération du modèle

Le projet produira sa propre quantification reproductible (Section 80 du cahier des charges).

Ne pas télécharger de modèles non contrôlés depuis des sources communautaires.

Les fichiers `.gguf` et `.bin` sont exclus du contrôle de version (voir `.gitignore`).

## Installation

Le modèle sera téléchargeable via le Model Manager intégré (Phase 12) ou installable manuellement dans ce dossier.

Phase 9 peut déjà charger un modèle local si :

- `llama-cli.exe` est disponible à côté de `KillEngine.exe`, dans `third_party/llama.cpp/build/bin/...`, ou via `KILLENGINE_LLAMA_CLI`.
- le modele GGUF est disponible sous `model/qwen/*.gguf`, ou via `KILLENGINE_QWEN_GGUF` pour les tests avances.

Si l'un des deux manque, KillEngine garde le planner déterministe local comme fallback.
