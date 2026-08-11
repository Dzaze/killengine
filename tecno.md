# Technologies et outils de KillEngine

Fiche courte des technologies utilisees dans le projet et de leur role.

| Technologie / outil | Role dans le projet |
| --- | --- |
| C++20 | Langage principal du backend, du moteur de scan memoire et de l'application desktop. |
| MSVC / Visual Studio Build Tools | Compilateur C++ utilise pour produire les executables Windows. |
| CMake | Configure le projet, trouve les dependances et genere les fichiers de build. |
| Ninja | Execute le build genere par CMake, plus rapidement que les generateurs classiques. |
| Qt 6.8 | Framework desktop C++ utilise pour la fenetre, les signaux/slots et l'integration UI. |
| Qt WebEngine | Affiche l'interface web Vue dans l'application desktop. |
| Qt WebChannel | Permet la communication entre le backend C++ et le frontend Vue. |
| Vue 3 | Framework frontend utilise pour construire l'interface utilisateur. |
| TypeScript | JavaScript type, utilise pour securiser le code frontend et les appels backend. |
| Vite | Build l'interface Vue dans `ui/dist/`. |
| Pinia | Stocke l'etat global de l'interface Vue. |
| llama.cpp / llama-cli | Runtime IA local qui execute le modele GGUF sur la machine. |
| Qwen3.5-2B GGUF | Modele IA local utilise pour comprendre la demande utilisateur et proposer une action. |
| Google Test | Framework de tests unitaires et d'integration C++. |
| LZ4 | Compression rapide des snapshots memoire. |
| nlohmann/json | Lecture et ecriture de donnees JSON en C++. |
| PowerShell | Scripts de configuration, build et packaging Windows. |
| Windows 64-bit | Plateforme cible, necessaire pour l'analyse et l'ecriture memoire de processus Windows. |

## Fonctionnement de l'IA

L'IA est separee dans le module `ai/`.

- `AIEngine` recoit la demande utilisateur.
- Si le modele Qwen GGUF est disponible, il peut appeler `llama-cli` via `llama.cpp`.
- Le modele ne modifie pas directement la memoire: il produit une intention structuree.
- `intent_contract` valide cette intention pour eviter les actions mal formees.
- `tool_registry` liste les actions autorisees que l'IA peut demander.
- Si le runtime IA n'est pas disponible, un fallback deterministe peut repondre sans modele local.

En simplifie: Qwen comprend la demande, `llama.cpp` execute le modele, l'IA propose une action, le contrat la verifie, puis KillEngine decide si elle peut etre executee.
