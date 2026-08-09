# KillEngine — Cahier des charges technique et plan de fabrication

> **Version du document :** 0.1 — architecture fondatrice  
> **Cible :** Windows 10/11 x64  
> **Nature du projet :** outil local d’analyse et d’édition mémoire assisté par IA  
> **Principe directeur :** *l’utilisateur donne une valeur ou décrit ce qu’il cherche ; KillEngine choisit et exécute la stratégie de recherche à sa place.*

---

## 0. Baseline actuelle du prototype

Dernière mise à jour : phases 9, 10 et 11 validées au niveau prototype ; Phase 12 / Polissage V1 complète au niveau checklist V1 ; Phase 13 ouverte comme baseline d'améliorations.

### État validé

- Application Windows Qt 6 + Vue 3 compilée en `KillEngine.exe`.
- Connexion UI/backend via QWebChannel opérationnelle.
- Énumération des processus Windows avec filtre UI et attachement au processus.
- Carte mémoire via `VirtualQueryEx()` avec statistiques de régions.
- Lecture mémoire sûre par chunks.
- Scan exact `Int32`, `Int64`, `Float32`, `Float64`.
- CandidateStore avec tri, filtrage et pagination.
- Next scan : `exact`, `changed`, `unchanged`, `increased`, `decreased`, `delta`.
- Unknown initial value : capture snapshot compressé LZ4, stockage memory-mapped temporaire, puis comparaison.
- Watch/read preview, écriture mémoire vérifiée, rollback simple, freeze simple.
- Assistant Smart Search connecté aux outils déterministes et au runtime IA local optionnel.
- Contrat d'intention IA structuré : le modèle peut produire une intention JSON validée avant exécution, avec fallback déterministe si le modèle est absent ou imprécis.
- Debug Assistant : les décisions de Smart Search et les intents sont visibles pour diagnostiquer les cas où la conversation choisit la mauvaise action.
- Workflow guidé validé sur Microsoft Solitaire :
  - l'utilisateur décrit une valeur actuelle et une valeur cible ;
  - KillEngine lance le scan exact ;
  - l'utilisateur fait varier la valeur dans le jeu ;
  - l'utilisateur donne la nouvelle valeur dans le chat ;
  - KillEngine réduit les candidats ;
  - si 1 à 4 candidats restent, KillEngine confirme les adresses par écriture temporaire vérifiée, restaure l'ancienne valeur, puis écrit automatiquement la valeur cible.
- L'affichage Assistant montre un message humain plutôt que le JSON complet de debug.
- Les adresses mémoire données dans la conversation peuvent être sélectionnées et réutilisées pour des écritures successives.
- Les dernières adresses auto-écrites restent disponibles pour les demandes suivantes du type "passe-les à 2000".
- Les profils peuvent contenir plusieurs cibles et les cibles résolues peuvent être utilisées par l'Assistant.
- Une nouvelle recherche explicite remet de côté les adresses actives de conversation/profil avant de scanner ailleurs.
- La page Paramètres sauvegarde des options persistantes : langue, type par défaut, limites de scan, fast scan, debug Smart Search et placeholders IA.
- Le packaging Windows initial produit un zip portable et fournit un template Inno Setup pour l'installateur.
- La page Paramètres permet de lire les dernières lignes du log principal et d'exporter un bundle diagnostic compressé.
- Les crashs non gérés produisent un rapport local et les rapports récents sont inclus dans l'export diagnostic.
- Le Mode Expert affiche un état de scan en cours, bloque les actions concurrentes et expose une progression simple.
- Les scans exacts, `next_scan` et `unknown` du Mode Expert peuvent partir dans un worker thread et rendre le contrôle au frontend immédiatement.
- Le Mode Expert expose une annulation de scan actif ; une annulation ne remplace pas l'état candidat avec des résultats partiels.
- La capture `unknown` transfère le snapshot memory-mapped au thread UI seulement si elle aboutit sans annulation.
- Un guide utilisateur V1 existe dans `docs/USER_GUIDE.md`.
- Une Phase 13 de suivi d'améliorations est ouverte dans `docs/PHASE_TRACKER.md` pour traiter précision, faux positifs, Assistant, profils, UX et robustesse.
- Crédit UI : `Pirolley Benoist` en bas à gauche.

### Limites connues de la baseline

- Phase 12 / Polissage V1 est complète au niveau checklist ; il reste à faire une passe de régression manuelle avant une release candidate.
- Le moteur IA tente maintenant d'utiliser un runtime local `llama-cli` + GGUF Qwen si disponibles, puis retombe sur le planner déterministe si le modèle ou l'exécutable manque.
- Le scan multi-type automatique complet n'est pas encore implémenté : le planner choisit surtout `Int32` par défaut sauf indication contraire.
- Le CandidateStore bascule automatiquement sur fichier temporaire compact au-delà d'un seuil, sait paginer sans hydrater toute la liste, et le `next_scan` async peut lire/écrire les candidats en streaming.
- Une réduction de candidats peut être restaurée depuis le Mode Expert via `Restaurer réduction`, y compris quand la génération précédente était stockée en fichier temporaire.
- Les résultats de scan exposent des métriques de performance V1 : temps écoulé, débits, stockage fichier et RAM estimée.
- L'auto-write Assistant passe par une confirmation temporaire vérifiée puis restauration avant l'écriture finale, afin de réduire les faux positifs d'adresses non fiables.
- Les candidats survivants conservent un historique borné des valeurs observées pendant les `next_scan` guidés ; cet historique est rattaché aux suggestions et aux cartes d'auto-write.
- Avant l'auto-write issu d'un scan guidé, les candidats sont filtrés par région mémoire : readable, writable, non guarded, et type de région `Private` ou `Mapped` ; les rejets restent visibles dans le chat.
- Quand une comparaison élimine tous les candidats, le résultat inclut un diagnostic lisible : volume comparé, adresses illisibles si connues, exemples de valeurs observées, et rappel de restauration de réduction.
- L'Assistant expose un contexte structuré visible : recherche initiale, cible, type, candidats courants, adresses actives, cibles de profil actives et réduction restaurable.
- L'Assistant reconnaît explicitement les intentions `nouvelle recherche`, `réutiliser adresses`, `changer valeur trouvée` et `oublier adresses/profils actifs`.
- Le rollback batch restaure maintenant toutes les écritures du dernier auto-write (jusqu'à 4 adresses) ; le rollback simple restaure la dernière uniquement.
- Les snapshots unknown sont maintenant compressés LZ4 par région et stockés dans un fichier temporaire memory-mapped ; la comparaison décompresse les régions à la volée.
- La Phase 11 Profils est implémentée au niveau prototype : sauvegarde/chargement/résolution de cibles multiples par locator module_offset ou adresse absolue.
- Le Mode Expert est implémenté au niveau V1 polish initial avec filtres d'adresse, alignement et protections mémoire.
- Les Paramètres sont implémentés au niveau V1 polish initial ; le Model Manager complet reste à brancher sur le chemin modèle sauvegardé.
- Le packaging est implémenté au niveau V1 polish initial ; la signature Authenticode et l'installateur final signé restent hors de cette brique.
- L'export diagnostic initial est disponible ; l'ouverture automatique du dossier exporté reste à ajouter si nécessaire.
- Le crash handling initial écrit des rapports texte locaux ; les minidumps natifs restent une amélioration ultérieure.
- La progression de scan V1 est volontairement grossière ; l'annulation est disponible mais ne fournit pas encore une progression fine par région.
- `KillEngineTestTarget.exe` est créé et exposé des variables connues (health, money, stamina, position, hidden_score, Player heap) pour les validations manuelles et scénarios automatisés.
- Les validations sont surtout manuelles sur Solitaire et via tests unitaires ; les tests d'intégration automatisés (`tests/integration/`) restent à écrire.
- Les opérations de scan lourdes restent à améliorer côté asynchronisme/progression fine pour une UX V1 complète.

### Pré-requis avant Phase 12

La Phase 12 est débloquée parce que les points suivants sont terminés ou explicitement sortis du périmètre V1 :

- Phase 9 : validation avec appel de scan généré par le modèle.
- Revalidation du workflow guidé Phase 10 après ces décisions.
- Revalidation du workflow profils Phase 11 après ces décisions.

### Baseline de build

Commandes de validation :

```powershell
.\scripts\build_ui.ps1
.\scripts\build.ps1
ctest --test-dir build --output-on-failure
```

Dernier état connu :

```text
UI build OK
C++ build OK
Tests unitaires OK : 30/30
KillEngineTestTarget.exe build OK
```

---

## 1. Vision du projet

**KillEngine** est un analyseur de mémoire Windows inspiré du principe de fonctionnement des outils de type Cheat Engine, mais conçu autour d’une différence fondamentale :

> **l’utilisateur ne doit pas connaître les types de données, les scans, l’alignement, les pointeurs ou la structure mémoire pour trouver une valeur.**

L’utilisateur doit pouvoir écrire simplement :

```text
Je cherche l'argent : 41 250
```

ou même :

```text
41250
```

KillEngine doit alors :

1. identifier le processus du jeu ;
2. analyser sa carte mémoire ;
3. choisir automatiquement les types numériques pertinents ;
4. lancer plusieurs recherches adaptées ;
5. regrouper et classer les résultats ;
6. demander uniquement une action simple dans le jeu si une deuxième observation est mathématiquement nécessaire ;
7. observer l’évolution des candidats ;
8. éliminer les faux positifs ;
9. calculer un niveau de confiance ;
10. proposer la valeur la plus probable ;
11. permettre de la modifier ou de la figer ;
12. mémoriser ce qui a été découvert pour les prochaines sessions.

Le mode normal de KillEngine doit donc masquer presque toute la complexité technique d’un scanner mémoire traditionnel.

---

# 2. Philosophie produit

## 2.1 Règle principale

**L’utilisateur décrit le résultat qu’il souhaite ; KillEngine choisit la méthode.**

L’interface normale ne demande pas :

- `4 Bytes` ;
- `Float` ;
- `Exact Value` ;
- `Unknown Initial Value` ;
- `First Scan` ;
- `Next Scan` ;
- `Fast Scan` ;
- `Alignment` ;
- plage d’adresses ;
- type de région mémoire.

Ces réglages existent uniquement dans un **mode Expert**.

---

## 2.2 Expérience utilisateur cible

### Exemple : recherche d’argent

Utilisateur :

```text
Argent : 41250
```

KillEngine :

```text
Recherche de la valeur 41 250 dans Imperialism.exe…

Hypothèses testées automatiquement :
✓ entier 32 bits
✓ entier 64 bits
✓ flottant
✓ double
✓ variantes signées/non signées
✓ représentations simples mises à l’échelle

1 842 candidats cohérents.
Fais varier ton argent dans le jeu puis clique sur « J’ai changé ».
```

L’utilisateur dépense de l’argent puis clique :

```text
[J’ai changé]
```

KillEngine reprend automatiquement un instantané des candidats :

```text
Analyse des changements…

1 842 → 37 → 5 candidats

J’observe encore quelques instants les 5 candidats.
```

Puis :

```text
Valeur probable trouvée

Adresse actuelle : 0x000001D4…
Type : Int32
Valeur : 39 750
Confiance : 97 %

[Modifier] [Figer] [Ajouter au profil]
```

L’utilisateur n’a effectué aucun `First Scan` ou `Next Scan` manuellement.

---

# 3. Objectif fonctionnel

KillEngine doit devenir progressivement un système en trois niveaux.

## Niveau 1 — Scanner intelligent

Il automatise les recherches mémoire classiques.

## Niveau 2 — Analyste mémoire

Il comprend la relation entre les valeurs, les adresses, les modules, les changements et les copies d’affichage.

## Niveau 3 — Assistant de rétro-ingénierie mémoire

Il peut :

- retrouver automatiquement une valeur après redémarrage ;
- reconnaître une structure ;
- retrouver un offset ;
- rechercher un pointeur stable ;
- trouver le code qui lit ou écrit une valeur ;
- produire un profil réutilisable pour un jeu.

---

# 4. Périmètre initial

## KillEngine V1 doit faire parfaitement

- lister les processus Windows pertinents ;
- sélectionner un processus ;
- détecter x86 / x64 ;
- obtenir la carte mémoire ;
- lire les régions mémoire accessibles ;
- effectuer des scans rapides multithread ;
- rechercher plusieurs types simultanément ;
- rechercher une valeur exacte ;
- effectuer des scans successifs ;
- rechercher une valeur inconnue ;
- filtrer :
  - augmentée ;
  - diminuée ;
  - changée ;
  - inchangée ;
  - supérieure ;
  - inférieure ;
  - comprise entre deux valeurs ;
  - modifiée d’un delta donné ;
- suivre des candidats en temps réel ;
- lire une adresse ;
- écrire une valeur ;
- vérifier l’écriture par relecture ;
- restaurer la valeur précédente ;
- figer une valeur ;
- enregistrer un profil local ;
- utiliser une IA locale pour orchestrer tout le processus.

---

## Hors périmètre V1

Ne pas commencer par :

- driver noyau ;
- hyperviseur ;
- contournement d’anti-cheat ;
- dissimulation du processus ;
- injection furtive ;
- bypass de protections ;
- modification de jeux compétitifs en ligne ;
- scan réseau ;
- débogueur kernel.

Ces fonctions complexifieraient énormément le projet sans être nécessaires pour valider le concept.

La V1 est volontairement **user-mode Windows**, orientée jeux locaux / solo / tests sur des processus appartenant à l’utilisateur.

---

# 5. Architecture générale retenue

```text
┌──────────────────────────────────────────────────────────┐
│                     KillEngine.exe                       │
│                                                          │
│   Qt 6 + Qt WebEngine                                    │
│   Vue 3 + TypeScript                                     │
│                                                          │
│   ┌──────────────────────────────────────────────────┐   │
│   │ Interface utilisateur                           │   │
│   │                                                  │   │
│   │ • Assistant                                     │   │
│   │ • Processus                                     │   │
│   │ • Résultats                                     │   │
│   │ • Mémoire                                       │   │
│   │ • Profils                                       │   │
│   │ • Mode Expert                                   │   │
│   └──────────────────────────────────────────────────┘   │
│                         │                                │
│                    QWebChannel                           │
│                         │                                │
│   ┌─────────────────────▼────────────────────────────┐   │
│   │              Application Controller              │   │
│   └──────────────┬───────────────────┬───────────────┘   │
│                  │                   │                   │
│           ┌──────▼──────┐     ┌──────▼──────┐            │
│           │ KillCore    │     │ KillAI      │            │
│           │ C++20       │     │ llama.cpp   │            │
│           └──────┬──────┘     └──────┬──────┘            │
│                  │                   │                   │
└──────────────────┼───────────────────┼───────────────────┘
                   │                   │
                   ▼                   ▼
             Win32 Memory API     Qwen3.5-2B
```

---

# 6. Choix technologiques

## 6.1 Langage du moteur mémoire

### Choix : C++20

Le scanner mémoire est la partie la plus sensible aux performances.

Il doit pouvoir :

- parcourir plusieurs gigaoctets ;
- copier de gros blocs ;
- comparer des millions de valeurs ;
- utiliser plusieurs threads ;
- manipuler directement les API Win32 ;
- conserver une consommation mémoire maîtrisée.

Le moteur principal ne doit donc **pas dépendre de Python au runtime**.

Python peut être utilisé pour :

- scripts de développement ;
- génération de jeux de tests ;
- benchmarks ;
- analyse de logs ;
- prototypes.

Mais le produit final doit fonctionner sans installation Python.

---

## 6.2 Interface

### Choix

```text
Qt 6
Qt WebEngine
QWebChannel
Vue 3
Vite
TypeScript
Pinia
```

### Pourquoi

Cette architecture permet :

- une vraie application `.exe` Windows ;
- une interface moderne ;
- un développement UI très rapide ;
- des animations et composants web ;
- un backend C++ natif ;
- aucune dépendance Node.js sur le PC de l’utilisateur après compilation.

Vue est compilé lors du build et devient un ensemble de ressources statiques embarquées.

---

## 6.3 Build

```text
CMake
Ninja
MSVC x64
Node.js uniquement au build de l'UI
```

Compilation cible :

```text
Release x64
Windows 10 22H2+
Windows 11
```

Le support x86 concerne les **processus analysés**, pas l’application principale.

KillEngine lui-même reste x64.

---

# 7. API Windows utilisées

Le cœur V1 repose principalement sur les API documentées de Windows.

## Processus

```cpp
OpenProcess()
CloseHandle()
```

## Carte mémoire

```cpp
VirtualQueryEx()
GetSystemInfo()
```

## Lecture

```cpp
ReadProcessMemory()
```

## Écriture

```cpp
WriteProcessMemory()
```

## Architecture du processus

```cpp
IsWow64Process2()
```

## Modules / processus

Selon le besoin :

```cpp
CreateToolhelp32Snapshot()
Process32First()
Process32Next()
Module32First()
Module32Next()
```

---

# 8. Gestion des droits

KillEngine doit demander uniquement les droits nécessaires.

## Recherche seule

```text
PROCESS_QUERY_INFORMATION
PROCESS_VM_READ
```

## Modification

Ajouter seulement lorsque nécessaire :

```text
PROCESS_VM_WRITE
PROCESS_VM_OPERATION
```

L’application ne doit pas demander automatiquement `PROCESS_ALL_ACCESS`.

Si l’ouverture échoue :

```text
Accès insuffisant au processus.

[Relancer KillEngine en administrateur]
[Annuler]
```

---

# 9. Architecture mémoire Windows

KillEngine ne doit jamais considérer l’espace virtuel d’un processus comme un bloc continu.

Il doit d’abord appeler `VirtualQueryEx()` pour obtenir des régions homogènes.

Pour chaque région :

```text
BaseAddress
AllocationBase
RegionSize
State
Protect
Type
```

---

## Régions V1 à scanner

Par défaut :

```text
MEM_COMMIT
```

Et régions lisibles :

```text
PAGE_READONLY
PAGE_READWRITE
PAGE_WRITECOPY
PAGE_EXECUTE_READ
PAGE_EXECUTE_READWRITE
PAGE_EXECUTE_WRITECOPY
```

À exclure par défaut :

```text
MEM_FREE
MEM_RESERVE
PAGE_NOACCESS
PAGE_GUARD
```

Le mode Expert pourra modifier ces filtres.

---

# 10. Lecture RAM performante

## Mauvaise méthode

```text
ReadProcessMemory()
pour chaque adresse
```

Cela provoquerait beaucoup trop d’appels système.

## Méthode KillEngine

```text
VirtualQueryEx
      │
      ▼
Région lisible
      │
      ▼
Découpage en blocs
      │
      ▼
ReadProcessMemory
      │
      ▼
Buffer local
      │
      ▼
Scan CPU multithread
```

Taille initiale recommandée des blocs :

```text
1 à 8 Mio
```

La valeur optimale sera déterminée par benchmark.

---

# 11. Scanner multithread

Le scanner doit utiliser un pool de threads.

Principe :

```text
MemoryRegion
     │
     ├── Chunk A ── Worker 1
     ├── Chunk B ── Worker 2
     ├── Chunk C ── Worker 3
     └── Chunk D ── Worker 4
```

Le nombre de workers doit être dynamique.

Par défaut :

```text
min(nombre_de_cœurs_logiques - 1, limite_configurée)
```

KillEngine doit laisser assez de ressources au jeu pour rester utilisable pendant l’analyse.

Le scan doit être :

- annulable ;
- suspendable ;
- mesurable ;
- progressif ;
- thread-safe.

Utiliser lorsque possible :

```cpp
std::jthread
std::stop_token
```

---

# 12. Types de valeurs pris en charge

## Entiers

```text
Int8
UInt8

Int16
UInt16

Int32
UInt32

Int64
UInt64
```

## Flottants

```text
Float32
Float64
```

## Texte

```text
ASCII / UTF-8
UTF-16LE
```

## Octets

```text
Array of Bytes
AOB avec wildcards
```

Exemple :

```text
48 8B ?? ?? 89 43 18
```

---

# 13. Alignement

KillEngine doit pouvoir rechercher selon :

```text
alignement 1
alignement 2
alignement 4
alignement 8
```

Mais le mode IA choisit lui-même.

Exemple :

```text
41250
```

Le moteur commence généralement par :

```text
Int32 align 4
Int64 align 8
Float32 align 4
Float64 align 8
```

Puis élargit si nécessaire.

---

# 14. First Scan

Lors d’une recherche exacte :

```text
RAM lisible
   │
   ▼
scan(value = 41250)
   │
   ▼
CandidateStore
```

Le scanner ne conserve que les adresses pertinentes.

Exemple :

```text
5,2 Go analysés
12 413 occurrences
```

Les résultats ne sont pas envoyés un par un à l’IA.

---

# 15. CandidateStore

Un LLM ne doit jamais recevoir des millions d’adresses.

Le moteur doit posséder son propre stockage compact.

Structure logique :

```cpp
Candidate
{
    address;
    value_type;
    current_value;
    previous_value;
    region_id;
    module_id;
    flags;
    confidence_features;
}
```

En pratique, pour plusieurs millions de résultats, une structure compacte en colonnes est préférable à une grosse structure C++ par entrée.

Exemple :

```text
addresses.bin
types.bin
region_ids.bin
state.bin
```

ou stockage mémoire mappé.

---

# 16. Unknown Initial Value

La recherche inconnue nécessite une stratégie différente.

Au premier scan :

```text
RAM
 │
 ▼
Baseline Snapshot
```

Au scan suivant :

```text
Baseline
   │
   ├── comparaison
   │
Current RAM
   │
   ▼
Candidates
```

Exemples de filtres :

```text
changed
unchanged
increased
decreased
```

---

# 17. Stockage des snapshots

Ne pas stocker naïvement plusieurs copies complètes de plusieurs gigaoctets en RAM.

Architecture :

```text
%LOCALAPPDATA%\KillEngine\Sessions\<session-id>\
```

avec :

```text
regions.map
baseline.kesnap
candidates.kec
session.json
```

Les snapshots peuvent utiliser :

- fichiers mémoire mappés ;
- compression rapide LZ4 par blocs ;
- index de régions ;
- bitsets de candidats.

LZ4 est adapté aux données temporaires où la vitesse est plus importante que le taux de compression maximal.

---

# 18. Next Scan

Le prochain scan ne repart pas de zéro.

Il travaille sur le jeu de candidats précédent.

```text
12 413 candidats
      │
      ▼
nouvelle observation
      │
      ▼
427 candidats
      │
      ▼
nouvelle observation
      │
      ▼
8 candidats
```

Filtres :

```text
Exact
Changed
Unchanged
Increased
Decreased
GreaterThan
LessThan
Between
ChangedBy
IncreasedBy
DecreasedBy
```

---

# 19. Gestion des floats

Les floats ne doivent pas être comparés naïvement avec `==` dans tous les cas.

Prévoir :

```text
Exact bits
Exact displayed value
Tolerance
Rounded displayed value
```

Exemple :

```text
100 affiché
```

peut correspondre à :

```text
99.999992
100.00001
```

Le moteur IA peut essayer une tolérance automatiquement.

---

# 20. Recherche multi-type automatique

C’est l’une des fonctions centrales de KillEngine.

Si l’utilisateur entre :

```text
41250
```

le moteur ne doit pas faire un seul scan.

Il crée un **ScanBundle** :

```text
ScanBundle #42

Int32
UInt32
Int64
UInt64
Float32
Float64
```

Les résultats sont fusionnés dans une vue unique.

L’utilisateur ne voit pas la complexité.

---

# 21. Hypothèses de représentation

Après les représentations directes, l’IA peut tester automatiquement des variantes simples.

Exemple :

```text
Valeur affichée : 412.50
```

Possibilités :

```text
Float32  = 412.5
Float64  = 412.5
Int32    = 41250
Int64    = 41250
```

Pourcentage :

```text
75 %
```

peut être :

```text
75
0.75
7500
```

Ces hypothèses doivent être **bornées et explicables**.

Ne jamais lancer arbitrairement des milliers de transformations inventées par le LLM.

---

# 22. Rôle exact de l’IA

L’IA n’effectue pas directement les opérations mémoire.

Elle joue le rôle de :

```text
Planner
+
Analyst
+
Decision Engine
+
Assistant utilisateur
```

Le moteur C++ reste l’autorité technique.

---

# 23. Séparation déterminisme / IA

Architecture impérative :

```text
Utilisateur
    │
    ▼
LLM
    │
    ▼
Plan d'action structuré
    │
    ▼
Validator
    │
    ▼
KillCore
    │
    ▼
Résultats déterministes
    │
    ▼
Evidence Summarizer
    │
    ▼
LLM
    │
    ▼
Décision suivante
```

Le modèle ne doit jamais appeler directement :

```text
ReadProcessMemory
WriteProcessMemory
OpenProcess
```

Il appelle des outils KillEngine de haut niveau.

---

# 24. Modèle IA retenu

## Modèle standard

```text
Qwen3.5-2B
```

### Raisons

- modèle compact ;
- 2 milliards de paramètres ;
- licence Apache-2.0 ;
- capacités d’instruction ;
- raisonnement ;
- capacités agent/outils ;
- multilingue ;
- français correctement géré ;
- famille disponible en exécution locale ;
- modèle assez petit pour un logiciel Windows grand public ;
- architecture Qwen3.5 également multimodale, ce qui ouvre la voie à l’analyse future de captures d’écran.

---

## Quantification retenue

```text
GGUF
Q4_K_M
```

Ordre de grandeur constaté pour une quantification Q4_K_M de Qwen3.5-2B :

```text
≈ 1,27 Go
```

Le projet devra produire et tester sa **propre quantification reproductible** à partir du modèle officiel au lieu de dépendre d’un fichier communautaire non contrôlé.

---

# 25. Runtime IA

## Choix : llama.cpp

Pourquoi :

- C/C++ ;
- intégrable directement dans une application native ;
- fonctionne en CPU ;
- supporte AVX / AVX2 / AVX512 selon matériel ;
- quantification GGUF ;
- CUDA ;
- HIP ;
- Vulkan ;
- SYCL ;
- exécution hybride CPU/GPU ;
- serveur OpenAI-compatible disponible pour le développement ;
- grammars disponibles pour contraindre la sortie.

En production, KillEngine doit préférer **l’intégration directe de libllama** plutôt qu’un serveur HTTP local visible.

---

# 26. Accélération matérielle

Ordre de priorité :

## Mode universel

```text
CPU
```

## GPU générique Windows

```text
Vulkan
```

## NVIDIA optimisé

```text
CUDA
```

## AMD avancé

```text
HIP
```

## Intel

Possibilité future :

```text
SYCL / OpenVINO selon maturité
```

KillEngine détecte le matériel au lancement et choisit un backend.

L’utilisateur peut forcer un backend dans les paramètres.

---

# 27. Taille de contexte

Le modèle officiel accepte un contexte très long.

KillEngine n’en a pas besoin.

La V1 doit viser :

```text
8192 tokens
```

avec possibilité :

```text
16384 tokens
```

Le contexte ne contient jamais les millions de résultats.

Il contient seulement :

- objectif utilisateur ;
- historique court ;
- résumé du processus ;
- résumé des scans ;
- statistiques ;
- top candidats ;
- décisions précédentes.

Cette limitation réduit fortement l’usage RAM et accélère l’inférence.

---

# 28. Modes IA

## Standard

```text
Qwen3.5-2B Q4_K_M
```

C’est le modèle officiel de référence du projet.

## Lite — option future

```text
Qwen3.5-0.8B
```

Pour machines très modestes.

Fonctions IA plus simples.

## Enhanced — option future

```text
Qwen3.5-4B
```

Pour machines disposant de plus de RAM/GPU.

La logique du logiciel doit rester identique quel que soit le modèle.

---

# 29. Pas de fine-tuning au début

La première version ne doit **pas** commencer par entraîner un modèle.

On utilise :

```text
Qwen3.5-2B
+
prompt système
+
tool calling
+
heuristiques
+
machine à états
```

Le fine-tuning ne sera envisagé qu’après avoir accumulé des sessions correctement étiquetées.

Sinon on entraînerait un modèle sans dataset fiable.

---

# 30. Tool Calling KillEngine

Le LLM possède une liste fermée d’actions.

Exemple conceptuel :

```json
{
  "action": "scan_exact",
  "arguments": {
    "value": 41250,
    "types": ["int32", "uint32", "int64", "float32", "float64"]
  }
}
```

Actions V1 :

```text
get_processes
select_process
get_process_info

scan_exact
scan_unknown
scan_changed
scan_unchanged
scan_increased
scan_decreased
scan_between
scan_delta

get_candidate_stats
get_top_candidates
watch_candidates

read_value
write_value
restore_value
freeze_value
unfreeze_value

save_target
save_profile
```

---

# 31. JSON strict

Les décisions IA doivent être produites dans un format strict.

Exemple :

```json
{
  "intent": "find_value",
  "target": {
    "label": "argent",
    "display_value": 41250
  },
  "next_action": {
    "tool": "scan_exact",
    "arguments": {
      "strategy": "numeric_bundle"
    }
  },
  "user_message": "Je recherche l'argent."
}
```

Le backend :

1. parse ;
2. valide le schéma ;
3. refuse les actions inconnues ;
4. refuse les paramètres dangereux ;
5. exécute uniquement une commande autorisée.

---

# 32. Prompt système du modèle

Le prompt système doit être court et spécialisé.

Exemple de philosophie :

```text
Tu es l'analyste mémoire de KillEngine.

Ton objectif est d'identifier la donnée demandée par l'utilisateur
en utilisant uniquement les outils fournis.

Règles :
- n'invente jamais une adresse ;
- n'invente jamais un résultat de scan ;
- privilégie la méthode qui réduit le plus les candidats ;
- ne demande jamais à l'utilisateur de choisir un type mémoire
  si KillEngine peut le déterminer ;
- si une observation supplémentaire est nécessaire, demande une seule
  action concrète dans le jeu ;
- n'écris jamais en mémoire sans intention explicite de modification ;
- fonde toute confiance sur les données renvoyées par KillCore.
```

---

# 33. AI Scan Planner

Le planner reçoit :

```text
label
valeur
type d'application
architecture
historique
nombre de candidats
```

Exemple :

```text
label = argent
value = 41250
```

Plan initial :

```text
1. Int32 / UInt32
2. Int64 / UInt64
3. Float32
4. Float64
5. alignements naturels
```

Si aucun résultat :

```text
6. alignement 1
7. transformations simples
8. recherche inconnue assistée
```

---

# 34. Machine à états

L’IA ne doit pas improviser tout le workflow.

États V1 :

```text
IDLE

PROCESS_SELECTED

INITIAL_VALUE_RECEIVED

FIRST_SCAN_RUNNING

CANDIDATES_FOUND

WAITING_FOR_USER_CHANGE

REFINING

WATCHING

TARGET_PROBABLE

TARGET_CONFIRMED

VALUE_EDITED

PROFILE_SAVED
```

Transitions contrôlées par le backend.

---

# 35. Réduction automatique des candidats

Exemple :

```text
Scan 1
41 250

8 942 candidats
```

KillEngine ne présente pas 8 942 lignes.

Il dit :

```text
J'ai plusieurs candidats.
Fais varier cette valeur dans le jeu.
```

Puis capture :

```text
snapshot A
```

L’utilisateur clique :

```text
J'ai changé
```

Capture :

```text
snapshot B
```

Analyse :

```text
unchanged       8 325
changed           617

relation probable :
decreased         124
increased         493
```

La sémantique peut fournir un prior mais jamais une certitude.

---

# 36. Mode « J’ai changé »

Cette fonction est essentielle.

L’utilisateur n’est pas obligé de connaître la nouvelle valeur.

Il peut simplement cliquer :

```text
J'ai changé
```

KillEngine compare l’état des candidats.

Si nécessaire, il demande :

```text
La valeur a :

[Augmenté]
[Diminué]
[Je ne sais pas]
```

Le nombre de choix doit rester minimal.

---

# 37. Mode valeur connue

Encore plus efficace :

```text
Nouvelle valeur : 39750
```

KillEngine effectue automatiquement un `Next Exact`.

Cette information doit toujours avoir priorité sur une simple relation `decreased`.

---

# 38. Observation temporelle

Une fois qu’il ne reste que quelques candidats, KillEngine les surveille.

Exemple :

```text
Address A
41250 → 39750 → 39750 → 39750

Address B
41250 → 39750 → 39750 → 39750

Address C
41250 → 39750 → 39749 → 39748
```

C est probablement :

- compteur ;
- copie temporaire ;
- autre donnée.

Les séries temporelles améliorent la confiance.

---

# 39. Candidate Scoring Engine

Ne pas demander au LLM d’inventer un pourcentage.

Le score est calculé par KillCore.

Exemple de features :

```text
exact_match
delta_match
direction_match
temporal_correlation
stable_read
natural_alignment
writable_region
module_relation
restart_stability
pointer_stability
duplicate_cluster
unexpected_changes
read_errors
```

---

# 40. Exemple de scoring initial

Exemple conceptuel, poids à calibrer :

```text
+30 correspond exactement à la valeur observée
+20 reproduit le bon delta
+15 change uniquement durant l'événement attendu
+10 alignement naturel
+10 région cohérente
+10 relation stable à un module
+15 stable sur plusieurs observations

-20 change hors événement
-20 valeur très volatile
-15 copie manifestement temporaire
-30 erreurs fréquentes de lecture
```

Le score final est normalisé :

```text
0.00 → 1.00
```

Affichage :

```text
97 %
```

Le LLM explique le score ; il ne le produit pas.

---

# 41. Détection des copies

Un jeu peut posséder :

```text
valeur logique
valeur UI
valeur réseau
cache
copie temporaire
```

Plusieurs adresses peuvent donc afficher le même nombre.

KillEngine crée des clusters de comportement.

```text
Cluster 1
A B C
mêmes changements au même moment

Cluster 2
D
mise à jour retardée

Cluster 3
E
volatilité permanente
```

Le moteur conserve plusieurs hypothèses tant qu’il n’a pas assez de preuves.

---

# 42. Écriture mémoire

Une écriture doit suivre :

```text
Read original
      │
      ▼
Validate
      │
      ▼
WriteProcessMemory
      │
      ▼
Read back
      │
      ▼
Compare
```

Si la relecture ne correspond pas :

```text
WRITE_FAILED
```

---

# 43. Rollback

Avant toute première modification d’une cible :

```text
original_value
```

est sauvegardée.

Bouton :

```text
[Restaurer]
```

La session conserve l’historique :

```text
41250
→ 900000
→ 41250
```

---

# 44. Freeze

Le freeze ne doit pas saturer le CPU.

Architecture :

```text
Freeze Manager
     │
     ├ target A
     ├ target B
     └ target C
```

Intervalle configurable.

Valeur par défaut à benchmarker, par exemple :

```text
100 ms
```

Le freeze s’arrête immédiatement si :

- le processus quitte ;
- l’adresse devient invalide ;
- la région change ;
- l’utilisateur désactive la cible.

---

# 45. Memory Watch

Les adresses candidates peuvent être observées sans les modifier.

Affichage :

```text
Nom       Adresse        Type     Valeur
Argent    0x...          int32    39 750
Bois      0x...          int32       125
Fer       0x...          int32        87
```

Fréquence UI recommandée :

```text
5 à 10 Hz
```

Pas besoin de rafraîchir l’interface à plusieurs centaines de Hz.

---

# 46. Sessions

Chaque recherche possède une session.

```text
Session
├ process
├ executable path
├ executable hash
├ architecture
├ start time
├ memory map fingerprint
├ scans
├ candidates
├ watches
└ AI state
```

---

# 47. Profils de jeu

Une découverte validée peut devenir :

```text
Imperialism
```

Profil :

```json
{
  "game": "Imperialism",
  "executable": "imperialism.exe",
  "targets": [
    {
      "name": "Money",
      "type": "int32",
      "locator": {
        "kind": "module_offset",
        "module": "imperialism.exe",
        "offset": "0x..."
      }
    }
  ]
}
```

---

# 48. Module-relative address

Si une adresse se trouve dans un module :

```text
Imperialism.exe + 0x00123456
```

préférer cette notation à :

```text
0x7FF6A123456
```

car l’ASLR peut déplacer le module au prochain démarrage.

---

# 49. Pointer Engine — V2

Lorsque la cible est sur le heap :

```text
adresse dynamique
```

KillEngine V2 cherchera une chaîne stable.

Exemple :

```text
Imperialism.exe + 0x3120
      ↓
   pointer
      + 0x18
      ↓
   pointer
      + 0x28
      ↓
   Money
```

Représentation :

```text
[[Imperialism.exe+0x3120]+0x18]+0x28
```

---

# 50. Pointer Scan intelligent

Au lieu d’exposer des millions de chemins, KillEngine les classe.

Critères :

```text
profondeur
offsets
module racine
stabilité entre relances
stabilité entre sauvegardes
nombre de chemins concurrents
```

L’utilisateur voit :

```text
Pointeur stable trouvé — confiance 94 %
```

---

# 51. Rescan après redémarrage

Fonction essentielle V2 :

```text
Session A
trouve adresse
```

L’utilisateur redémarre le jeu.

```text
Session B
nouvelle adresse
```

KillEngine compare les chemins et élimine les pointeurs qui ne fonctionnent plus.

---

# 52. AOB / signatures — V2

Pour certaines cibles, une signature de code est plus robuste qu’un pointeur.

Exemple :

```text
48 8B ?? ?? 89 43 18 48 85 C0
```

Le scanner AOB doit supporter :

```text
byte exact
??
wildcards
masques
```

---

# 53. « Find what writes » — V2

Une fois la bonne adresse trouvée, KillEngine pourra proposer :

```text
Trouver le code qui modifie cette valeur
```

Le moteur debugger surveille la cible et collecte les instructions qui l’écrivent.

Exemple :

```asm
mov [rbx+18], eax
```

KillEngine pourra interpréter :

```text
RBX semble être l'adresse de base d'une structure.
L'offset de la valeur est probablement +0x18.
```

---

# 54. Désassemblage — V2

Bibliothèque recommandée :

```text
Zydis
```

Pourquoi :

- x86 ;
- x86-64 ;
- rapide ;
- léger ;
- thread-safe ;
- licence MIT ;
- adapté au décodage d’instructions.

---

# 55. Structure Analyzer — V3

À partir d’une adresse de base probable :

```text
base + 0x00
base + 0x04
base + 0x08
...
```

KillEngine peut afficher :

```text
+00  int32  7
+04  int32  100
+08  int32  100
+0C  int32  12
+10  int32  3400
+18  int32  39750
```

L’IA peut proposer :

```text
Structure candidate : Player / Economy
```

sans prétendre que le nom est certain.

---

# 56. Vision — option future

Qwen3.5 étant une famille multimodale, une version ultérieure pourra exploiter la capture de la fenêtre du jeu.

Exemple :

L’utilisateur dit :

```text
Trouve mon argent.
```

KillEngine capture la fenêtre :

```text
39 750 $
```

L’IA lit la valeur affichée.

Lorsque le joueur achète quelque chose :

```text
38 250 $
```

KillEngine peut récupérer lui-même la nouvelle observation.

Objectif à terme :

```text
Utilisateur :
Trouve l'argent.

KillEngine :
Je m'en occupe.
```

Ceci est une V3, pas une dépendance de la V1.

---

# 57. Interface principale

Proposition :

```text
┌───────────────────────────────────────────────────────┐
│ KillEngine                                  ● attaché │
├──────────────┬────────────────────────────────────────┤
│              │                                        │
│ Assistant    │   Que veux-tu trouver ?               │
│              │                                        │
│ Processus    │   [ Argent : 41250                 ]  │
│              │                                        │
│ Mémoire      │   KillEngine analyse Imperialism…     │
│              │                                        │
│ Profils      │   ███████████████░ 76 %               │
│              │                                        │
│ Expert       │   427 candidats                        │
│              │                                        │
│ Paramètres   │   Fais varier l'argent puis :         │
│              │                                        │
│              │   [ J'ai changé ]                     │
└──────────────┴────────────────────────────────────────┘
```

---

# 58. Sélection du processus

L’écran doit privilégier les applications avec fenêtre visible.

Exemple :

```text
Jeux et applications

[Imperialism]
Imperialism.exe
PID 4212
x86

[Seven Cities of Gold]
dosbox.exe
PID 8120
x64
```

Puis :

```text
Processus système
```

dans une section repliée.

---

# 59. Détection automatique du jeu

Option :

```text
[Utiliser la fenêtre active]
```

KillEngine :

1. récupère la fenêtre foreground ;
2. récupère son PID ;
3. identifie l’exécutable ;
4. propose l’attachement.

Cela réduit encore la complexité.

---

# 60. Mode Expert

Le mode Expert fournit l’équivalent des réglages avancés traditionnels.

Il peut exposer :

```text
Value Type
Scan Type
Start Address
Stop Address
Memory Protection
Alignment
Writable
Executable
Copy-on-write
Hex
AOB
```

Mais ce mode ne doit jamais être nécessaire pour l’usage standard.

---

# 61. Memory Viewer — après V1

Une vue hexadécimale pourra être ajoutée :

```text
Address         00 01 02 03 ...
000001D4...     2E A1 00 00 ...
```

Avec :

```text
hex
int
float
double
ASCII
UTF-16
```

---

# 62. Historique IA explicable

Chaque décision doit pouvoir être expliquée.

Exemple :

```text
Pourquoi cette adresse ?

✓ correspondait à 41 250
✓ est passée à 39 750 après la dépense
✓ n'a pas changé pendant 5 secondes d'inactivité
✓ est restée accessible
✓ même comportement sur 3 observations
```

Pas :

```text
L'IA pense que c'est celle-ci.
```

---

# 63. Journal technique

Mode debug :

```text
[17:42:01] Attached PID 4212
[17:42:01] Architecture x86
[17:42:02] 1.84 GB readable
[17:42:04] Exact Int32: 8,431
[17:42:04] Exact UInt32: merged
[17:42:05] Float32: 11
[17:42:05] Candidate total: 8,442
```

Les logs sont locaux.

---

# 64. Confidentialité

Principe :

```text
RAM du jeu
   │
   ▼
KillEngine local
   │
   ▼
IA locale
```

Aucune adresse, capture RAM ou valeur n’a besoin d’être envoyée sur Internet.

La V1 doit être :

```text
offline-first
```

La télémétrie doit être absente ou désactivée par défaut.

---

# 65. Sécurité interne

Le LLM doit être considéré comme une source **non fiable**.

Il ne reçoit jamais :

```text
HANDLE
pointeur C++
fonction brute arbitraire
commande shell
PowerShell
```

Il reçoit uniquement une API fermée.

Exemple :

```text
scan_exact
watch_candidates
```

Le backend contrôle tout.

---

# 66. Garde-fous d’écriture

Modes :

## Safe

Défaut.

- lecture automatique ;
- scan automatique ;
- observation automatique ;
- confirmation avant première écriture.

## Assisted

L’utilisateur autorise KillEngine à appliquer une valeur explicitement demandée sur une cible ayant une confiance élevée.

## Expert

Contrôle manuel complet.

---

# 67. Limites d’usage

KillEngine est conçu pour :

- jeux solo ;
- jeux hors ligne ;
- rétro-gaming ;
- analyse de programmes personnels ;
- tests ;
- apprentissage ;
- modding local.

Le projet ne doit pas chercher à contourner des systèmes anti-cheat ou des protections noyau.

Cela simplifie énormément :

- le développement ;
- les tests ;
- la distribution ;
- la stabilité.

---

# 68. Architecture du dépôt

```text
KillEngine/
│
├─ CMakeLists.txt
├─ README.md
├─ LICENSE
│
├─ apps/
│  └─ desktop/
│     ├─ main.cpp
│     ├─ application_controller.cpp
│     └─ application_controller.h
│
├─ core/
│  ├─ process/
│  │  ├─ process_enumerator.cpp
│  │  ├─ process_handle.cpp
│  │  └─ architecture.cpp
│  │
│  ├─ memory/
│  │  ├─ memory_map.cpp
│  │  ├─ memory_reader.cpp
│  │  ├─ memory_writer.cpp
│  │  └─ memory_region.cpp
│  │
│  ├─ scanner/
│  │  ├─ scan_engine.cpp
│  │  ├─ scan_bundle.cpp
│  │  ├─ scan_types.cpp
│  │  ├─ scan_filters.cpp
│  │  └─ worker_pool.cpp
│  │
│  ├─ candidates/
│  │  ├─ candidate_store.cpp
│  │  ├─ candidate_ranker.cpp
│  │  └─ candidate_watch.cpp
│  │
│  ├─ snapshot/
│  │  ├─ snapshot_store.cpp
│  │  └─ snapshot_codec.cpp
│  │
│  ├─ freeze/
│  │  └─ freeze_manager.cpp
│  │
│  └─ profiles/
│     ├─ profile_store.cpp
│     └─ locator.cpp
│
├─ ai/
│  ├─ ai_engine.cpp
│  ├─ llama_runtime.cpp
│  ├─ tool_registry.cpp
│  ├─ tool_validator.cpp
│  ├─ state_machine.cpp
│  ├─ evidence_summary.cpp
│  └─ prompts/
│     └─ memory_analyst.txt
│
├─ ui/
│  ├─ package.json
│  ├─ vite.config.ts
│  └─ src/
│     ├─ views/
│     ├─ components/
│     ├─ stores/
│     └─ services/
│
├─ third_party/
│
├─ models/
│  └─ README.md
│
├─ tests/
│  ├─ unit/
│  ├─ integration/
│  ├─ memory_targets/
│  └─ benchmarks/
│
├─ docs/
│  ├─ ARCHITECTURE.md
│  ├─ AI.md
│  ├─ MEMORY_ENGINE.md
│  └─ PROFILE_FORMAT.md
│
└─ packaging/
   ├─ installer/
   └─ windows/
```

---

# 69. Programme cible de test

Ne pas développer directement contre un vrai jeu pour chaque fonction.

Créer :

```text
KillEngineTestTarget.exe
```

Cette application de test contient volontairement :

```cpp
int health = 100;
int money = 41250;
float stamina = 75.0f;
double position = 123.456;
```

Elle possède des boutons :

```text
Spend 500
Gain 1000
Damage
Heal
Relocate Object
Reallocate Player
```

Ainsi les tests sont parfaitement reproductibles.

---

# 70. Tests x86 et x64

Produire :

```text
KillEngineTestTarget32.exe
KillEngineTestTarget64.exe
```

Les deux doivent être utilisés dans CI locale / tests Windows.

---

# 71. Test des adresses dynamiques

Le programme cible crée un objet :

```cpp
Player* player = new Player();
```

Puis permet :

```text
Destroy
Reallocate
```

pour vérifier :

- invalidation d’adresse ;
- déplacement heap ;
- comportement des watchers ;
- future recherche de pointeurs.

---

# 72. Test Unknown Value

Le programme cible possède une valeur invisible.

KillEngine reçoit :

```text
Unknown
```

Puis :

```text
Increase
Decrease
No change
```

Les tests vérifient la réduction correcte.

---

# 73. Test de crash

KillEngine doit gérer :

```text
processus fermé pendant scan
processus fermé pendant write
région libérée
ReadProcessMemory échoue
WriteProcessMemory échoue
```

Résultat attendu :

```text
aucun crash KillEngine
```

---

# 74. Objectifs de performance

Les objectifs devront être mesurés, pas supposés.

Benchmarks :

```text
scan 500 MB
scan 1 GB
scan 4 GB

exact int32
float
unknown
next scan
```

Mesures :

```text
GB/s
CPU %
RAM
temps total
nombre d'appels ReadProcessMemory
nombre de candidats / seconde
```

---

# 75. Objectif UX

Pour l’utilisateur, l’écran doit toujours répondre immédiatement.

Le scan tourne dans des workers.

Jamais :

```text
interface figée
```

Progression :

```text
Mémoire analysée : 1,2 / 2,8 Go
Candidats : 4 821
```

---

# 76. Gestion des erreurs IA

Si le modèle renvoie un JSON invalide :

```text
ToolValidator
```

rejette la commande.

Le système peut :

1. demander une correction au modèle ;
2. appliquer un fallback déterministe.

L’application ne doit jamais devenir inutilisable parce que le modèle hallucine.

---

# 77. Fallback sans IA

Très important.

KillEngine doit rester capable de scanner sans modèle.

Le backend possède les fonctions :

```text
exact
unknown
changed
increased
decreased
write
freeze
```

L’IA automatise leur utilisation.

Elle n’est pas leur implémentation.

Ainsi :

```text
IA indisponible
```

n’implique pas :

```text
scanner inutilisable
```

Le mode Expert reste opérationnel.

---

# 78. Boot de l’IA

Au lancement :

```text
HardwareDetector
      │
      ▼
CPU / GPU
      │
      ▼
ModelManager
      │
      ▼
Qwen3.5-2B Q4_K_M
      │
      ▼
llama.cpp
```

Le chargement peut être différé jusqu’à la première utilisation de l’assistant afin de rendre le démarrage plus rapide.

---

# 79. Model Manager

Fonctions :

```text
model installed
model version
model hash
quantization
runtime version
backend
context
```

Exemple :

```text
Qwen3.5-2B
KillEngine Quant Q4_K_M
SHA256: ...
```

---

# 80. Reproductibilité du modèle

Le dépôt doit contenir un script documenté permettant de :

1. récupérer le modèle officiel ;
2. vérifier sa version/hash ;
3. convertir au format GGUF ;
4. quantifier en Q4_K_M ;
5. calculer le SHA-256 ;
6. produire le package KillEngine.

Ne jamais dépendre d’un fichier modèle téléchargé au hasard.

---

# 81. Versionnement des profils

Un profil doit indiquer :

```text
game executable hash
file version
KillEngine version
profile version
```

Une mise à jour du jeu peut invalider :

```text
module offsets
AOB signatures
pointers
```

KillEngine doit le détecter.

---

# 82. SQLite ou JSON ?

## Métadonnées

SQLite convient pour :

- profils ;
- sessions ;
- paramètres ;
- historique ;
- labels.

## Millions de candidats

Ne pas mettre chaque candidat dans SQLite pendant un scan.

Utiliser un format binaire/memory-mapped spécialisé.

---

# 83. Format de session

Exemple :

```text
session.json
```

```json
{
  "process": {
    "name": "imperialism.exe",
    "pid": 4212,
    "architecture": "x86"
  },
  "target": {
    "label": "argent",
    "initialValue": 41250
  },
  "scan": {
    "strategy": "numeric_bundle",
    "candidateCount": 427
  }
}
```

---

# 84. IPC UI / backend

QWebChannel expose des méthodes haut niveau.

Exemples :

```text
attachProcess(pid)
startSmartSearch(query)
confirmValueChanged()
submitObservedValue(value)
writeCandidate(id, value)
freezeCandidate(id)
saveCandidate(id, name)
```

Ne pas exposer directement :

```text
writeMemory(address, rawBytes)
```

au frontend standard.

Le mode Expert peut passer par une API distincte.

---

# 85. Événements backend → UI

Exemples :

```text
processAttached
scanStarted
scanProgress
scanStatsUpdated
candidateCountChanged
aiMessage
targetConfidenceChanged
targetFound
processExited
errorOccurred
```

---

# 86. Contrat « Smart Search »

Entrée :

```json
{
  "query": "argent : 41250"
}
```

Sortie progressive :

```text
intent parsed
scan plan
scan progress
candidate summary
next instruction
target result
```

---

# 87. Parser de valeur

Il doit reconnaître :

```text
41 250
41,250
41250
41 250 $
€ 41250
75 %
12.5
12,5
0x1234
```

La locale française ne doit pas casser les nombres.

---

# 88. Labels sémantiques

Le label est utile pour le planner.

Exemples :

```text
argent
or
money
gold
vie
health
munitions
ammo
bois
wood
année
year
```

Mais le label ne doit jamais déterminer seul le type mémoire.

Il sert uniquement de prior.

---

# 89. Première stratégie IA recommandée

Pour une valeur entière positive :

```text
PASS A

Int32
UInt32
Int64
UInt64
```

Puis :

```text
PASS B

Float32
Float64
```

L’ordre dépend du coût et du contexte.

L’implémentation peut scanner plusieurs types dans **le même buffer** pour éviter de relire la RAM plusieurs fois.

---

# 90. Scan fusionné

Optimisation importante.

Au lieu de :

```text
Read RAM → scan int32
Read RAM → scan int64
Read RAM → scan float
```

faire :

```text
Read RAM
   │
   ├ scan int32
   ├ scan int64
   ├ scan float
   └ scan double
```

Cela évite plusieurs lectures inter-processus.

---

# 91. SIMD — optimisation ultérieure

Après validation fonctionnelle :

```text
SSE2
AVX2
AVX512
```

pour certains scans.

Ne pas commencer par cela.

Priorité :

```text
correctness
puis benchmark
puis optimisation
```

---

# 92. Ordre de fabrication

## PHASE 0 — Fondation

Créer :

```text
repo
CMake
Qt shell
Vue shell
QWebChannel
logging
tests
```

### Validation

L’application démarre et Vue peut appeler une fonction C++.

---

## PHASE 1 — Process Manager

Créer :

```text
process enumeration
window detection
PID
path
architecture
modules
```

### Validation

KillEngine affiche correctement le programme cible x86 et x64.

---

## PHASE 2 — Memory Map

Créer :

```text
VirtualQueryEx iterator
region filters
memory statistics
```

### Validation

Affichage :

```text
committed
readable
writable
executable
```

---

## PHASE 3 — Memory Reader

Créer :

```text
safe chunk reader
error handling
cancellation
```

### Validation

Lire des variables connues dans `KillEngineTestTarget`.

---

## PHASE 4 — Exact Scan

Créer :

```text
Int32
Int64
Float32
Float64
```

### Validation

Retrouver automatiquement les variables du programme cible.

---

## PHASE 5 — CandidateStore

Créer :

```text
compact results
sorting
filtering
pagination
```

### Validation

Supporter plusieurs millions de candidats sans exploser la RAM.

---

## PHASE 6 — Next Scan

Créer :

```text
exact
changed
unchanged
increased
decreased
delta
```

### Validation

Scénarios automatisés avec le programme cible.

---

## PHASE 7 — Unknown Initial Value

Créer :

```text
snapshot
comparison
LZ4
mapped storage
```

### Validation

Retrouver une variable dont KillEngine ignore la valeur initiale.

---

## PHASE 8 — Watch / Write / Freeze

Créer :

```text
watch
safe write
verify
rollback
freeze
```

### Validation

Modifier et restaurer le programme de test.

---

## PHASE 9 — IA locale

Intégrer :

```text
llama.cpp
Qwen3.5-2B
GGUF Q4_K_M
```

Créer :

```text
AIEngine
ToolRegistry
ToolValidator
StateMachine
IntentContract
fallback déterministe
```

### Validation

L’IA peut demander un scan exact via JSON sans accès Win32 direct, et peut produire une intention structurée validée avant exécution.

---

## PHASE 10 — Smart Search

Créer le workflow magique :

```text
"Argent : 41250"
       │
       ▼
auto scan
       │
       ▼
auto refinement
       │
       ▼
target confidence
```

### Validation

Un utilisateur sans connaissance de Cheat Engine retrouve une variable du programme cible, peut réécrire les dernières adresses trouvées dans la conversation, et peut démarrer une nouvelle recherche sans rester bloqué sur les anciennes adresses actives.

---

## PHASE 11 — Profils

Créer :

```text
labels
module offsets
hash exe
session persistence
multiple targets
assistant target reuse
```

### Validation

KillEngine retrouve une ou plusieurs cibles statiques après redémarrage, puis l'Assistant peut les utiliser pour écrire une nouvelle valeur.

---

## PHASE 12 — Polissage V1

Précondition :

```text
Ne pas démarrer cette phase tant que les pré-requis Phase 12 de la baseline ne sont pas terminés ou explicitement descopés.
```

Créer :

```text
installer
settings
logs
crash handling
model manager
updates
documentation
Mode Expert UX
```

---

# 93. Critères de réussite V1

La V1 est terminée lorsque le scénario suivant fonctionne :

1. lancer `KillEngineTestTarget`;
2. lancer KillEngine ;
3. sélectionner le processus ;
4. écrire :

```text
Argent : 41250
```

5. ne choisir aucun type mémoire ;
6. laisser KillEngine scanner ;
7. cliquer sur :

```text
J'ai changé
```

après avoir dépensé 500 ;
8. KillEngine identifie la bonne variable ;
9. afficher un score de confiance ;
10. demander :

```text
Nouvelle valeur ?
```

11. entrer :

```text
900000
```

12. la variable du programme cible devient 900000 ;
13. le bouton Restaurer remet la valeur originale ;
14. le profil peut être enregistré.

**Si ce scénario fonctionne proprement, le concept KillEngine est validé.**

---

# 94. Critères de qualité

## Aucun crash sur

```text
process quit
invalid address
failed read
failed write
region disappeared
scan cancel
AI invalid JSON
model unavailable
```

## Aucun blocage UI

Tous les travaux lourds doivent être asynchrones.

## Aucune hallucination d’adresse

Une adresse affichée provient obligatoirement de KillCore.

---

# 95. Tests automatiques

## Unit tests

```text
value parser
scan comparator
float tolerance
candidate scoring
JSON tool validation
profile parser
```

## Integration tests

```text
attach
read
scan
next scan
write
rollback
freeze
process exit
```

## AI tests

Jeu de scénarios fixe :

```text
money exact
money changed
unknown health
ambiguous duplicates
no result
process closed
```

Le test vérifie surtout **l’action choisie**, pas la formulation du texte.

---

# 96. Golden scenarios IA

Exemple :

Entrée :

```text
Je cherche l'argent, j'ai 41250.
```

État :

```text
no scan
```

Action attendue :

```text
scan_exact / numeric_bundle
```

Autre exemple :

```text
8421 candidates
user_value_unknown_after_change
```

Action attendue :

```text
ask user to change value
```

ou :

```text
capture differential snapshot
```

Pas :

```text
invent address
```

---

# 97. Benchmark IA

Mesurer :

```text
model load time
tokens/s CPU
tokens/s Vulkan
RAM
VRAM
latency first token
tool decision latency
```

Mais KillEngine doit continuer à scanner pendant que l’IA réfléchit.

---

# 98. Priorité CPU

Le scanner et le modèle peuvent vouloir beaucoup de CPU en même temps.

Créer un Resource Manager.

Pendant le scan :

```text
AI threads réduits
scanner prioritaire
```

Après le scan :

```text
scanner idle
AI utilise plus de threads
```

Cela évite que les deux moteurs se battent pour tous les cœurs.

---

# 99. Packaging

Deux distributions possibles.

## KillEngine Full

```text
KillEngine
Qt runtime
Vue assets
llama.cpp
Qwen3.5-2B Q4_K_M
```

Fonctionne immédiatement offline.

## KillEngine Core

```text
KillEngine
sans modèle
```

Le Model Manager télécharge ensuite le modèle.

Pour une version personnelle, **Full** est la plus simple.

---

# 100. Installation Windows

Installer recommandé :

```text
Inno Setup
```

ou :

```text
WiX Toolset
```

Installation :

```text
Program Files\KillEngine\
```

Données :

```text
%LOCALAPPDATA%\KillEngine\
```

Profils exportables :

```text
*.keprofile
```

---

# 101. Code signing

Avant diffusion publique :

```text
signature Authenticode
```

à envisager.

Les programmes qui ouvrent et modifient la mémoire d’autres processus peuvent déclencher des alertes de sécurité.

La signature, un installateur propre et un comportement transparent améliorent la confiance.

---

# 102. Mise à jour

Séparer :

```text
KillEngine version
AI model version
profile version
```

Un modèle peut être mis à jour sans remplacer le scanner.

---

# 103. Licence du projet

Choisir explicitement la licence de KillEngine dès le début.

Pour les dépendances :

- respecter leurs licences ;
- conserver les notices ;
- tenir un fichier `THIRD_PARTY_NOTICES.md`.

Le développement doit rester **clean-room** :

> étudier les capacités et concepts généraux des logiciels existants, mais réimplémenter le moteur à partir des API Windows documentées, de nos tests et de notre propre architecture.

Ne pas copier-coller du code provenant de Cheat Engine dans KillEngine.

---

# 104. Dépendances proposées

## Requises

```text
Qt 6
Qt WebEngine
llama.cpp
Qwen3.5-2B
nlohmann/json
LZ4
```

## V2

```text
Zydis
```

## UI build

```text
Vue 3
Vite
TypeScript
Pinia
```

---

# 105. Ce qu’il ne faut surtout pas faire

## 1

Construire le scanner en Python.

## 2

Envoyer les millions de candidats au LLM.

## 3

Laisser le LLM écrire directement à une adresse.

## 4

Faire dépendre la recherche de la qualité du modèle.

## 5

Développer un driver kernel avant que la V1 fonctionne.

## 6

Commencer par un pointer scanner gigantesque.

## 7

Faire du fine-tuning avant d’avoir des données.

## 8

Mélanger UI, scan, IA et Win32 dans les mêmes classes.

## 9

Relire toute la RAM pour chaque type numérique.

## 10

Essayer de reproduire visuellement Cheat Engine.

KillEngine doit avoir sa propre identité et surtout une UX beaucoup plus simple.

---

# 106. Vision finale

À terme :

```text
Utilisateur :
Je veux modifier mon argent.

KillEngine :
J'ai détecté Imperialism.
Ton argent semble être à 41 250.
Fais une dépense.

Utilisateur :
C'est fait.

KillEngine :
Valeur identifiée avec 98 % de confiance.
Elle est maintenant à 39 750.

Utilisateur :
Mets 900 000 et garde-la à ce niveau.

KillEngine :
Fait.
```

Le logiciel masque :

```text
VirtualQueryEx
ReadProcessMemory
Int32
heap
alignement
snapshots
next scan
candidate sets
module offsets
pointer chains
```

C’est précisément la raison d’être du projet.

---

# 107. Roadmap après V1

## V1.1

- amélioration scoring ;
- recherche AOB ;
- export profils ;
- meilleur mode Expert.

## V1.5

- auto-détection de valeurs affichées simples ;
- meilleure analyse de doublons ;
- signatures module-relative.

## V2

- pointer scanner ;
- rescan après restart ;
- debugger user-mode ;
- « what writes » ;
- « what accesses » ;
- Zydis.

## V3

- structure inference ;
- analyse écran par vision ;
- recherche quasi autonome ;
- génération d’éditeur/trainer local à partir d’un profil.

---

# 108. Première milestone de développement

Le premier objectif ne doit pas être :

```text
faire un nouveau Cheat Engine
```

Il doit être :

> **Créer le plus petit prototype capable de transformer “Money = 41250” en identification automatique d’une variable mémoire sur notre programme de test.**

Milestone :

```text
KillEngine Prototype 0.1

✓ attach
✓ memory map
✓ read
✓ exact scan
✓ next scan
✓ multi-type
✓ watch
✓ write
✓ Qwen planner
✓ smart workflow
```

Une fois cela obtenu, le projet possède son cœur technologique.

---

# 109. Décision architecturale finale

Le cœur de KillEngine est donc :

```text
                  KILLENGINE
                      │
        ┌─────────────┴─────────────┐
        │                           │
   DETERMINISTIC CORE           AI BRAIN
        │                           │
   Win32 + C++20             Qwen3.5-2B
        │                     llama.cpp
        │                           │
        ├── Memory Map              │
        ├── Scanner                 │
        ├── Snapshots               │
        ├── Candidates ◄────────────┤
        ├── Ranker                  │
        ├── Watcher                 │
        ├── Writer                  │
        └── Profiles                │
                                    │
                           Plan / interprétation
```

**L’IA dirige.  
Le moteur mesure.  
Le moteur décide de ce qui est techniquement vrai.  
L’IA explique et choisit l’étape suivante.**

C’est cette séparation qui doit rendre KillEngine à la fois puissant, fiable et accessible.

---

# 110. Références techniques de départ

Sources primaires / officielles utilisées pour fixer cette architecture :

## Microsoft Win32

- OpenProcess  
  https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-openprocess

- VirtualQueryEx  
  https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualqueryex

- ReadProcessMemory  
  https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-readprocessmemory

- WriteProcessMemory  
  https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-writeprocessmemory

## Cheat Engine — référence fonctionnelle, pas base de code

- dépôt source  
  https://github.com/cheat-engine/cheat-engine

- interface du moteur de scan / FirstScan / NextScan  
  https://github.com/cheat-engine/cheat-engine/blob/master/Cheat%20Engine/LuaMemscan.pas

- pointer scan worker  
  https://github.com/cheat-engine/cheat-engine/blob/master/Cheat%20Engine/pointerscanworker.pas

- abstraction des accès mémoire  
  https://github.com/cheat-engine/cheat-engine/blob/master/Cheat%20Engine/NewKernelHandler.pas

## IA

- Qwen3.5-2B  
  https://huggingface.co/Qwen/Qwen3.5-2B

- Qwen3.5-4B  
  https://huggingface.co/Qwen/Qwen3.5-4B

- llama.cpp  
  https://github.com/ggml-org/llama.cpp

## Composants natifs

- Zydis  
  https://github.com/zyantific/zydis

- nlohmann/json  
  https://github.com/nlohmann/json

- LZ4  
  https://github.com/lz4/lz4

---

# 111. Résumé exécutable

```text
Nom
KillEngine

Plateforme
Windows 10/11 x64

UI
Qt 6 + Qt WebEngine + Vue 3 + TypeScript

Core
C++20 + Win32

IA
Qwen3.5-2B

Quantification
GGUF Q4_K_M

Runtime IA
llama.cpp

Mode IA
local / offline

Scanner V1
user-mode uniquement

Principe UX
l'utilisateur donne la valeur, KillEngine choisit les scans

Stockage candidats
binaire compact / memory mapped

Snapshots
blocs + LZ4

Profils
SQLite/JSON + format .keprofile

Future
pointers + debugger + AOB + structures + vision
```

---

# 112. Phrase de référence du projet

> **KillEngine n’est pas un scanner mémoire auquel on a ajouté un chatbot.  
> KillEngine est un scanner mémoire conçu dès l’origine pour être piloté par une intelligence artificielle.**
