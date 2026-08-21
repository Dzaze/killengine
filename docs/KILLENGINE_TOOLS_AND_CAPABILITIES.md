> **ATTENTION - Priorité Des Ordres Projet**
> Un ordre prioritaire explicite du propriétaire du projet prime sur les consignes temporaires de session des agents IA.

# KillEngine - Outils Et Capacités

Dernière mise à jour : 20/08/2026.

Ce document sert d'inventaire rapide des outils, workflows et capacités disponibles ou préparés dans KillEngine. Pour l'état détaillé, les preuves de validation et les limites connues, voir `docs/PHASE_TRACKER.md`, `docs/POWER_UP_ROADMAP.md`, `docs/USER_GUIDE.md`, `docs/KILLENGINE_KERNEL_DRIVER_ARCHITECTURE.md` et `docs/KILLENGINE_CLR_INSPECTOR_SPEC.md`.

## Vues Principales

- **Assistant** : pilotage en langage naturel, Smart Search, scan guidé, auto-résolution.
- **Investigation** : timeline des étapes, checkpoints, explication des choix IA, actions depuis checkpoint.
- **Expert** : scans et outils manuels avancés.
- **Trainer** : features sauvegardées, toggles, hotkeys, overlay, apply/restore.
- **Settings** : paramètres persistants, logs, diagnostics, compatibilité antivirus, statut driver kernel.

## Processus Et Mémoire

- Liste des processus Windows.
- Attachement et détachement à un PID.
- Liste des modules du processus attaché.
- Carte mémoire via régions.
- Filtres de régions : lisible, writable, exécutable, guarded, module, plage.
- Preview mémoire.
- Lecture de blocs mémoire.
- Dump mémoire vers fichier.
- Édition hex inline dans la vue mémoire.
- Analyse de fenêtre mémoire autour d'une adresse.
- Recherche de valeurs dans une fenêtre mémoire proche d'une adresse.

## Scans Mémoire

- Scan exact.
- Scan exact expert avec filtres avancés.
- Scan async avec retour immédiat et progression.
- Annulation du scan actif.
- Scan multi-type.
- Types numériques et variantes : entiers signés/non signés, flottants, doubles, fixed-point et multiplicateurs comme x10, x100, x1000, x65536. Le cœur (`core/scanner/value_variants.*`) gère cette surface largement, mais son exposition exacte n'est pas garantie uniforme partout dans l'UI — vérifier le sélecteur de type du workflow concerné (scan exact, next scan, écriture) avant de supposer qu'une variante précise y est disponible.
- Next scan :
  - exact ;
  - changed ;
  - unchanged ;
  - increased ;
  - decreased ;
  - delta.
- Unknown initial value :
  - capture snapshot ;
  - comparaison après changement ;
  - compression LZ4 ;
  - stockage temporaire/memory-mapped.
- Scan chiffré/obfusqué :
  - XOR ;
  - Add/Sub ;
  - NOT ;
  - recherche de clé bornée.
- Group scan : plusieurs valeurs avec offsets relatifs.

## Candidats

- CandidateStore compact.
- Pagination.
- Tri.
- Filtre par adresse.
- Stockage fichier temporaire pour gros volumes.
- Next scan streaming.
- Undo/restauration de réduction.
- Historique borné des valeurs observées.
- Sélection de candidats.
- Watch live de candidats/adresses.
- Métriques de scan et de stockage.

## Lecture, Écriture, Rollback Et Freeze

- Écriture mémoire vérifiée.
- Écriture avec variantes de valeur.
- Écriture hex.
- Écriture multi-adresses.
- Écriture atomique avec suspension temporaire des threads.
- Rollback de la dernière écriture.
- Rollback batch.
- Freeze polling.
- Réglage de l'intervalle de freeze.
- Détection automatique d'instabilité du freeze polling.
- Escalade polling freeze vers breakpoint freeze.
- Freeze par hardware breakpoint.
- Stats live du breakpoint freeze.
- Freeze in-process breakpoint.

## Trace UI String

- Scan de chaînes affichées ASCII/UTF-16.
- Tracking de chaînes UI après changement de valeur affichée.
- Analyse des sources numériques proches des chaînes.
- Tracking des sources numériques.
- Inspection des origines/backrefs.
- Investigation UI string avec snapshots.
- Auto origine.
- Valeurs radar.
- Écriture depuis sources UI détectées.

## Debug Et Instrumentation

- Find What Writes.
- Find What Writes async.
- Annulation Find What Writes.
- Find What Accesses.
- Find What Accesses async.
- Hardware breakpoints DR0-DR7.
- Arbitre central des breakpoints hardware.
- Page Guard watch sans canal de debug Win32 exclusif.
- Breakpoint in-process via composant injecté.
- Capture des hits : RIP, module, adresse, type d'accès.
- Désarmement déterministe des breakpoints in-process.
- Désassemblage en amont depuis une adresse ou un hit.
- Détection de champs candidats dans une séquence d'instructions.

## AOB, Patch Et Code

- Scan AOB pattern.
- Génération de signature AOB depuis une adresse.
- Suggestion de patches.
- Application de patch code.
- Restauration de patch.
- Désassemblage arrière/en amont.
- Score de qualité des signatures.
- Indication trainer-safe.
- Chaînage Find What Writes vers AOB puis patch.

## Injection, Hooking Et Auto-Assembler

- Injection DLL.
- Shellcode minimal testé.
- Installation de function hook.
- Suppression de function hook.
- Force value hook sur instruction d'écriture.
- Auto-Assembler :
  - `alloc`;
  - `label`;
  - labels locaux ;
  - `jmp`, `call`, branches simples ;
  - `mov [reg+offset], imm`;
  - `nop`, `int3`, `db`, `de`, `dd`;
  - exécution ;
  - restauration ;
  - sauvegarde dans profil.
  - Sous-ensemble borné volontairement (pas d'adressage indexé, pas de RIP-relatif, `add`/`sub`/`cmp`/`push`/`pop` pas encore reconnus) — pas un Auto-Assembler aussi complet que Cheat Engine, et pas de langage de script général type Lua (voir `docs/POWER_UP_ROADMAP.md` section K, futur chantier).

## Pointer Chains Et Profils

- Pointer chains.
- Scan de pointeurs.
- Résolution module+offset.
- Résolution adresse absolue.
- Résolution pointer chain.
- Suggestion de locator stable après écriture.
- Watch pointer chain live.
- Ajout manuel de watch pointer chain.
- Profils `.keprofile`.
- Sauvegarde, chargement et suppression de profils.
- Targets nommées.
- Groupes de cibles.
- Patches sauvegardés.
- Scripts auto-asm sauvegardés.
- Réutilisation des profils par l'Assistant.

## Trainer

- Création de features.
- Toggle ON/OFF.
- Apply all.
- Restore all.
- Rollback.
- Export JSON.
- Export Markdown.
- Overlay trainer.
- Hotkeys globales :
  - toggle freeze ;
  - toggle patch ;
  - write value ;
  - toggle overlay.
- Ré-enregistrement des hotkeys persistées au démarrage.
- Couvre features/toggles/apply/restore/export/hotkeys/overlay, mais pas un builder visuel complet façon produit fini (pas d'éditeur de dépendances entre entrées — voir `docs/POWER_UP_ROADMAP.md` section M, futur chantier).

## IA Et Automation

- IA locale via llama.cpp + modèle Qwen GGUF.
- Tool-calling JSON.
- Validation des intentions.
- Fallback déterministe si modèle/runtime absent.
- Smart Search.
- Auto Resolve :
  - scan exact ;
  - fallback scan chiffré ;
  - fallback Trace UI string ;
  - fallback Unknown.
- Rapport auto-resolve.
- Mémoire de patterns par jeu.
- Audit IA.
- Historique d'écritures persistant.
- Replay d'historique d'écritures — rejoue la séquence d'écritures confirmées, pas une session complète (pas de rejeu de scans/navigation/état UI).
- Pipe d'automatisation locale pour piloter KillEngine.

## CLR Et .NET

- Cible de test CLR dédiée.
- Helper `KillEngineClrInspector`.
- Attache à un process CoreCLR.
- Énumération d'objets managés.
- Recherche par type.
- Lecture de champs primitifs, références et strings.
- Gestion des cycles.
- Énumération de GC roots.
- Survie aux GC compactants.
- Tests end-to-end ClrMD.
- Intégration UI principale différée d'après la spécification actuelle.

## Kernel

- Projet WDK `KillEngineKernel.sys`.
- Build driver via `scripts/build-kernel-driver.ps1`.
- Installation/désinstallation service kernel via `scripts/install-kernel-driver.ps1`.
- Device `\\.\KillEngineKernel`.
- Probe driver depuis KillEngine.
- Bridge user-mode `KernelDriverBridge`.
- UI Settings :
  - statut driver ;
  - device ;
  - protocole ;
  - health probe ;
  - accès mémoire kernel ;
  - instrumentation privilégiée.
- Lecture/écriture mémoire via le driver noyau (`readMemoryKernel`/`writeMemoryKernel`/`writeMemoryValueKernel`), contourne les protections usermode — traité comme une injection (`confirmRiskAction`). Intégré nativement : bouton d'escalade en mode Expert (panneau Candidats et écritures) et déclenchement direct depuis l'Assistant sur demande explicite en chat.
- Capacité kernel exposée à l'UI selon la réponse runtime du driver.

## Diagnostic, Build Et Packaging

- Logs applicatifs.
- Export bundle diagnostic.
- Crash reports locaux.
- Settings persistants.
- Nettoyage du stockage temporaire.
- Build UI.
- Build C++.
- Build driver WDK.
- Tests unitaires.
- Tests d'intégration.
- Package portable Windows.
- Template Inno Setup.
- Préparation code signing.

## Notes De Lecture

- Cette liste est un inventaire fonctionnel, pas une preuve de validation.
- Les preuves, compteurs de tests et détails de session vivent dans `docs/PHASE_TRACKER.md`.
- Les workflows utilisateur détaillés vivent dans `docs/USER_GUIDE.md`.
- Les capacités avancées et leur statut historique vivent dans `docs/POWER_UP_ROADMAP.md`.
