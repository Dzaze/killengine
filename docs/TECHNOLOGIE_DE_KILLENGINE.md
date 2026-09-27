# La technologie derrière KillEngine

KillEngine est un outil d'analyse et d'édition de la mémoire des processus Windows. Il s'inspire du fonctionnement d'un logiciel bien connu dans le monde du modding, **Cheat Engine**, mais repense entièrement l'approche : au lieu de demander à l'utilisateur de connaître les types de données, l'alignement mémoire ou les pointeurs, KillEngine embarque une **IA locale** capable de choisir et d'enchaîner les scans à sa place. L'utilisateur donne une information simple — « la valeur affichée est 100 » — et KillEngine se charge du reste.

Ce document présente les technologies utilisées, les capacités embarquées et les différentes façons d'utiliser l'outil.

---

## 1. Positionnement

KillEngine s'adresse à quatre profils, du plus guidé au plus manuel, via quatre vues principales :

| Vue | Rôle | Pour qui |
| --- | --- | --- |
| **Assistant** | Pilotage en langage naturel : on décrit ce qu'on cherche, l'IA choisit et enchaîne les scans sûrs | Le flux recommandé, débutant comme expert |
| **Investigation** | Timeline de l'enquête : stratégie choisie, étapes exécutées, hypothèses, niveau de confiance, points de contrôle à valider | Comprendre *pourquoi* l'IA a fait tel choix |
| **Expert** | Outils manuels complets : scans exact/unknown/chiffré, traçage de chaînes de texte affichées, scanner de pointeurs, écriture/freeze, débogage, signatures de code, patchs | Reprendre la main sur chaque étape |
| **Trainer** | Fonctionnalités nommées et réutilisables, interrupteurs ON/OFF, overlay, raccourcis clavier globaux | Le résultat final, prêt à être réutilisé |

Une cinquième vue, **Settings**, centralise les paramètres, les journaux, les diagnostics et la compatibilité antivirus.

---

## 2. Stack technologique

| Brique | Technologie | Rôle |
| --- | --- | --- |
| Moteur mémoire | **C++20**, API Win32 natives | Scanner multithread déterministe : lecture/écriture mémoire, énumération de processus, gestion des régions |
| Intelligence artificielle | **llama.cpp** + modèle **Qwen** (GGUF, quantifié) | IA embarquée, exécutée 100 % en local, sans cloud, pour le tool-calling et le raisonnement guidé |
| Interface | **Qt 6.8 (WebEngine)** + **Vue 3** + **TypeScript** + **Vite** | Application desktop native dont l'UI est une app web moderne, reliée au C++ via un pont `QWebChannel` |
| Compression | **LZ4** | Compression des snapshots mémoire pour les scans de type « valeur inconnue » |
| Sérialisation | **nlohmann/json** | Échanges structurés entre le moteur, l'IA et l'interface |
| Désassemblage x64 | **Zydis** | Décodage d'instructions pour l'analyse de code, les signatures et les patchs |
| Inspection .NET/CLR | **ClrMD** (helper .NET dédié) | Lecture du tas managé pour les jeux/applications basés sur Mono ou .NET |
| Pilote noyau (optionnel) | **Windows Driver Kit (WDK)** | Bridge kernel-mode pour un accès mémoire qui contourne certaines protections user-mode |
| Tests | **Google Test** | Suites unitaires et d'intégration |
| Plateforme | **Windows 64-bit** | Cible unique, aucune dépendance externe à l'exécution (build portable) |

L'architecture logicielle sépare trois couches indépendantes :

- **Core** — le moteur mémoire déterministe (scan, candidats, snapshots, pointeurs, freeze, débogage bas niveau).
- **AI** — la couche de raisonnement (contrat d'intention structuré, registre d'outils, exécution du plan).
- **UI** — l'interface Vue 3, qui ne contient aucune logique métier sensible : elle appelle le moteur via des méthodes exposées côté C++.

---

## 3. Ce que KillEngine sait faire

### Scan et recherche de valeurs en mémoire
- Scan exact, scan multi-type, scan asynchrone avec progression et annulation.
- Tous les types numériques usuels et leurs variantes (entiers signés/non signés, flottants, virgule fixe, multiplicateurs x10/x100/x1000/x65536…).
- Recherche en « valeur inconnue » (l'utilisateur ne connaît pas la valeur de départ, seulement comment elle évolue : augmente, diminue, change, reste stable), avec snapshots compressés LZ4.
- Scan de données chiffrées/obfusquées (XOR, addition/soustraction, NOT) avec recherche de clé bornée.
- Recherche groupée de plusieurs valeurs liées par des offsets relatifs (utile pour une structure de données).
- Gestion de gros volumes de résultats : stockage en fichier temporaire, pagination, tri, undo/restauration.

### Écriture, verrouillage et sécurisation des changements
- Écriture mémoire vérifiée, multi-adresses, atomique (avec suspension temporaire des threads si nécessaire).
- « Freeze » d'une valeur par écriture périodique (polling), avec détection automatique d'instabilité et intervalle réglable.
- « Freeze » par point d'arrêt matériel (hardware breakpoint) : la valeur est corrigée dans le cycle même de l'écriture, sans le clignotement typique d'un freeze par polling classique.
- Annulation (rollback) de la dernière écriture ou d'un lot d'écritures.

### Traçage de valeurs affichées à l'écran
Beaucoup de valeurs visibles dans une interface (score, vie, munitions) ne sont que des copies d'affichage d'une donnée gameplay réelle. KillEngine sait :
- scanner directement les chaînes de texte affichées (ASCII/UTF-16),
- suivre ces chaînes quand la valeur change,
- remonter vers les sources numériques qui les alimentent,
- puis vers les pointeurs qui alimentent ces sources — pour retrouver la vraie donnée derrière l'affichage.

### Débogage et instrumentation bas niveau
- « Find What Writes » / « Find What Accesses » : capturer l'instruction exacte qui écrit ou lit une adresse mémoire, via points d'arrêt matériels (DR0-DR7).
- Surveillance mémoire par Page Guard, sans monopoliser le canal de débogage Windows.
- Désassemblage à la volée autour d'une adresse ou d'un hit de capture.

### Signatures de code, patchs et injection
- Génération de signatures AOB (« array of bytes ») stables autour d'une instruction.
- Suggestion automatique de patchs classés par risque (NOP, forcer un saut pris/non pris, INT3, RET…), application et restauration.
- Injection de DLL, pose de hooks de fonctions, et un **mini-assembleur x64** intégré (façon script Cheat Engine) pour écrire des routines d'injection simples directement en mémoire.

### Chaînes de pointeurs et profils réutilisables
- Scanner de pointeurs multi-niveaux pour retrouver une adresse stable qui survit à un redémarrage du programme.
- Profils (`.keprofile`) qui sauvegardent cibles, chaînes de pointeurs, patchs et scripts pour les réutiliser d'une session à l'autre.
- Pont d'export/import avec **Ghidra** pour faire circuler des symboles entre les deux outils.

### Trainer — le résultat prêt à l'emploi
- Regroupement des découvertes en fonctionnalités nommées, activables individuellement.
- Overlay superposable à l'application ciblée, avec raccourcis clavier globaux (freeze, patch, écriture de valeur).
- Dépendances entre fonctionnalités, export JSON/Markdown, apply all / restore all.

### Intelligence artificielle et automatisation
- IA locale (llama.cpp + Qwen), aucune donnée envoyée dans un cloud.
- « Smart Search » et « Auto Resolve » : l'IA enchaîne automatiquement plusieurs stratégies de scan (exact → chiffré → traçage de texte → valeur inconnue) selon ce qui fonctionne, avec confirmation obligatoire avant toute écriture.
- Mémoire de patterns par jeu/application pour accélérer les sessions suivantes.
- **Pipe d'automatisation locale** (canal JSON-RPC sur named pipe Windows) : n'importe quel outil du moteur peut être piloté par un script ou un agent IA externe, en parallèle de l'utilisateur qui observe l'interface.
- **Scripting Lua** intégré pour automatiser des séquences d'actions (scan, next scan, lecture de candidats) via un helper dédié.

### Support .NET / CLR
- Inspecteur dédié pour les applications basées sur **Mono** ou **.NET CoreCLR** : énumération d'objets managés, recherche par type, lecture/écriture de champs, suivi des racines du ramasse-miettes — là où un scan mémoire brut échoue à cause du déplacement des objets par le GC.

### Accès noyau (optionnel, avancé)
- Un pilote Windows dédié (`KillEngineKernel.sys`) permet, quand l'utilisateur l'active explicitement, un accès mémoire kernel-mode qui contourne certaines protections user-mode. Traité comme une action à haut risque : confirmation explicite systématique.

---

## 4. Cas d'usage concrets

- **Modding et création de trainers** pour des jeux solo (vie infinie, munitions illimitées, argent, vitesse…), à la manière de Cheat Engine mais assisté par IA.
- **Reverse engineering éducatif** : comprendre comment une application stocke et manipule ses données en mémoire.
- **Recherche de sauvegardes locales** (notamment applications UWP) : découverte, lecture, surveillance et patch sûr de fichiers de sauvegarde.
- **Débogage de ses propres logiciels** : retrouver quelle instruction écrit une valeur inattendue, tracer un champ recalculé dynamiquement (interpolation d'affichage) plutôt qu'une vraie donnée source.
- **Recherche en sécurité et tests autorisés** : signatures de code, patchs binaires, hooks de fonctions, injection — sur des cibles où l'utilisateur a l'autorisation d'intervenir.
- **Automatisation de sessions de test** : piloter le moteur depuis un script Lua ou un agent IA externe pendant qu'un humain observe et valide.

> KillEngine est conçu pour être utilisé sur des processus dont l'utilisateur a la maîtrise ou l'autorisation (jeux solo, logiciels personnels, cibles de test autorisées). Ce n'est pas un outil de triche en ligne ni un outil de contournement d'anti-cheat à des fins malveillantes.

---

## 5. Toutes les façons de l'utiliser

1. **Interface graphique guidée (Assistant)** — en langage naturel, sans connaissance technique préalable.
2. **Mode Expert** — accès manuel à chaque scan, chaque outil, chaque réglage, pour l'utilisateur qui veut garder la main.
3. **Trainer** — une fois la donnée trouvée, la transformer en fonctionnalité permanente avec overlay et raccourcis clavier.
4. **Scripting Lua** — automatiser une séquence d'actions reproductible (`scripts/killengine.lua`).
5. **Pipe d'automatisation JSON-RPC** — piloter n'importe quelle capacité du moteur depuis un script externe ou un agent IA tiers, en local, sans réseau.
6. **Profils `.keprofile`** — sauvegarder une cible complète (chaînes de pointeurs, patchs, scripts) et la recharger d'une session à l'autre, y compris après un redémarrage du programme ciblé.
7. **Pont Ghidra** — exporter/importer des symboles entre KillEngine et un projet de reverse engineering statique.

---

## 6. Statut du projet

KillEngine est un prototype avancé en montée en gamme continue : le moteur mémoire, les profils, le Trainer, le scan multi-type et l'IA locale sont opérationnels et testés. Le projet évolue activement, avec une checklist de non-régression et une suite de tests automatisés à chaque itération.
