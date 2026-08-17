# KillEngine Ultimate Product Guideline

Ce document est la ligne directrice de construction pour transformer KillEngine en produit premium.
Il doit etre lu par tout agent IA avant de contribuer aux phases avancees.

Objectif commercial: rendre KillEngine assez utile, fiable et agreable pour qu'un utilisateur serieux accepte de payer au moins 50 USD pour gagner du temps dans l'analyse runtime de programmes Windows qu'il possede ou controle.

Positionnement public: KillEngine est un assistant d'analyse runtime, reverse engineering educatif, debugging memoire, modding et trainer-building pour logiciels locaux autorises. Ne pas positionner le produit comme un outil de contournement anti-cheat, malware, vol de donnees, intrusion ou abus de services tiers.

## Vision

KillEngine doit devenir:

> Un workspace d'analyse runtime assiste par IA qui transforme une observation utilisateur en enquete reproductible, puis en feature stable.

L'utilisateur ne doit pas avoir a connaitre toute la technique pour avancer. Il doit pouvoir dire:

- "Je vois 45 a l'ecran, trouve ce qui alimente cette valeur."
- "Cette valeur revient toute seule, rends-la stable."
- "Transforme cette piste en toggle reutilisable."
- "Explique pourquoi ce scan ne trouve rien et propose la meilleure piste suivante."

KillEngine doit repondre par des actions outillees, bornees, visibles et confirmables:

1. Choisir la strategie.
2. Executer les actions safe.
3. Montrer les hypotheses et leur confiance.
4. Demander confirmation avant toute action risquee.
5. Sauvegarder ce qui marche dans un profil reutilisable.

## Regle D'Or Pour Les Agents IA

Un agent ne doit pas se contenter de creer une sous-tache quand il peut livrer une tranche produit complete.

Une tache est complete seulement si elle inclut:

- implementation backend ou core;
- raccord UI si la fonctionnalite est utilisateur;
- persistance/profil si le resultat doit survivre a la session;
- telemetry/audit si l'action influence une enquete;
- garde-fous si l'action peut ecrire, debugger, patcher, hooker ou injecter;
- tests unitaires ou integration selon le risque;
- mise a jour de `docs/PHASE_TRACKER.md`.

Interdit:

- marquer une phase terminee sans workflow utilisable de bout en bout;
- cacher une action dangereuse derriere un bouton vague;
- ajouter un module core sans l'exposer a l'Assistant, a Expert ou a Trainer quand c'est attendu;
- ajouter une feature sans test minimal;
- creer seulement une documentation ou une TODO quand le code peut etre fait dans la meme passe;
- automatiser debug, patch, hook, injection ou write sans consentement utilisateur explicite.

## Inspirations A Integrer

KillEngine ne doit pas copier un outil existant; il doit combiner leurs meilleures idees avec une IA d'orchestration.

### Cheat Engine

Inspirations:

- scan de valeurs et variantes;
- memory viewer avance;
- auto assembler;
- find what writes/accesses;
- break and trace;
- dissect structures;
- pointer scanner;
- cheat tables/profils reutilisables.
- AOB scan avec wildcards, filtres protection/alignment et signatures uniques.

Direction KillEngine:

- garder la puissance de Cheat Engine;
- rendre les workflows comprehensibles par Assistant et Investigation;
- transformer les trouvailles en features Trainer propres.

Reference: https://wiki.cheatengine.org/index.php?title=Cheat_Engine%3AHelp_File

### Frida / Luma

Inspirations:

- instrumentation dynamique;
- sessions persistantes;
- REPL/script runtime;
- trace de fonctions avec handlers editables;
- workspace collaboratif et live.
- tracing par blocs/instructions pour expliquer le flux runtime sans forcer l'utilisateur a tout debugger manuellement.

Direction KillEngine:

- ne pas demarrer par une copie Frida;
- construire d'abord un pipeline externe fiable;
- ajouter plus tard une couche instrumentation/hook scriptable pour utilisateurs experts;
- garder les scripts bornes, audites et confirmes.

Reference: https://frida.re/

### x64dbg

Inspirations:

- debugger Windows extensible;
- plugins C++;
- database utilisateur: comments, labels, bookmarks;
- desassemblage rapide;
- API plugin et personnalisation.
- trace conditionnelle avec historique instructions, registres et acces memoire.

Direction KillEngine:

- ajouter labels, notes IA, bookmarks de candidats, RIP, AOB et structures;
- permettre des extensions propres;
- faire du debug un workflow guide, pas une fenetre obscure.

Reference: https://x64dbg.com/

### Ghidra

Inspirations:

- plateforme SRE extensible;
- disassembly, assembly, decompilation, graphing, scripting;
- analyse a grande echelle;
- projets/workspaces durables.
- identification par signatures/hashes de fonctions pour stabiliser les artefacts quand les offsets bougent.

Direction KillEngine:

- ne pas chercher a remplacer Ghidra;
- devenir le meilleur pont entre runtime live et comprehension assistee;
- exporter/importer des artefacts: AOB, modules, offsets, notes, symboles, rapports.

### Algorithmes Retenus Pour KillEngine

Les algorithmes doivent privilegier robustesse, explicabilite et performance mesurable:

- AOB wildcard accelere: recherche type Boyer-Moore-Horspool avec ancre sur octet fixe, tout en gardant les overlaps et patterns wildcard.
- Scoring AOB: evaluer longueur, octets fixes, wildcards, diversite des octets et securite Trainer avant sauvegarde ou patch.
- Garde-fou Trainer AOB: une feature patch persistante doit avoir une signature unique dans le code image executable et assez d'octets fixes; sinon bloquer la sauvegarde et expliquer comment stabiliser.
- Profils Trainer auditables: persister dans `.keprofile` les metriques AOB (score, niveau, fixes, wildcards, unicite) pour que l'utilisateur et l'IA puissent juger une feature sans repartir de zero.
- Trainer local auditables: les features creees depuis Expert/bookmarks/checkpoints doivent conserver les metriques AOB dans le store local, l'export et la sauvegarde profil; l'UI doit bloquer les patchs AOB faibles avant appel backend.
- Resolution AOB locale: une feature Trainer locale avec `locatorKind=aob` doit rescanner la signature en code image executable avant patch, exiger un seul match, puis patcher l'adresse resolue plutot que l'adresse historique.
- Application Trainer protegee: ne jamais appliquer en lot une feature patch dont l'AOB est faible, introuvable ou non unique; les anciens profils doivent etre revalides avant activation.
- IA Trainer consciente de la qualite: les rapports Auto doivent detecter AOB faible, multi-match et blocages Trainer, puis recommander explicitement stabilisation/regeneration plutot que patch direct.
- Trace conditionnelle: capturer seulement les instructions utiles selon adresse, module, ecriture memoire ou predicate utilisateur.
- Trace history: enregistrer RIP, module, registres utiles et acces memoire autour d'un evenement pour alimenter Investigation.
- Signature de fonction: hash stable de fenetres code et metadonnees module/section pour retrouver une feature apres mise a jour.
- Scoring de confiance: combiner unicite AOB, stabilite module, proximite instruction-source, type memoire et observations live.

Reference: https://github.com/NationalSecurityAgency/ghidra

## Architecture Produit Cible

KillEngine doit converger vers quatre vues principales.

### 1. Assistant

Role: langage naturel, decision, orchestration safe.

Doit faire:

- comprendre l'objectif utilisateur;
- choisir entre scan exact, Unknown, Trace UI, scan chiffre, structures, pointer, debugger, AOB;
- executer automatiquement uniquement les actions safe;
- expliquer les resultats;
- proposer des checkpoints clairs pour les actions risquees;
- apprendre des sessions precedentes.

### 2. Investigation

Role: timeline d'enquete.

Doit afficher:

- objectif;
- strategie choisie;
- etapes executees;
- resultats chiffres;
- hypotheses;
- confiance;
- risques;
- prochaine action;
- boutons confirmer/annuler;
- liens vers Expert/Trainer.

Cette vue est obligatoire pour rendre l'IA credible. Un utilisateur doit voir ce que l'IA fait et pourquoi.

### 3. Expert

Role: outils manuels puissants.

Doit contenir:

- scans exact/multi-type/chiffre;
- Unknown initial value;
- Trace UI string;
- structure analyzer;
- pointer scanner;
- write/freeze/freeze BP;
- find what writes;
- AOB signatures;
- patches;
- live watch;
- diagnostics.

Expert ne doit pas etre le seul chemin. Tout outil important doit pouvoir etre appele par Assistant ou Investigation.

### 4. Trainer

Role: produit final utilisateur.

Doit contenir:

- features nommees;
- toggles ON/OFF;
- hotkeys;
- apply all / restore all;
- statut par feature;
- warnings multi-match;
- profil par jeu/app/version;
- historique d'application;
- rollback.

Le Trainer Builder est ce qui transforme KillEngine en produit vendable.

## Phases Determinantes

Les phases ci-dessous ne sont pas des suggestions. Elles definissent le chemin pour atteindre le produit premium.

### Phase U1 - Assistant Proactif Complet

Objectif: l'Assistant mene une enquete safe de bout en bout jusqu'au checkpoint write/freeze.

Livrables obligatoires:

- `startAutoResolve` gere les etats: nouveau scan, reduction active, no candidates, too many candidates, low candidates.
- `getAutoResolveReport` et `startAutoResolve` produisent toujours une `nextBestAction` unique, scoree et expliquee, pour eviter les recommandations molles.
- Auto execute une mini-boucle bornee `executedSafeSteps` sur actions safe uniquement: scan/reduction, fallback scan chiffre borne, puis Trace UI string borne; arret obligatoire sur variation utilisateur, ecriture, freeze breakpoint, debugger, patch ou injection.
- L'Assistant affiche `executedSafeSteps` comme preuve structuree dans le chat: outil, statut, metrique et detail, sans cacher l'audit dans un texte libre.
- Les resultats produits par la mini-boucle safe doivent etre actionnables: hits chiffres et strings UI sont promus en checkpoints Investigation bornes, jamais en ecriture automatique.
- Si les fallbacks exact/chiffre/Trace UI sont vides, Auto capture un snapshot Unknown borne et passe en `awaiting_unknown_observation`; la reprise doit utiliser ce snapshot existant.
- Les checkpoints Auto doivent etre scores et tries: encrypted hits, strings UI, sources UI numeriques et candidats Unknown.
- L'export Investigation doit produire un bilan final exploitable: actions safe, checkpoints, meilleure piste et prochaine etape.
- Auto peut declencher ou preparer:
  - scan exact multi-type;
  - Unknown capture async;
  - Unknown compare apres variation utilisateur;
  - scan chiffre borne;
  - Trace UI string;
  - analyse sources UI.
- Auto produit une timeline Investigation.
- Auto produit des `suggestedWrites` seulement quand le nombre de candidats est bas.
- Auto n'ecrit jamais sans confirmation.
- Tests unitaires pour strategie et transitions.
- Test integration sur `KillEngineTestTarget.exe` pour scan -> reduction -> low candidates -> suggestedWrites.

Definition of Done:

- un utilisateur peut partir d'une phrase simple, faire varier une valeur deux fois, et obtenir une liste courte de candidats avec explication et checkpoint.

### Phase U2 - Investigation Timeline

Objectif: rendre l'enquete lisible et pilotable.

Livrables obligatoires:

- nouvelle vue `InvestigationView.vue`;
- modele backend/TS pour `InvestigationRun`, `InvestigationStep`, `Hypothesis`, `Checkpoint`;
- persistance session locale;
- chaque outil appele par Auto ajoute une etape;
- boutons: confirmer, annuler, ouvrir Expert, creer feature Trainer;
- timeline filtrable par risque/resultat/type;
- export JSON/Markdown du rapport.

Definition of Done:

- on peut comprendre apres coup pourquoi KillEngine a choisi une strategie, quels outils ont ete lances, et ce qu'il recommande.

### Phase U3 - Trainer Builder

Objectif: transformer une piste validee en feature reutilisable.

Livrables obligatoires:

- vue `TrainerView.vue`;
- modele `TrainerFeature`:
  - name;
  - process/module/version;
  - locator: absolute, module_offset, pointer_chain, aob, patch;
  - action: write, freeze_polling, freeze_breakpoint, patch, hook;
  - value/type;
  - hotkey;
  - risk;
  - status;
  - rollback data.
- bouton "Creer feature" depuis Assistant/Investigation/Expert;
- apply/restore par feature;
- apply all/restore all;
- verification avant application;
- blocage si AOB multi-match non resolu;
- sauvegarde dans ProfileStore.

Definition of Done:

- une decouverte peut devenir un toggle reutilisable apres redemarrage de l'app.

### Phase U4 - Runtime Debugger Propre

Objectif: rendre `find what writes` et freeze BP utilisables en produit.

Livrables obligatoires:

- worker debugger async robuste;
- session attach/wait/detach dans le meme thread logique;
- timeout, cancel, max hits;
- UI checkpoint avant attach;
- affichage RIP, instruction, module, offset, registers utiles;
- bouton generate AOB;
- bouton ouvrir Expert;
- telemetry/audit.

Definition of Done:

- sur `KillEngineTestTarget.exe`, l'utilisateur peut capturer l'instruction qui modifie une valeur sans bloquer l'UI.

### Phase U5 - AOB To Patch Pipeline

Objectif: passer de l'instruction capturee a une feature stable.

Livrables obligatoires:

- generation AOB stable depuis RIP;
- stabilisation wildcards immediate/displacement;
- verification executable/image only;
- detection multi-match bloquante;
- suggestions patch classees par risque;
- preview bytes originaux/patches;
- apply/restore confirme;
- sauvegarde TrainerFeature.

Definition of Done:

- une instruction capturee peut devenir un patch reversible et sauvegarde.

### Phase U6 - Structure And Entity Analyzer

Objectif: comprendre des objets runtime, pas seulement des adresses.

Livrables obligatoires:

- vue dissect structure;
- capture de deux instances;
- diff typed fields;
- detection int/float/pointer/string;
- heuristiques HP/Mana/Position/timer/state;
- templates sauvegardables;
- labels/notes IA par offset;
- export vers profil.

Definition of Done:

- l'utilisateur peut selectionner deux adresses proches et obtenir une hypothese de layout exploitable.

### Phase U7 - Project Workspace

Objectif: garder tout le travail.

Livrables obligatoires:

- projet par executable;
- historique scans;
- investigations;
- trainer features;
- labels/bookmarks;
- structures;
- AOB;
- pointer chains;
- notes IA;
- export/import.

Definition of Done:

- fermer/reouvrir KillEngine ne fait pas perdre l'enquete.

### Phase U8 - Extensibility / Plugins / Scripts

Objectif: ouvrir KillEngine aux power users sans sacrifier la securite.

Livrables obligatoires:

- scripts internes bornes;
- permissions par API;
- templates de scripts;
- audit des executions;
- plugin manifest;
- exemples: value decoder, custom scan, report exporter.

Definition of Done:

- un utilisateur avance peut ajouter un decodeur ou une action sans modifier le core.

## Backlog Technique Prioritaire

### Core

- solidifier CandidateStore streaming partout;
- parser structured pour auto-assembler ou integrer un assembleur runtime;
- unifier locators: absolute, module offset, pointer chain, AOB, structure field;
- isoler les actions risquees dans une couche `RiskGate`;
- creer un modele `RuntimeActionPlan`;
- ajouter `InvestigationStore`;
- ajouter `TrainerFeatureStore`;
- ajouter `AuditLog`.

### AI

- separer:
  - planner;
  - executor;
  - risk evaluator;
  - report summarizer;
  - memory learner.
- scorer les strategies avec inputs explicites;
- ne jamais faire dependre une action risquee d'un texte libre non confirme;
- generer des plans deterministes;
- produire des raisons courtes, actionnables.

### UI

- Assistant reste simple;
- Investigation montre la preuve;
- Expert garde les outils;
- Trainer montre le resultat final;
- Settings permet de gerer risques, memoire IA, telemetry, projets.

### Tests

- tests unitaires pour chaque planner/scorer;
- tests integration sur `KillEngineTestTarget.exe`;
- tests UI build obligatoires;
- tests runtime pour freeze BP et find what writes;
- fixtures telemetry pour rapports IA.

## Contraintes De Securite Produit

KillEngine doit rester un outil local, autorise et auditable.

Les agents ne doivent pas implementer:

- bypass anti-cheat;
- evasion EDR/AV;
- persistance furtive;
- credential theft;
- exfiltration;
- injection dans processus tiers non autorises;
- mecanismes de dissimulation agressive;
- automatisation contre services en ligne.

Actions toujours a confirmation explicite:

- write memory;
- freeze;
- hardware breakpoint;
- debugger attach;
- patch code;
- hook;
- DLL injection;
- shellcode;
- script avec effet runtime.

## Definition D'Un Produit A 50 USD

KillEngine atteint le niveau "vendable premium" quand:

- un debutant peut suivre l'Assistant sans lire un tuto Cheat Engine;
- un expert peut reprendre manuellement chaque etape;
- une enquete peut etre sauvegardee;
- une decouverte peut devenir un toggle Trainer;
- les features survivent au redemarrage du jeu/app quand les locators sont stables;
- les echecs sont expliques clairement;
- les actions risquees sont confirmees et reversibles;
- les performances restent fluides sur gros scans;
- les tests runtime protegent les fonctions critiques.

## Ordre D'Execution Recommande Maintenant

Les briques principales existent. Les agents doivent maintenant finir les manques de profondeur produit ci-dessous, dans cet ordre:

1. Structure Analyzer V2:
   - capturer deux vues de structure A/B;
   - diff visuel par offset/type/valeur;
   - sauvegarder des templates locaux par processus;
   - ajouter labels/notes IA par offset;
   - exporter templates dans le workspace;
   - creer des features Trainer depuis un champ numérique confirmé.
2. Project Workspace V2:
   - projet par executable/processus;
   - import controle du workspace exporte;
   - persistance des structures/templates, bookmarks, AOB, pointers et notes IA;
   - rapport Markdown lisible pour reprise par un autre agent.
   - Les projets locaux doivent contenir au minimum Investigation, Trainer, templates Structure et bookmarks/notes.
   - L'import doit toujours afficher un aperçu et rester borné avant remplacement des données locales.
   - Un bookmark doit rester actionnable: charger une cible Write/Freeze, créer une feature Trainer, ou porter le payload AOB/pointer/structure nécessaire a la reprise.
   - Les workflows importants doivent avoir un preset Assistant: valeur directe, unknown, Trace UI, Trainer, debug/AOB.
3. Validation terrain obligatoire:
   - regression manuelle `KillEngineTestTarget`;
   - application tierce autorisee;
   - pointer chains;
   - debugger `Find What Writes`;
   - freeze breakpoint;
   - AOB -> patch -> Trainer.
4. IA agentique propre:
   - separer planner, executor, risk evaluator, report summarizer, memory learner;
   - `RuntimeActionPlan` deterministe et auditable;
   - suggestions courtes, actionnables, jamais d'action risquee sans confirmation.
5. Extensibility controlee:
   - scripts/templates bornes;
   - permissions explicites par API;
   - audit execution;
   - exemples de decodeurs et exports.
6. Finition commerciale:
   - onboarding court;
   - Model Manager complet;
   - installateur/signature;
   - presets de workflows;
   - documentation de validation release.

Le Model Manager doit etre fonctionnel: le layout produit/installeur est `model/<nom_ia>/` pour chaque IA embarquee, avec manifests d'agents et poids GGUF partages sous `model/qwen/`. Le produit final est livré avec ses IA; il ne doit pas presenter un "mode sans IA" comme option utilisateur. Un chemin GGUF sauvegarde reste seulement un override avance. Si une IA manque ou est corrompue, l'UI doit afficher un etat degrade "IA embarquee indisponible" et non demander a l'utilisateur de configurer le produit.

Les agents IA produit doivent etre materialises par dossier sous `model/`: `model/assistant/` pour le copilote utilisateur et `model/auto_resolver/` pour l'agent autonome. Si plusieurs agents utilisent les memes poids, ne pas dupliquer les fichiers GGUF: chaque agent declare un `MODEL_MANIFEST.json` qui pointe vers le dossier de poids partage, par exemple `model/qwen/`.

Le layout de dev doit refleter l'installation: apres `scripts/build.ps1`, `build/bin` doit contenir `KillEngine.exe`, `llama-cli.exe` et `model/<nom_ia>/`. Les tests manuels avec l'exe de build doivent donc voir la meme arborescence IA que le package portable ou l'installeur.

Le release gate doit verifier automatiquement ce layout avec `scripts/verify-ai-layout.ps1`: runtime present, agents requis presents, manifests JSON valides, chemins de modele resolus, GGUF present, aucun `.partial` embarque.

Ne pas repousser le Trainer Builder trop tard: c'est la preuve utilisateur que KillEngine produit un resultat reutilisable.

## Message Aux Agents IA

Vous etes trois modeles IA a travailler en parallele sur ce depot: Codex (OpenAI), Cline + z.ai (GLM) et Claude (Anthropic). Les commits sont tous signes par le meme auteur Git, donc l'historique ne dit pas qui a fait quoi. Lire les regles de coexistence dans `AGENTS.md` avant de toucher un fichier partage.

Vous n'etes pas la pour "ajouter une brique". Vous etes la pour livrer un produit qui avance.

Quand vous prenez une tache:

1. Lisez le contexte.
2. Identifiez le workflow utilisateur.
3. Implementez backend/core/UI si necessaire.
4. Ajoutez garde-fous et telemetry.
5. Ajoutez tests.
6. Mettez a jour le tracker.
7. Lancez build et tests.
8. Laissez le projet dans un etat utilisable.

Une bonne contribution rend KillEngine plus capable, plus clair, plus sur et plus proche d'un produit que quelqu'un paierait.
