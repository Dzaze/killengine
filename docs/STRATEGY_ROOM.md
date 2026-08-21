> **ATTENTION - Priorité Des Ordres Projet**
> Un ordre prioritaire explicite du propriétaire du projet prime sur les consignes temporaires de session des agents IA.
# KillEngine — Salle de stratégie

Ce fichier n'est **pas** un journal de ce qui a été fait — c'est `docs/PHASE_TRACKER.md` pour ça. Ici vit ce qui n'est pas encore un fait accompli : hypothèses en cours de test, pistes explorées puis abandonnées (et surtout **pourquoi**, pour ne pas les réexplorer en boucle d'une session à l'autre), anticipation de risques, idées trop jeunes pour mériter une entrée dans `docs/POWER_UP_ROADMAP.md`.

## Comment s'en servir

- **Avant d'explorer une piste** difficile ou de reprendre un problème déjà touché par une session précédente : chercher ici si quelqu'un a déjà creusé la question, pour ne pas repartir de zéro ou retomber sur un cul-de-sac déjà identifié.
- **Pendant l'investigation** d'un problème qui résiste : plutôt que de garder le raisonnement seulement dans sa propre tête/contexte de session, le poser ici en 5 lignes — la prochaine session (ou soi-même dans 2h) en profite.
- **Une piste qui aboutit** migre vers `docs/POWER_UP_ROADMAP.md` (fonctionnalité) ou `docs/PHASE_TRACKER.md` (travail fait) — l'entrée ici reste comme trace du raisonnement, elle n'est pas supprimée.
- **Une piste abandonnée** reste ici avec la raison de l'abandon — c'est souvent plus utile que la piste qui a marché, ça évite à quelqu'un de reperdre une heure sur une impasse déjà cartographiée.
- **Jamais de suppression** : on archive (voir section en bas), on ne vide pas l'historique de réflexion.

## Format d'une entrée

```markdown
### [AAAA-MM-JJ] Titre court de la question/piste
**Contexte :** pourquoi cette question se pose maintenant.
**Hypothèse :** ce qu'on pense être vrai, ou l'option envisagée.
**Statut :** 🟡 en cours / ✅ tranché (→ voir résultat) / ❌ abandonné (→ voir raison) / 💤 en veille (pas urgent, pas oublié)
**Lié à :** fichier(s) ou entrée de PHASE_TRACKER.md concernés.
```

---

## Sommaire (mis à jour à chaque nouvelle entrée)

| Date | Titre | Statut |
| --- | --- | --- |
| 2026-08-19 | Rôle sémantique auto-détecté pour la mémoire de pattern | ✅ tranché (option 2, inféré) |
| 2026-08-19 | Attribution du contenu non commité trouvé sans commit associé | ❌ abandonné (git ne peut pas trancher) |
| 2026-08-19 | Regroupement Cline/Codex hors-ligne jusqu'au 20/08/2026 — impact sur la stratégie de session | 💤 en veille |
| 2026-08-19 | Microsoft Solitaire Collection résiste à l'édition mémoire classique (XP/Score) — session de test live via le connecteur d'automatisation | 🟡 en cours (Score débloqué, XP non) |
| 2026-08-20 | Reprise investigation XP Solitaire — 3ᵉ copie/pointeur/réseau testés, cause probable identifiée (tas .NET managé) | ✅ tranché (mécanisme compris) — 🟡 XP toujours non contrôlable, outillage manquant identifié |
| 2026-08-20 | Vérification candidat #8 — Solitaire.exe n'est PAS une cible CLR (pas de COR20 header), hypothèse "tas .NET managé" invalidée | ❌ abandonné pour Solitaire précisément (→ voir raison) — 💤 ClrMD reste en veille comme capacité générale |
| 2026-08-19 | Hypothèses d'amélioration KillEngine issues de la session Solitaire (visibilité temps réel via le pipe + logs) | ✅ 6/6 livrées (PHASE 26) |
| 2026-08-20 | Cycle de vie des breakpoints matériels : arbitre DR0-DR7, désarmement in-process déterministe, `scanMemoryWindow` dédié | ✅ tranché — protections livrées et testées, régression injection root-causée (EDR Microsoft Defender for Endpoint, pas un bug KillEngine) et mitigée |
| 2026-08-20 | Écriture kernel-mode (driver `KillEngineKernel.sys`) testée sur l'XP Solitaire — même conclusion que l'écriture usermode | ✅ tranché (hypothèse "détection d'écriture" définitivement éliminée) — 🟡 XP toujours non contrôlable, nouvelle piste identifiée (remonter à la fonction de calcul) |
| 2026-08-20 | **XP Solitaire enfin contrôlable** — remontée du désassemblage depuis l'animation d'affichage jusqu'au vrai champ cible (`[RSI+0x908]`), écriture kernel confirmée persistante | ✅ **résolu** — champ identifié, écriture validée deux fois (valeur tenue, gain suivant additionné par-dessus) |

---

### [2026-08-19] Rôle sémantique auto-détecté pour la mémoire de pattern
**Contexte :** `getRememberedPatterns()`/`rememberedPatterns` (roadmap H.2, livré le 19/08/2026) sait *où* une valeur a été confirmée (module + offset + type) mais pas *ce que c'est* ("argent", "vie", "score"...). Le label actuel est l'événement d'audit technique (`checkpoint_write_executed`), pas un nom lisible.

**Hypothèse :** deux approches possibles, pas encore arbitrées :
1. **Déclaratif** — demander explicitement à l'utilisateur un nom au moment de la confirmation ("comment veux-tu appeler cette valeur ?"), simple mais ajoute une friction UX à un flux qui se veut sans friction.
2. **Inféré** — relier le nom que l'utilisateur a tapé dans le chat Assistant au moment de la recherche initiale (ex. "trouve mes minerais") au pattern confirmé ensuite. Zéro friction ajoutée, mais demande de faire remonter ce contexte texte jusqu'à `logAiAudit`, qui aujourd'hui ne reçoit qu'`address`/`type`/`aobPattern` — pas le texte de la requête d'origine.

**Statut :** ✅ tranché et livré le 19/08/2026 (option 2, inféré). Le fil requête → candidat → confirmation n'a pas eu besoin d'être construit : il existait déjà. Le wrapper frontend `logAiAudit()` (`ui/src/stores/app.ts`) injecte `objective: activeInvestigation.value?.objective ?? searchQuery.value` sur **chaque** appel, sans exception — cette info remontait donc déjà jusqu'au backend à chaque écriture confirmée, juste jamais lue côté C++. Corrigé côté `ApplicationController::logAiAudit` (`apps/desktop/application_controller.cpp`) : l'`objective` reçu devient `queryLabel` dans l'entrée `rememberedPatterns`, avec une liste d'objectifs génériques exclus (`"nouvelle recherche"`, `"investigation manuelle"`, etc. — des placeholders de reset de contexte, pas de vraies phrases utilisateur) pour ne pas figer un faux rôle à leur place ; si l'objectif de la confirmation en cours est générique, le `queryLabel` précédemment mémorisé pour cette cible est conservé plutôt qu'écrasé. Exposé par `getRememberedPatterns()`, affiché en premier (avant le module+offset technique) dans `SettingsView.vue`. Aucune nouvelle donnée frontend à tracer — confirme après-coup que l'hypothèse de friction ("demande de faire remonter ce contexte texte jusqu'à logAiAudit") était en fait déjà résolue par le code existant, juste jamais exploitée.

**Lié à :** `docs/POWER_UP_ROADMAP.md` section H.2, `docs/PHASE_TRACKER.md` PHASE 23, `ApplicationController::logAiAudit`/`getRememberedPatterns` (`apps/desktop/application_controller.cpp`).

---

### [2026-08-19] Attribution du contenu non commité trouvé sans commit associé
**Contexte :** `.editorconfig` et une section entière d'`AGENTS.md` (tableau de conventions de formatage) sont apparus dans l'arbre de travail sans entrée dans `PHASE_TRACKER.md`, ni aucun commit associé. Besoin de savoir quel agent les a écrits pour comprendre le contexte.

**Hypothèse :** vérifier via `git diff`/mtimes si l'origine est traçable.

**Statut :** ❌ abandonné — pas une impasse technique mais une limite structurelle : les 3 agents résidents (Codex/Cline+GLM/Claude) committent tous sous la même identité git `Dzaze`, et le contenu en question n'est même pas commité. Aucune métadonnée ne permet de trancher. Seul indice temporel faible (mtime) suggère un ordre probable, jamais une certitude. **Conclusion pour les prochaines sessions :** ne pas perdre de temps à enquêter sur l'auteur d'un contenu non commité dans ce dépôt — consigner ce qui existe et pourquoi c'est correct/incorrect suffit, l'attribution n'est pas récupérable après coup.

**Lié à :** `docs/PHASE_TRACKER.md`, entrée "PHASE 21 SUITE".

---

### [2026-08-19] Regroupement Cline/Codex hors-ligne jusqu'au 20/08/2026 — impact sur la stratégie de session
**Contexte :** l'utilisateur a signalé que Cline et Codex sont hors service depuis le 19/08/2026 au matin et jusqu'au **20/08/2026** (corrigé le 19/08 — pas "20 mai" comme noté par erreur dans la première version de cette entrée) — Claude est donc seul à travailler sur ce dépôt pendant une fenêtre très courte (le lendemain), pas une longue période.

**Hypothèse :** pendant cette fenêtre, les règles de coexistence multi-agents (édition ciblée, relire avant d'écrire, `PHASE_TRACKER.md` comme point de rendez-vous) restent valables — elles ne sont pas là uniquement pour éviter les collisions temps réel, mais aussi pour que **Cline/Codex retrouvent un état cohérent et documenté à leur retour**. Ne pas relâcher la discipline juste parce que personne d'autre ne touche le dépôt "maintenant", d'autant plus que le retour est imminent (dès le 20/08).

**Statut :** 💤 en veille — pas une action à prendre, juste un rappel de contexte pour ne pas dériver vers des habitudes moins soigneuses sous prétexte de solo. Fenêtre courte : pas la peine de la re-vérifier, elle sera de toute façon close au 20/08/2026.

**Lié à :** `AGENTS.md` section "Équipe d'agents".

---

### [2026-08-19] Microsoft Solitaire Collection résiste à l'édition mémoire classique (XP/Score)

**Contexte :** première session de test live utilisant le connecteur d'automatisation (`apps/desktop/automation_pipe_server.h`, `docs/PHASE_TRACKER.md` PHASE 24) sur une cible réelle : l'utilisateur voulait augmenter son XP dans Microsoft Solitaire Collection (`Solitaire.exe`, app UWP/MSIX), pendant que l'agent pilotait KillEngine via le pipe en même temps que l'utilisateur jouait et rapportait les valeurs affichées. Objectif secondaire atteint au passage : validation du connecteur en usage réel prolongé (dizaines d'appels, plusieurs heures), pas juste un smoke test isolé.

**Ce qui a été essayé, dans l'ordre, et pourquoi chaque étape a échoué avant de trouver ce qui marche :**

1. **Scan exact direct sur la valeur affichée** (`30`, puis `60`, `90`...) en `Int32` : converge bien via next scan (15609 → 4 → 3 candidats sur plusieurs manches), mais **aucun des candidats trouvés ne pilotait l'affichage** — écrire dedans (vérifié en mémoire) ne changeait rien à l'écran. Byte size incorrect : les candidats étaient en fait des champs `Int64` tronqués par un scan `Int32` (voir point 4).
2. **Trace UI string** (`scanUiStrings("90")` → `trackUiStringCandidates`) : bruit énorme au départ (2454 correspondances pour un nombre à 2 chiffres dans une grosse app), réduit à 17 survivants après un changement de valeur réel — mais `analyzeUiStringSources` n'a trouvé **aucune source numérique adjacente** (`Int32`/x100/x65536) pour aucun des 17. Conclusion : ce n'est pas un moteur natif avec un entier brut à côté du texte affiché — cohérent avec une UI XAML/managée où texte et données ne sont pas dans la même structure mémoire contiguë.
3. **Scan direct en `Float64`** (hypothèse : moteur JS-like, nombres stockés en double) : réduit à 11 candidats pour `120`, mais **0/11 n'a matché `150`** au changement suivant — tous des coïncidences (probablement des constantes de layout UI à `120.0`).
4. **Scan direct en `Int64`** : bien plus propre (222 candidats pour `150`, réduit à 2 pour `180` puis constamment 2 sur 8 manches de suite, valeurs 30→60→90→120→150→180→210→240). **Les 2 survivants sont exactement les 2 adresses trouvées à l'étape 1** (`Int32` matchait leurs 4 octets bas) — confirmation que la vraie représentation est bien `Int64`, pas `Int32`.
5. **Écriture directe sur une seule des 2 adresses** (valeurs test `777`, `9999`) : vérifiée en mémoire immédiatement après écriture, mais **toujours corrigée silencieusement** par le jeu à la vraie valeur au prochain changement naturel (ex. écrit `9999`, l'utilisateur joue, `210 → 240` naturel, et les 2 adresses affichent bien `240`, pas `9999`). Les deux adresses restent **en permanence synchronisées entre elles et avec la vraie valeur** — ce ne sont pas des coïncidences, c'est une paire de copies redondantes.
6. **Find What Writes** (breakpoint matériel) sur l'une des 2 adresses, plusieurs tentatives synchronisées avec l'utilisateur (jouer un As = +30 XP direct, action courte et contrôlable) : **0 capture** sur la première adresse testée après plusieurs essais malgré un timing correct. Sur la **seconde** adresse (`...ac268`) au relancement d'une partie (donc écriture `0 → 30` à l'init), **capture réussie** : RIP `Solitaire.exe+0x3EB3FB`, 8 écritures identiques.
7. **`generateAobSignature`/`suggestCodePatches` sur ce RIP** : `ReadProcessMemory failed (error=299)` — **le code de l'exécutable n'est pas lisible depuis l'extérieur du processus**, cohérent avec une protection de signature/anti-lecture sur du contenu Microsoft Store packagé. La voie patch/hook est donc fermée pour cette instruction précise, alors même que le breakpoint matériel a pu observer l'écriture (l'observation CPU/registres ne nécessite pas de lire les octets de l'instruction, contrairement à un patch).
8. **Freeze (réécriture continue, 16ms) sur une seule des 2 adresses** : **a fait crasher `Solitaire.exe`** (process relancé avec un nouveau PID juste après). Hypothèse la plus probable : pas nécessairement un vrai anti-triche actif, plutôt une violation d'invariant interne — si ce champ fait partie d'une paire vérifiée/comparée en continu par le jeu, le maintenir de force à une valeur différente de sa jumelle pendant que le jeu tente de la relire/recopier peut suffire à planter un moteur managé sans protection dédiée. Non confirmé, mais suffisant pour ne plus recommander cette technique sur ce champ précis.
9. **✅ Écriture simultanée sur les 2 adresses jumelles à la fois** (au lieu d'une seule) — **hypothèse proposée par l'utilisateur lui-même** ("la modification doit s'écrire sur les deux adresses en même temps"), testée d'abord sur le **Score** (`200a24db924`/`200ad9f88cc`, valeur test `8000`) : **succès confirmé à l'écran**. Le mécanisme de correction silencieuse observé au point 5 est cohérent avec une vérification "les deux copies doivent être égales, sinon je resynchronise à partir de l'autre" — écrire les deux en même temps ne laisse jamais de désaccord à détecter.
10. **Même technique (écriture double simultanée) retentée sur l'XP** (nouvelles adresses après relance de partie, revalidées par un nouveau scan `Int64` propre) : **échec**, l'écran n'a pas bougé. **L'XP a donc une protection strictement plus robuste que le Score** — au minimum une 3ᵉ copie/checksum non identifiée, ou une validation qui ne se limite pas à comparer 2 copies locales (possible synchronisation compte/serveur, l'app ayant une intégration Xbox/Microsoft Rewards documentée par son éditeur).

**Statut :** 🟡 en cours, pas fermé. **Score débloqué et confirmé** (technique de l'écriture simultanée multi-adresses, jamais testée aussi explicitement avant cette session). **XP toujours protégée**, cause exacte non identifiée — dernier point testé et non concluant, pas une impasse formellement démontrée comme le sont les points 1-3.

**Pistes à explorer pour une future session, par ordre de coût croissant :**
- Relire les 2 adresses XP juste après un Find What Writes réussi côté data (pas juste côté RIP) pour voir s'il existe une 3ᵉ zone mémoire lue/comparée au moment de l'écriture (poser un `readMemoryBlock` sur une plage large autour des 2 adresses au moment de la capture, pas juste les 8 octets).
- Essayer l'écriture simultanée avec un délai variable entre les deux écritures (0ms testé implicitement ici via deux appels séquentiels rapides sur le pipe — pas un vrai atomique) : si une fenêtre de tolérance existe, elle vaut la peine d'être mesurée plutôt que supposée nulle.
- Chercher si Microsoft Solitaire Collection expose une API/synchronisation Xbox Live visible côté réseau (`netstat`/proxy local) au moment d'un changement d'XP — si un round-trip serveur a lieu, ça confirmerait l'hypothèse "source de vérité pas 100% locale" et fermerait définitivement la piste memory-only.
- Envisager un vrai atomic write ("group write", voir candidat ajouté à `docs/POWER_UP_ROADMAP.md`) côté KillEngine : l'écriture double a été faite ici via deux appels pipe successifs (quelques ms d'écart réseau/IPC compris), pas une vraie écriture atomique en un seul `WriteProcessMemory`/lock — pourrait faire la différence sur une cible avec une fenêtre de comparaison plus stricte.

**Lié à :** `docs/PHASE_TRACKER.md` PHASE 24 (connecteur) et PHASE 25 (cette session), `apps/desktop/automation_pipe_server.h`, `scripts/automation-pipe-call.ps1`.

---

### [2026-08-19] Hypothèses d'amélioration KillEngine issues de la session Solitaire

**Contexte :** la session de test live sur Microsoft Solitaire Collection (entrée précédente) a donné une visibilité inhabituelle sur le produit — chaque scan/lecture/écriture/debugger/freeze a été observé en direct via le pipe, corrélé à une confirmation visuelle humaine immédiate, et tout est resté dans `scan_telemetry.jsonl`. Ce niveau de visibilité combiné a fait ressortir des trous produit qu'une session de dev classique (sans cible réelle qui résiste) ne révèle pas facilement. Six hypothèses distinctes, du plus mûr au moins mûr.

**H1 — Détection auto de "copies redondantes" parmi les candidats.** Sur Solitaire, les 2 adresses finales (Score comme XP) sont restées identiques et ont narrowed ensemble sur 8 cycles de next-scan consécutifs, sans jamais diverger. C'est un signal statistique fort et déjà disponible dans `candidateStore` — personne n'a besoin de le chercher manuellement. Hypothèse : si N candidats (N petit, genre 2-5) restent à la **même valeur** sur **plusieurs cycles de next-scan d'affilée** sans diverger, les marquer comme "groupe probable de copies liées" dans le résultat de `nextScan`/`getCandidates`, avec une suggestion explicite d'écriture groupée plutôt qu'individuelle.

**H2 — Détection de "l'écriture a été silencieusement annulée".** On a perdu du temps à écrire sur une adresse, demander une confirmation visuelle à l'utilisateur, et découvrir seulement au **prochain changement naturel du jeu** que la valeur avait été re-corrigée entre-temps. Hypothèse : après un `writeMemoryValue`/`writeMemoryValueConfirmed` réussi, programmer une **re-lecture automatique** de l'adresse quelques secondes plus tard (ou au prochain tick d'un watch actif) ; si la valeur ne correspond plus à ce qui a été écrit (et n'a pas été ré-écrite intentionnellement par l'utilisateur), générer un insight explicite ("cette adresse semble resynchronisée automatiquement — vérifier s'il existe une copie jumelle") au lieu de laisser l'utilisateur le découvrir par hasard.

**H3 — Écriture multi-adresses atomique/groupée.** Déjà promu en candidat roadmap (`docs/POWER_UP_ROADMAP.md` #7) suite à cette session — la technique qui a débloqué le Score (2 appels `writeMemoryValue` séquentiels rapides) a marché ici mais n'est pas un vrai atomique bas niveau ; une cible avec une fenêtre de comparaison plus stricte pourrait quand même détecter le désaccord entre les deux écritures.

**H4 — Message actionnable pour `ERROR_PARTIAL_COPY` (299) sur lecture de code.** `generateAobSignature`/`suggestCodePatches` ont renvoyé une erreur Win32 brute (`ReadProcessMemory failed... error=299`) sans aucune explication ; il a fallu creuser pour comprendre que c'est une protection anti-lecture de code, typique des apps Microsoft Store/UWP signées. Hypothèse : détecter spécifiquement ce code d'erreur dans `ApplicationController` et retourner un message diagnostique clair + une suggestion de pivot ("code protégé contre la lecture externe — essaie Freeze ou une écriture groupée sur la donnée plutôt qu'un patch du code") au lieu de l'erreur Win32 nue.

**H5 — Garde-fou avant Freeze sur un candidat jamais testé en écriture.** Le freeze à 16ms sur une adresse XP fraîchement trouvée (jamais validée par une écriture simple au préalable) a fait crasher le process cible. Hypothèse : dans l'UI (et idéalement dans le contrat retourné par `setFreezeValue`), signaler/avertir quand un freeze est activé sur une adresse qui n'a **jamais été write-testée avec succès visuel** avant — pas bloquant, juste un avertissement supplémentaire proportionné au risque observé concrètement cette session.

**H6 — Nouvelle dimension de confiance "write-verified" dans `candidate_confidence`.** Un candidat qui survit à 8 cycles de next-scan n'est **pas** la preuve qu'il pilote l'affichage — Solitaire l'a démontré deux fois (Score et XP). La confiance actuelle mesure la stabilité du *scan*, pas l'effet réel d'une écriture. Hypothèse (la moins mûre des 6, mérite discussion avant même de la garder) : ajouter une dimension distincte "confirmé par écriture visible" vs juste "stable au scan", pour que l'UI arrête de présenter un candidat scan-stable comme "prêt à écrire" avec la même confiance qu'un candidat déjà write-vérifié.

**Statut :** ✅ tranché et livré le 19/08/2026 — les 6 hypothèses ont toutes été implémentées suite à la demande explicite de l'utilisateur ("fait tout mon pote"). Détail complet dans `docs/PHASE_TRACKER.md` PHASE 26. Deux surprises en cours de route, utiles pour une future session :
- **H2 existait déjà** avant même d'être posée comme hypothèse — `registerWriteWatch`/`writeDidNotHold` (probablement une session antérieure). Le vrai gap était une couverture incomplète (`writeMemoryValue` n'appelait pas `registerWriteWatch`, contrairement à `writeMemoryValueConfirmed`), pas une fonctionnalité manquante. Leçon : vérifier plus systématiquement l'existant avant de proposer une hypothèse comme "nouvelle" — ça vaut aussi pour la salle de stratégie, pas seulement pour le code.
- **H3 (écriture atomique) a révélé un vrai bug par le test** : premier essai en auto-attachant KillEngine à lui-même a provoqué un deadlock (le thread exécutant l'écriture se suspendait lui-même). Corrigé avant que ça arrive en usage réel. Rappel que tester une nouvelle capability contre une cible pratique (même artificielle) trouve des bugs qu'une relecture de code ne trouve pas.

**Lié à :** entrée précédente (session Solitaire), `docs/PHASE_TRACKER.md` PHASE 25, `core/scanner/candidate_confidence.*` (H6), `apps/desktop/application_controller.cpp` (H1/H2/H4/H5).

---

### [2026-08-20] Cycle de vie des breakpoints matériels : arbitre DR0-DR7, désarmement in-process déterministe, `scanMemoryWindow` dédié

**Contexte :** pendant la reprise de la session Solitaire (nouvelle tentative sur l'XP), enchaînement d'un breakpoint in-process (`startInProcessBreakpointWatchAsync`) puis d'un `findWhatWrites` externe sur la même adresse, sans coordination entre les deux mécanismes — `Solitaire.exe` a crashé peu après, et `KillEngine.exe` s'est ensuite figé (thread GUI bloqué) en tentant de relire la mémoire d'un process déjà mort. L'utilisateur a explicitement demandé de traiter l'hypothèse "conflit DR0-DR7" comme **non prouvée** (pas de dump analysé pour ce crash précis) mais de construire les protections structurelles quand même, plus une correction ciblée d'une vraie faiblesse identifiée par le raisonnement : le désarmement in-process dépendait d'un hit futur hypothétique pour se produire.

**Ce qui a été construit :**
1. **`core/debug/breakpoint_arbiter.h/.cpp`** — `HwBreakpointArbiter`, singleton process-wide, un seul propriétaire logique des registres de debug par PID cible (états `Idle`/`Arming`/`Active`/`Disarming`/`Error`). `tryAcquire()` refuse immédiatement (pas d'attente) si un autre mécanisme détient déjà le PID. `release(pid, disarmConfirmed)` : si `disarmConfirmed=false`, le PID passe en `Error` ("empoisonné") et reste refusé jusqu'à `resetForPid()` explicite — sécurité par défaut, pas par optimisme. `HwBreakpointOwnershipGuard` (RAII) garantit la libération sur chaque chemin de sortie, y compris les exceptions. Câblé dans les 3 mécanismes qui touchent DR0-DR7 : `hardware_breakpoint.cpp` (`attach`/`detach`), `breakpoint_freeze.cpp` (`freezeLoop`), `inprocess_breakpoint.cpp` (`monitor`/`startFreeze`/`stop`).
2. **Désarmement in-process déterministe** (le point le plus important signalé par l'utilisateur) — avant : `InstallThread` (composant injecté) armait la thread appelante puis **se terminait immédiatement**, laissant le VEH désarmer `Dr7` seulement "au prochain hit" ; un timeout de capture sans aucune écriture interceptée laissait donc DR0 armé indéfiniment dans la cible pendant que KillEngine croyait déjà la ressource libre. Corrigé : `InstallThread` ne se termine plus après l'armement, elle boucle (`Sleep(20)`) en attendant `stopRequested`, puis désarme **activement** chacune des threads qu'elle a elle-même armées (liste bornée à 8 TIDs dans l'état IPC partagé, jamais une énumération de "toutes les threads du process" — ça reste le piège du tout premier crash de cette fonctionnalité) avant de confirmer via `state->disarmed`. Côté KillEngine, `waitForDeterministicDisarm()` attend jusqu'à 2s cette confirmation avant de libérer l'arbitre — au-delà, le PID est marqué `Error` plutôt que supposé propre.
3. **`core/scanner/memory_window_search.h/.cpp`** (`findValuesInMemoryWindow`) — primitive dédiée à la recherche générique d'une valeur dans une fenêtre mémoire autour d'une adresse binaire, remplace le détournement d'`analyzeUiStringSources` (conçue pour tracer une chaîne UI) fait un peu plus tôt dans cette même session pour chercher une 3ᵉ copie potentielle de l'XP. Exposée via le nouveau `Q_INVOKABLE ApplicationController::scanMemoryWindow(addressHex, value, options)`.
4. **Nettoyage automatique au changement de cible** — `ApplicationController::resetHardwareBreakpointStateForPreviousTarget()`, appelée en tête d'`attachProcess()` et `detachProcess()` : arrête au mieux les sessions in-process encore actives (borné à 2s par leur propre désarmement déterministe, ne peut donc pas bloquer longtemps même si la cible a déjà disparu) et force `HwBreakpointArbiter::resetForPid()` pour l'ancien PID, avec détection best-effort (juste pour le log) de si la cible précédente existe encore ou a disparu.

**Validé comment :** pas juste compilé — 8 nouveaux tests unitaires purs (`tests/unit/test_breakpoint_arbiter.cpp`, logique de l'arbitre sans hardware réel) + 4 nouveaux tests d'intégration sur `KillEngineTestTarget.exe` réel (`tests/integration/test_breakpoint_arbiter_runtime.cpp`) :
- `InProcessTimeoutWithoutHitDisarmsDeterministically` — reproduit précisément le piège corrigé (capture sans aucune écriture pendant la fenêtre), vérifie que l'arbitre revient bien à `Idle` et n'est pas empoisonné. **Passe.**
- `ConcurrentInProcessAndExternalIsRefused` — le cœur de la demande utilisateur : capture in-process maintenue active (stress rewrite réel), tentative `findWhatWrites` externe pendant ce temps, vérifiée refusée (0 hit, snapshot de l'arbitre inchangé). **Passe.**
- `SequentialCyclesAcrossFreshPidsInProcessThenExternal` (3 PID neufs) et `...ExternalThenInProcess` (2 PID neufs, sens inverse) — cycles répétés, vérifient qu'aucun état ne fuit d'un cycle/PID à l'autre.

**✅ Régression trouvée en écrivant ces tests, root-causée le 20/08/2026 (même session, sur demande explicite de l'utilisateur de creuser plutôt que de contourner) :** `SequentialCyclesAcrossFreshPidsExternalThenInProcess` échouait de façon reproductible juste après un cycle `findWhatWrites` externe complet — `VirtualAllocEx`/`CreateRemoteThread` retournaient error=5 (ACCÈS REFUSÉ) lors de l'injection in-process qui suivait sur la même cible.

**Démarche de root-cause (demandée explicitement, comparative A/B, pas de contournement avant compréhension) :** nouveau fichier `tests/integration/test_debug_detach_reproducer.cpp`, 9 scénarios (A à I) en Win32 pur, totalement indépendants du code KillEngine, pour bissecter précisément :
- Scénarios A à D et F (attach/detach pur, cycle Suspend/SetThreadContext/Resume, mapping partagé nommé, ordre d'ouverture du handle) : tous **réussissent** de façon fiable — élimine le port de debug seul, le cycle de threads, le mapping partagé et l'ordre d'ouverture du handle comme causes.
- Scénario E (le vrai `findWhatWrites()` + le vrai `InProcessBreakpointSession::monitor()`, sans boucle ni retry) : **échoue** de façon fiable dès la première tentative — élimine tout artefact de boucle/retry.
- Scénario G (chaîne d'injection complète en Win32 pur — `VirtualAllocEx` → `WriteProcessMemory` → `CreateRemoteThread`, exactement la séquence d'`injectDll`) : **échoue** de façon fiable, systématiquement sur `CreateRemoteThread` (parfois `VirtualAllocEx` selon le run) — la seule différence avec les scénarios A-D/F qui réussissaient : ceux-ci ne testaient jamais `CreateRemoteThread`, seulement `VirtualAllocEx` isolé.
- Scénario H (même séquence avec `PROCESS_ALL_ACCESS` littéral au lieu de nos flags restreints) : échoue pareil — élimine les flags d'accès comme cause.
- Scénario I (délai de 3s avant `CreateRemoteThread`) : échoue pareil (et cette fois c'est `VirtualAllocEx` qui a été refusé) — élimine un délai/une race de nettoyage kernel asynchrone comme cause, et confirme le caractère **intermittent** (l'appel refusé n'est pas toujours le même).

**Cause identifiée :** pas un bug KillEngine. `Get-Service` a révélé la présence du service **Windows Defender Advanced Threat Protection** (Microsoft Defender for Endpoint / ATP) sur la machine — distinct de la protection temps réel classique. Désactiver cette dernière (testé en conditions réelles, à la demande de l'utilisateur) n'a **rien changé** au comportement — confirmant que c'est bien l'EDR (ATP), pas l'antivirus de base, qui intervient. La séquence "attache un débogueur → détache → alloue de la mémoire + crée un thread distant dans la même cible" est une heuristique classique de détection d'injection de code chez les EDR modernes — qui ne distingue pas un usage légitime (debug/instrumentation, exactement ce que fait KillEngine) d'un usage malveillant. Confirmé par l'utilisateur comme un type de friction connu et attendu avec des outils de dev/test de ce genre.

**Corrigé comment (mitigation, pas un contournement du diagnostic) :**
- `core/inject/dll_injector.cpp` : nouveau `accessDeniedHint(DWORD err)`, ajouté au message d'erreur de `VirtualAllocEx`/`WriteProcessMemory`/`CreateRemoteThread` quand `err == ERROR_ACCESS_DENIED` — explique la cause probable (antivirus/EDR) et suggère de réessayer ou d'exclure `KillEngine.exe`, plutôt que de laisser un code Win32 nu.
- Les 4 tests qui rencontraient cette signature (`BreakpointArbiterRuntimeTest.SequentialCyclesAcrossFreshPidsExternalThenInProcess`, `DebugDetachReproducer.ScenarioE/G/I`) détectent maintenant spécifiquement ce message et font `GTEST_SKIP()` documenté (pas un échec dur, pas un skip silencieux — le message explique précisément pourquoi) ; toute AUTRE erreur reste un échec dur, pour ne jamais masquer un futur vrai bug derrière ce skip.
- Reste à faire (hors scope immédiat, noté pour une future session) : surfacer ce même message dans l'UI Expert (aujourd'hui `result.error` remonte déjà tel quel au frontend, donc le hint apparaît déjà, mais pas encore de mention explicite dans l'aide contextuelle du produit) ; envisager de documenter dans le guide utilisateur que désactiver temporairement la protection en temps réel (ou ajouter une exclusion `KillEngine.exe`) peut être nécessaire pour les fonctionnalités d'injection/breakpoint, **uniquement le temps de l'utiliser**.

**Statut :** ✅ fermé. Protections d'arbitrage livrées et testées (8/8 tests unitaires, tests d'intégration tous verts ou skip documenté — plus aucun rouge). Cause racine de la régression identifiée avec certitude raisonnable (reproduction Win32 pure, confirmée insensible à la désactivation de la protection temps réel classique, cohérente avec la présence du service ATP) et mitigée par un message actionnable.

**Lié à :** `core/debug/breakpoint_arbiter.h/.cpp`, `core/debug/inprocess_breakpoint.{h,cpp}`, `core/debug/inprocess_breakpoint_handler/inprocess_breakpoint_handler.cpp`, `core/debug/hardware_breakpoint.cpp`, `core/debug/breakpoint_freeze.cpp`, `core/scanner/memory_window_search.{h,cpp}`, `core/inject/dll_injector.cpp` (`accessDeniedHint`), `apps/desktop/application_controller.cpp` (`resetHardwareBreakpointStateForPreviousTarget`, `scanMemoryWindow`), `tests/unit/test_breakpoint_arbiter.cpp`, `tests/integration/test_breakpoint_arbiter_runtime.cpp`, `tests/integration/test_debug_detach_reproducer.cpp`.

---

### [2026-08-20] Reprise investigation XP Solitaire — 3ᵉ copie/pointeur/réseau testés, cause probable identifiée

**Contexte :** après avoir livré l'arbitrage des breakpoints matériels (entrée précédente), reprise de l'investigation XP laissée ouverte le 19/08/2026 (`Statut : 🟡 en cours, pas fermé`, voir entrée "Microsoft Solitaire Collection résiste..."). Nouveau PID Solitaire (12128, le même tout au long de cette session — jamais redémarré), nouvelles adresses jumelles relocalisées par scan `Int64` classique (`221921b7ca8`/`221b8003c68`, méthode déjà connue de fonctionner pour retrouver la paire).

**Ce qui a été testé, dans l'ordre, avec le résultat de chaque étape :**

1. **`scanMemoryWindow` (nouvelle primitive dédiée, remplace le détournement d'`analyzeUiStringSources` de la session précédente) autour des 2 adresses, rayon 64 Ko** : aucune 3ᵉ copie plausible de la valeur trouvée — seulement des artefacts de lecture qui chevauchent nos propres 8 octets (même valeur relue à un offset décalé de 2) et une coïncidence isolée à grande distance. **Élimine l'hypothèse "3ᵉ copie locale en clair à proximité".**

2. **`findWhatAccesses` (nouvelle version bloquante ajoutée cette session, miroir de `findWhatWrites` — n'existait qu'en async, non exploitable via le connecteur d'automatisation) sur l'adresse 1, 15s, capture lecture ET écriture** : 28 hits, **tous identiques** — même RIP (`Solitaire.exe+0x9ABEA`), même thread (11936), `valueBefore=0 → valueAfter=3000` à chaque fois. Aucune lecture avec un RIP différent (qui aurait pu trahir une fonction de vérification/comparaison distincte). Interprété comme un rafraîchissement régulier de l'UI, pas une action de jeu isolée.

3. **Falsification d'une seule adresse (écrite à 9999) puis observation immédiate (`findWhatAccesses`, 8s)** : **0 accès capturé**. La correction n'est donc pas immédiate — contrairement à l'hypothèse initiale d'une "vérification active en continu" entre les 2 copies.

4. **Lecture des 2 adresses après cette falsification, sans action de jeu entre-temps** : **divergence totale** — adresse 1 toujours à 9999 (jamais touchée), adresse 2 passée à 3750 (nouvelle valeur, ni 3000 ni 9999 — le joueur avait continué de jouer en parallèle). **Élimine l'hypothèse "les 2 copies sont comparées et resynchronisées entre elles"** — si c'était le cas, l'une aurait dû s'aligner sur l'autre.

5. **Écriture forcée sur l'adresse 2 seule (5000, `verified: true` en relecture immédiate) puis vérification visuelle à l'écran** : écran toujours à 3750 (pas 5000) — l'écriture n'a jamais été reflétée à l'affichage. Après un nouveau changement de jeu naturel (carte jouée, XP → 4125), **les deux adresses sont redevenues identiques entre elles ET égales à l'affichage (4125)** — écrasées ensemble, pas resynchronisées l'une sur l'autre.

   **Conclusion du point 5, la plus solide de cette session** : ce n'est vraisemblablement **pas une protection anti-triche active** (pas de comparaison ni de correction ciblée). Le moteur de jeu **recalcule l'XP depuis une vraie source interne à chaque événement de jeu** et **réécrit aveuglément** les 2 copies avec la valeur fraîche — sans jamais les relire ni les comparer entre elles. Ça explique tout ce qui avait été observé les deux sessions : une adresse seule "tient" jusqu'au prochain événement (pas une correction, juste le prochain écrasement programmé) ; le Score avait "tenu" à l'écriture double la session précédente probablement parce qu'aucun nouvel événement de scoring ne s'était produit dans la fenêtre d'observation, pas parce que la technique avait réellement "débloqué" quoi que ce soit.

6. **`scanPointerChains` sur l'adresse 2, maxDepth=4, maxOffset=4096** : **0 chaîne trouvée** après ~200 millions de pointeurs scannés / 1,6 Go / ~12 minutes. Cause la plus probable : Solitaire tourne sur un **tas .NET managé**, dont le garbage collector déplace régulièrement les objets — un scanner de pointeurs bas niveau cherche des adresses **fixes**, qui n'existent structurellement pas de la même façon dans ce genre d'environnement. **Élimine (dans les limites testées) l'hypothèse "chemin de pointeurs stable atteignable par scan classique".**

7. **Surveillance réseau (`Get-NetTCPConnection`/`Get-NetUDPEndpoint` sur le PID) avant/après un gain d'XP** : aucune nouvelle connexion, horodatages de création identiques avant/après, les 2 connexions HTTPS existantes (une IP Azure, une IP AWS) étaient en `CloseWait` (pas actives). **Aucune preuve d'un aller-retour serveur synchrone au moment du gain** — n'exclut pas une synchronisation périodique/différée, mais élimine l'hypothèse d'une vérification serveur immédiate à chaque changement.

8. **Découverte incidente en relocalisant un "score de manche" séparé (compteur distinct de l'XP totale, remis à zéro par manche)** : sa paire d'adresses (`221921b7cb0`/`221b80032e8`) est à **exactement 8 octets** de l'ancienne adresse 1 de l'XP (`221921b7ca8`) — cohérent avec des **champs voisins dans le même objet**, pas des allocations indépendantes.

9. **`analyzeStructureMemory` autour de cette zone (128 octets)** : layout cohérent avec un objet du tas managé .NET — pointeur à l'offset 0 (probable MethodTable/vtable interne CLR), pointeur à l'offset 0x18 qui **s'auto-référence** (pointe vers le début du bloc analysé lui-même), champs Int64 aux offsets 0x30/0x40/0x48/0x50 (1000 / 330 — ni l'XP ni le score suivis, encore un autre champ réécrit indépendamment / 660 = score de manche confirmé / 1 = probable flag). Confirme la structure commune, mais **KillEngine n'a aucun moyen de décoder ce layout correctement** (pas de résolution de MethodTable → nom de type, pas de désassemblage fiable du code qui l'utilise — protection anti-lecture de code déjà rencontrée la session précédente sur cet exécutable Store).

**Statut :** ✅ tranché sur le *mécanisme* (recalcul + écrasement périodique depuis une source interne, pas une vérification anti-triche active) — mais 🟡 l'XP reste **non contrôlable durablement** avec les outils actuels de KillEngine. Cause de blocage identifiée précisément : absence d'un vrai outil d'inspection **.NET/CLR managé** (type ClrMD/SOS) capable de résoudre un objet du tas managé au-delà d'un scan de pointeurs bas niveau — **nouveau candidat #8 ajouté à `docs/POWER_UP_ROADMAP.md` section "Prochains gros chantiers"** avec le détail technique (ClrMD vs `windbg`/SOS externe, effort estimé élevé).

**Pistes à explorer pour une future session, par ordre de coût croissant :**
- Implémenter le candidat #8 (ClrMD ou pilotage de `windbg`+SOS) — seule piste qui adresserait directement la cause racine identifiée ici.
- Tester `!GCRoot` (SOS) spécifiquement sur l'adresse de l'objet — c'est la commande conçue exactement pour "trouver un chemin stable vers un objet du tas managé", le problème que le scan de pointeurs classique (point 6 ci-dessus) ne peut pas résoudre.
- Reconsidérer la piste réseau avec un vrai outil de capture (Wireshark/proxy), pas juste les métadonnées de connexion — le test de cette session (point 7) n'exclut qu'une synchronisation immédiate, pas une synchronisation périodique en arrière-plan.

**Lié à :** entrée précédente (session Solitaire du 19/08/2026), `docs/POWER_UP_ROADMAP.md` candidat #8, `core/scanner/memory_window_search.{h,cpp}`, `apps/desktop/application_controller.cpp` (`scanMemoryWindow`, `findWhatAccesses` bloquant, nouveau cette session).

---

### [2026-08-20] Vérification candidat #8 avant de coder — Solitaire.exe n'est pas une cible CLR

**Contexte :** avant d'engager l'effort "élevé" du candidat #8 (ClrMD/SOS), demande explicite de l'utilisateur de vérifier factuellement les prérequis plutôt que de partir sur l'hypothèse "tas .NET managé" de l'entrée précédente, qui reposait uniquement sur un layout mémoire *compatible* avec un objet CLR (pointeur offset 0, auto-référence offset 0x18) — jamais sur une preuve directe qu'un CLR est réellement chargé dans le process.

**Vérification faite (sans lancer Solitaire — inspection statique du package installé)** :
- Package trouvé : `C:\Program Files\WindowsApps\Microsoft.MicrosoftSolitaireCollection_4.26.7290.0_x64__8wekyb3d8bbwe`. `AppxManifest.xml` confirme `Executable="Solitaire.exe" EntryPoint="Solitaire.App"` — `Solitaire.exe` est bien le process observé dans les sessions précédentes (`Solitaire.exe+0x9ABEA` dans les hits `findWhatAccesses`).
- **Aucun fichier runtime .NET dans le package** : ni `mrt100_app.dll`/`mrt100_appx.dll` (runtime .NET Native/UWP classique), ni `coreclr.dll`, ni `hostfxr.dll`, ni `mscorlib`/`System.Private.CoreLib`. Seul `clrcompression.dll` est présent (bibliothèque de (dé)compression réutilisée par plusieurs runtimes, pas un signal de CLR à elle seule).
- **Parsing manuel de l'en-tête PE** (offset `e_lfanew` → Optional Header → Data Directory #14 "COM Descriptor/CLR Header") sur `Solitaire.exe` **et** `Microsoft.MicrosoftSolitaireCollection.dll` : `ClrHeaderSize = 0` sur les deux — **aucun COR20 header**. Un exécutable/DLL managé (.NET Framework ou CoreCLR) a toujours ce header non-vide ; son absence est une preuve directe, pas une déduction.
- `EntryPoint="Solitaire.App"` (activation WinRT par nom de classe résolue via `RoGetActivationFactory`, pas un point d'entrée managé `Main()`) est cohérent avec un exécutable **C++/WinRT ou C++/CX natif**, pas un exécutable C#/.NET.

**Conclusion :** l'hypothèse "tas .NET managé" de l'entrée précédente (19-20/08/2026) est **invalidée pour Solitaire spécifiquement**. Le layout observé par `analyzeStructureMemory` (pointeur offset 0 = probable vtable C++ ou pointeur d'interface COM/WinRT, auto-référence offset 0x18 = probable compteur de référence intrusif ou nœud de structure interne) est bien plus cohérent avec un **objet C++ natif** (vtable + refcounting COM/WinRT, ou allocateur de pool/arène propre au moteur de jeu) qu'avec un objet CLR — ce qui expliquerait aussi pourquoi `scanPointerChains` (200M pointeurs, maxDepth=4) n'a rien trouvé : pas forcément un GC qui déplace les objets, potentiellement un allocateur de pool qui réutilise/recycle les blocs, ou une profondeur de chaîne réelle supérieure à celle testée.

**Conséquence directe sur le candidat #8** : ClrMD (ou SOS/`windbg`) ne peut **littéralement pas s'attacher** à ce process — les deux outils nécessitent un CLR chargé et découvrable via le DAC (`mscordacwks.dll`/`mscordaccore.dll`), qui n'existe pas ici. Implémenter le candidat #8 maintenant n'aurait **rien résolu** pour le cas Solitaire qui l'a motivé. Le candidat reste néanmoins potentiellement utile comme **capacité générale** de KillEngine pour de vraies cibles CLR (app .NET Framework/WPF/WinForms classique, service .NET Core/5+) — mais avec un périmètre plus étroit que supposé : ClrMD ne supporte ni Mono/IL2CPP (donc la plupart des jeux Unity), ni .NET Native/UWP classique (le cas qu'on vient d'éliminer ici), seulement Desktop CLR et CoreCLR. À réévaluer la priorité de ce candidat en conséquence — il ne débloquera pas Solitaire, seulement une classe de cibles différente et plus restreinte que prévu.

**Statut :** ❌ abandonné pour Solitaire précisément — piste éliminée par preuve directe (pas de COR header), pas par manque d'outillage. 💤 ClrMD/SOS reste en veille comme capacité générale KillEngine, à ne prioriser que si une vraie cible CLR (Desktop .NET Framework ou CoreCLR, pas Mono/UWP-.NET-Native) se présente.

**Piste à explorer pour la suite de l'investigation XP Solitaire (nouvelle direction, pas celle envisagée avant cette entrée)** :
- Résoudre le vtable à l'offset 0 de l'objet (comparer aux exports/RTTI de `Solitaire.exe`/`Microsoft.MicrosoftSolitaireCollection.dll` si le binaire n'est pas strippé) pour identifier le type C++/WinRT réel plutôt que de deviner à l'aveugle.
- Reconsidérer un scan de pointeurs avec une profondeur/fenêtre d'offset plus large (le point 6 de l'entrée précédente s'arrêtait à `maxDepth=4`/`maxOffset=4096`), ou en partant d'une racine plus probable (objet `Application`/scène de jeu) plutôt qu'en aveugle depuis l'adresse XP.
- Revenir à l'hypothèse "recalcul + écrasement périodique depuis une source interne" (conclusion la plus solide de l'entrée précédente, point 5) : plutôt que de chercher un chemin de pointeurs stable vers la donnée, chercher la fonction qui **calcule** la valeur avant écriture (au lieu de celle qui écrit) — piste debugger déjà partiellement outillée (`findWhatWrites`/`findWhatAccesses`) mais jamais poussée en amont du store lui-même.

**Lié à :** entrée précédente (19-20/08/2026), `docs/POWER_UP_ROADMAP.md` candidat #8 (à mettre à jour avec ce résultat), `core/scanner/structure_analyzer.*`, `apps/desktop/application_controller.cpp` (`scanMemoryWindow`, `findWhatAccesses`, `scanPointerChains`).

---

### [2026-08-20] Écriture kernel-mode testée sur l'XP Solitaire — même conclusion que l'écriture usermode

**Contexte :** le driver `KillEngineKernel.sys` vient d'être rendu fonctionnel (lecture/écriture réelles via `KeStackAttachProcess`/`ProbeForRead`/`ProbeForWrite`, validées sur `KillEngineTestTarget.exe` — voir `docs/PHASE_TRACKER.md` PHASE 39). Question directe posée par l'utilisateur : est-ce qu'une écriture **kernel-mode**, qui contourne entièrement `WriteProcessMemory`/toute API usermode surveillable, tiendrait là où l'écriture usermode classique échouait sur l'XP (voir entrée du 19-20/08/2026 ci-dessus) ?

**Hypothèse testée :** si le mécanisme qui fait "perdre" l'écriture était une détection/protection au niveau API (hook, EDR, vérification usermode), une écriture kernel-mode devrait s'en affranchir et tenir. Si c'est bien un recalcul interne périodique du jeu (conclusion la plus solide de l'entrée précédente), le kernel ne devrait rien changer.

**Protocole, en session live avec l'utilisateur (KillEngine attaché à `Solitaire.exe`, PID différent de la session précédente — nouvelles adresses) :**
1. `startExactScan("0", "Int32")` sur XP affichée à 0 → 59,3M correspondances, dépasse la capacité du candidate store (1M) → scan jeté (`partial: true`, pas de base exploitable). **Piège évité pour la suite** : ne jamais démarrer un scan exact sur une valeur aussi commune que `0` sur ce genre de cible, repartir directement sur la première valeur non triviale rapportée par l'utilisateur.
2. `startExactScan("60", "Int32")` (XP passée à 60 en jouant) → 11 333 candidats, dans la capacité du store.
3. `nextScan("exact", "120")` (XP passée à 120) → **3 candidats** : `0x1f1ab59c290`, `0x1f1d1627be0`, `0x1f1d1627be8` — les deux derniers espacés de exactement 8 octets, le pattern "paire jumelle" déjà documenté pour cette cible.
4. `writeMemoryKernel` avec la sentinelle `99999` sur les 3 adresses → succès rapporté par le driver sur les 3 (`bytesWritten: 4`).
5. Utilisateur : rien affiché à l'écran (jamais vu `99999`), puis a joué un as → affichage passé à `180`.
6. `readMemoryKernel` sur les 3 adresses → **les 3 retournent `B4 00 00 00` = 180**, aucune trace de `99999`.

**Résultat :** identique à l'échec usermode déjà documenté — la valeur écrite (sentinelle bien distincte, aucune ambiguïté possible) disparaît complètement, remplacée par la vraie valeur recalculée par le jeu. La confirmation par le driver que les 3 écritures ont réellement réussi au niveau mémoire (`success: true` à chaque fois, pas juste supposé) exclut un échec silencieux côté outillage.

**Conclusion, plus solide qu'avant** : l'hypothèse "le jeu détecte/bloque l'écriture externe (API hook, EDR, vérification usermode)" est maintenant **définitivement éliminée** — un accès kernel-mode qui ne passe par aucune API Win32 surveillable donne exactement le même résultat qu'un accès usermode. Le mécanisme réel ne peut être qu'un recalcul interne : le jeu **ne lit jamais** ces adresses pour vérifier une correspondance, il se contente de **réécrire la valeur qu'il vient de calculer** à chaque événement de jeu, quel que soit ce qui s'y trouvait juste avant. Aucune technique d'écriture, aussi privilégiée soit-elle, ne peut faire tenir une valeur dans un emplacement qui n'est pas la source de vérité.

**Piste retenue pour la suite (confirmée, pas nouvelle mais maintenant prioritaire par élimination)** : arrêter de cibler l'adresse mémoire, remonter à la **fonction qui calcule** la valeur avant de l'écrire — `findWhatWrites`/breakpoint sur l'instruction d'écriture (RIP déjà capturable via les outils existants), puis analyser ce qui l'alimente en amont (autre variable lue, appel de fonction, etc.) plutôt que d'agir sur le résultat déjà calculé. Session suivante : l'utilisateur relance une partie fraîche et prévient quand prêt à reprendre.

**Lié à :** entrée précédente (19-20/08/2026), `docs/PHASE_TRACKER.md` PHASE 39 (driver kernel fonctionnel), `core/kernel/kernel_driver_bridge.*`, `tools/kernel_driver/KillEngineKernel/driver.cpp`, `apps/desktop/application_controller.cpp` (`readMemoryKernel`/`writeMemoryKernel`, `findWhatWrites`).

---

### [2026-08-20] XP Solitaire enfin contrôlable — le champ ciblé depuis le début n'était que l'animation d'affichage

**Contexte :** suite immédiate de l'entrée précédente (écriture kernel-mode confirmant que ni usermode ni kernel-mode ne faisaient tenir l'XP). Deux pistes restaient : élargir le pointer scan, ou désassembler la fonction appelée dans la séquence d'écriture. Choix : la fonction appelée (`call` entre `addss xmm0,xmm2` et `cvttss2si rdx,xmm0`) — mais avant, un deuxième pointer scan (nouvelle session Solitaire, nouvelle base `0x16877070560`, mêmes paramètres maxDepth=4/maxOffset=4096) a été relancé jusqu'au bout : **71,3M pointeurs scannés, 0 chaîne trouvée**, en ~4 min. Deuxième échec reproductible sur deux PID différents — élimine solidement le pointer scan classique à ces paramètres comme piste viable pour cet objet.

**Incident en cours de route :** Solitaire a crashé (~25-60s après deux cycles `findWhatWrites`/`DebugActiveProcess` rapprochés sur le même PID, 2 min d'écart). Logs KillEngine : les deux détachements sont propres (`HwBreakpointArbiter... RELEASED... désarmement confirmé`), donc pas de preuve formelle que c'est la cause, mais le timing est cohérent avec la fragilité déjà connue de cette cible face aux attaches/détaches répétées. Solitaire redémarre seul (nouveau PID), aucune perte de données (jeu Store personnel).

**Désassemblage de la fonction appelée** (calculé précisément via la base de module réelle du process courant, `getProcessModules`, pas une estimation manuelle) : `0x7ff66898daf6` est un stub d'indirection IAT (`jmp [RIP+0xD973C]`, 6 octets, motif `FF 25 ?? ?? ?? ??`). Lecture du pointeur réel stocké dans ce slot → `0x7ffe6dae8050`, qui tombe dans la plage de **`ucrtbase.dll`** (confirmé via la liste de modules du process) — donc une fonction CRT générique (probablement un helper d'arrondi/troncature flottant inséré par le compilateur), **pas de la logique de jeu**. Piste éliminée : cette fonction ne calcule pas l'XP.

**La vraie percée : remonter en arrière depuis `addss`, pas en avant depuis l'appel.** Lecture de 80 octets avant l'instruction `addss xmm0, xmm2` et désassemblage manuel byte-par-byte (aligné exactement sur l'octet de départ connu de `addss`, aucun octet perdu ni mal interprété) :

```asm
mov      ecx, [rsi+0x904]     ; ecx = XP "actuel"      (entier, PAS un flottant)
movss    xmm0, [rsi+0x914]    ; horloge d'animation
subss    xmm0, [rsi+0x918]    ; - temps de depart
divss    xmm0, [rsi+0x90C]    ; / duree -> fraction de progression [0..1]
mov      eax, [rsi+0x908]     ; eax = XP "cible"        (entier, PAS un flottant)
sub      eax, ecx             ; delta = cible - actuel
xorps    xmm1, xmm1
cvtsi2ss xmm1, rax            ; delta en flottant
mulss    xmm0, xmm1           ; xmm0 = fraction * delta
xorps    xmm2, xmm2
cvtsi2ss xmm2, rcx            ; actuel en flottant
addss    xmm0, xmm2           ; xmm0 = actuel + fraction*delta  (= point d'entree connu)
call     ucrtbase!<helper>    ; (helper flottant generique, sans effet sur la formule)
cvttss2si rdx, xmm0           ; troncature en entier
mov      [rsi+0x900], edx     ; ecrit le champ AFFICHE (celui traque depuis le debut)
```

C'est une **interpolation d'animation de compteur** classique (`affiché = actuel + (cible - actuel) × progression`). `[RSI+0x900]` (le champ traqué depuis la toute première session) n'a jamais été que le résultat de ce calcul, recalculé à chaque frame — ce qui explique *tout* ce qui a été observé sur plusieurs sessions : écritures usermode et kernel qui ne tiennent jamais, "paire jumelle" à 8 octets d'écart qui semblait redondante.

**La paire jumelle n'était pas redondante — c'était `[+0x900]` (affiché) et `[+0x908]` (cible) qui coïncident une fois l'animation terminée.** Vérifié : `[+0x904]` (actuel) = 180, `[+0x908]` (cible) = 240, `[+0x900]` (affiché) = 240 — actuel et cible bien **distincts** au moment du test (l'animation avait déjà rattrapé la cible mais `actuel` n'était pas encore resynchronisé), confirmant que ce sont deux champs sémantiquement différents, pas deux copies du même nombre.

**Test final, écriture kernel sur `[RSI+0x908]` (la cible, PAS l'affiché) uniquement** : sentinelle `99999` écrite → l'affichage anime progressivement jusqu'à `99999` **et tient**. Carte jouée ensuite (gain de 60) → affichage passe à **`100059` = 99999 + 60**, confirmé par capture d'écran utilisateur. Le jeu additionne son gain **par-dessus** la valeur injectée au lieu de l'ignorer ou de revenir à l'ancienne valeur — preuve définitive que `[RSI+0x908]` est la vraie source de vérité utilisée par le moteur de jeu, pas un affichage ni une copie.

**Conclusion générale, au-delà de ce cas précis** : pour ce genre de compteur animé (très courant dans les jeux casual/mobile), la bonne méthode n'est **pas** de chercher un chemin de pointeurs vers le champ affiché (qui est un résultat dérivé, recalculé en continu, structurellement impossible à faire "tenir" par écriture externe), mais de **remonter le désassemblage en amont de l'instruction d'écriture** pour trouver les champs sources (souvent des entiers "actuel"/"cible" utilisés pour interpoler un flottant d'affichage) et d'écrire sur la source, pas sur le résultat. Le pointer scan classique reste utile pour d'autres cas, mais pas quand le champ ciblé est structurellement un champ dérivé/calculé.

**Deuxième confirmation, après une perte de manche (même session, même PID)** : l'utilisateur a perdu une manche peu après le premier test — l'affichage est redescendu progressivement, ce qui a été signalé comme "revenu en arrière" et interprété d'abord comme une possible resynchronisation externe (sauvegarde/cloud écrasant la valeur injectée). **Vérifié avant de conclure quoi que ce soit** : lecture de `[+0x904]` (actuel) = 360 et `[+0x908]` (cible) = 0 — cohérent à 100% avec le modèle déjà établi (une perte remet la cible à 0 via la logique de jeu normale, l'animation redescendait simplement de 360 vers 0), **pas une contradiction du mécanisme trouvé**, juste un comportement de jeu légitime qu'on n'avait pas encore observé. Écriture kernel de `9999` sur `[+0x908]` (même adresse, le PID n'ayant jamais changé) → confirmé fonctionnel par l'utilisateur ("ton action a marché essai j'ai bien la valeur changé"). Preuve que la méthode tient **across un événement de reset de round**, pas seulement sur le cas testé initialement.

**Méthode reproductible retenue pour cette cible (et gabarit pour tout compteur animé similaire)** :
1. Scan exact multi-étapes sur la valeur affichée (0 est trop bruyant, démarrer sur la première valeur non triviale rapportée par l'utilisateur) jusqu'à isoler la paire d'adresses à 8 octets d'écart (`+0x900` affiché / `+0x908` cible).
2. Base de structure = adresse `+0x900` moins `0x900`.
3. Écrire directement sur `base+0x908` (jamais sur `base+0x900`, qui est recalculé chaque frame et ne tiendra jamais).
4. Valable après un gain, une perte, ou un nouveau round tant que le PID/l'objet ne change pas — revérifier `+0x904`/`+0x908` avant d'écrire si le PID a changé (nouvelle base de structure probable, ASLR différent).

**Statut :** ✅ **résolu, confirmé deux fois** (gain initial + après un reset de round par perte). XP Solitaire contrôlable de façon fiable via `[RSI+0x908]` (relatif à la base de structure, elle-même retrouvable par scan exact multi-étapes sur la valeur affichée comme d'habitude).

**Lié à :** toutes les entrées précédentes de cette investigation (19-20/08/2026), `docs/PHASE_TRACKER.md` PHASE 39 (driver kernel), `core/kernel/kernel_driver_bridge.*`, `apps/desktop/application_controller.cpp` (`findWhatWrites`, `suggestCodePatches`, `getProcessModules`, `readMemoryKernel`/`writeMemoryKernel`).

---

## Archive

*(vide pour l'instant — une entrée migre ici, sans être supprimée, une fois son sujet devenu obsolète au point de ne plus mériter de rester dans le sommaire actif ci-dessus — ex. un risque anticipé qui ne s'est jamais matérialisé et ne peut plus se produire)*
