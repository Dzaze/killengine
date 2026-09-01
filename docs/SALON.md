# Salon IA

Derniere mise a jour : 2026-09-01

Objectif : coordonner Codex, Claude, Cline et tout autre agent IA qui travaille sur KillEngine, pour eviter les collisions de fichiers, les builds lances au mauvais moment et les doubles implementations.

## Regles rapides

1. Avant de coder, annoncer sa lane dans "Lanes actives".
2. Ne pas toucher les fichiers reserves par un autre agent sans accord explicite du proprietaire.
3. Pour un fichier partage ou fragile, poser un verrou court avec une expiration.
4. Avant un build/test lourd, verifier les lanes actives et annoncer l'intention.
5. Apres commit/push ou abandon, liberer la lane et noter le resultat.
6. `docs/PHASE_TRACKER.md` reste l'historique produit ; ce salon sert au temps reel.

## Lanes actives

| Agent | Chantier | Fichiers reserves | Debut | Statut | Notes |
| --- | --- | --- | --- | --- | --- |
| Codex | WEBVIEW-C | `ai/tool_registry.cpp`, `ai/ai_engine.cpp`, `ai/llama_runtime.cpp` | 2026-09-01 | En pause | Attendre stabilisation/push des travaux UI avant build final. |
| Claude/Cline | WEBVIEW-E | `ui/src/App.vue`, `ui/src/services/backend.ts`, `ui/src/stores/app.ts`, `ui/src/stores/assistantSmartSearch.ts`, `ui/src/stores/webView2Inspector.ts`, `ui/src/views/AssistantView.vue`, `ui/src/views/WebView2InspectorView.vue` | 2026-09-01 | En cours | Build annonce passe par Claude ; verifier tracker/status avant reprise. |

## Verrous courts

| Fichier | Agent | Raison | Expire |
| --- | --- | --- | --- |
| `docs/PHASE_TRACKER.md` | Libre | Aucun verrou actif connu | - |

## Messages

- 2026-09-01 Codex : creation du salon IA. WEBVIEW-C a ete commence, build interrompu a la demande du proprietaire pendant le travail UI d'un autre agent. Ne pas relancer de build lourd sans feu vert.

## Point de controle - Solitaire XP

Sources relues : `docs/PHASE_TRACKER.md` (etat courant + chantier WebView2/CDP) et `docs/PHASE_TRACKER_HISTORY.md` (PHASES 257-270, session terrain Solitaire/Bubble XP et corrections Assistant associees).

Constat important : on ne doit pas laisser Solitaire XP comme un simple "echec". C'est une cible basique en apparence, mais elle a revele exactement la classe de problemes que KillEngine doit savoir traiter : valeur affichee evidente, source reelle decouplee, UI moderne, stockage UWP, WebView2 present mais pas forcement porteur du gameplay.

Ce qui a ete appris :

- Le scan exact global Int32 n'a pas trouve la vraie XP.
- Le Float32 a converge vers quelques candidats, mais les ecritures n'ont pas modifie l'affichage.
- L'unknown scan brut a produit trop de bruit, donc pas exploitable sans strategie.
- Les renderers WebView2 donnent des hits generiques V8/Blink identiques, pas la donnee gameplay.
- CDP/WDP fonctionne vraiment, mais sur Solitaire il expose surtout pubs/blank WebViews, pas le plateau ni l'XP.
- Le plateau Solitaire est probablement natif/XAML/DirectComposition, ou alimente par une couche native/non-CDP.
- La piste fichier/local state reste la plus prometteuse : precedent "Bulles", package UWP, `.sgi`, `LocalState`, `LocalSettings`.

Idees de chantiers utiles pour KillEngine :

1. **Mode "valeur affichee introuvable" transversal**
   - Declencheur : exact scan vide, float sans effet, unknown trop large, ou ecriture qui ne tient pas.
   - Sortie : arbre de decision automatique : Trace UI string -> Changed Pages -> modules -> save files -> LocalSettings -> UI Automation/OCR -> debugger/source.
   - But : ne plus laisser l'utilisateur coincer dans "0 candidat" ou "mauvais candidat".

2. **Inspecteur UWP State**
   - Nouveau panneau ou sous-panneau Investigation.
   - Lister automatiquement `LocalState`, `RoamingState`, `TempState`, `Settings/settings.dat`, fichiers recents, extensions suspectes (`.sgi`, `.dat`, `.json`, `.bin`).
   - Ajouter diff avant/apres action utilisateur : snapshot fichiers + timestamps + tailles + hash + strings/nombres extraits.
   - Important pour Solitaire : capturer avant une partie, gagner/recevoir XP, capturer apres, puis comparer les fichiers modifies.

3. **Save File Value Radar**
   - Equivalent disque de Trace UI string.
   - Chercher une valeur affichee sous plusieurs encodages dans les fichiers : ASCII, UTF-16, JSON number, little-endian int/float, varint, base64 decode opportuniste, zlib/gzip si signature connue.
   - Si valeur non trouvee, chercher delta/sequence : ancienne XP, nouvelle XP, gain XP, timestamp proche.

4. **Diff structurel de fichiers binaires**
   - Sur deux snapshots de fichier, isoler les ranges modifies et scorer les champs candidats.
   - Heuristiques : champ qui augmente, champ proche d'un timestamp, checksum voisin, compteur monotone, bloc recompresse.
   - Ne pas ecrire tant que checksum/format non compris.

5. **UI Automation / OCR fallback**
   - Si CDP ne voit pas le gameplay, lire le texte visible via UI Automation, OCR local ou capture fenetre.
   - Objectif : synchroniser les valeurs affichees reelles meme quand DOM/strings memoire echouent.
   - Pour Solitaire, verifier si `30 XP`, score, timer, boutons et resultats de fin de partie sont accessibles hors CDP.

6. **DirectComposition/XAML Probe**
   - Ajouter une investigation de rendu natif moderne : modules XAML/WinUI/DirectComposition, surfaces, visual tree accessible, UIA tree, fenetres enfants.
   - Pas pour "modifier" directement, mais pour comprendre quelle couche rend la valeur.

7. **Event/Network Correlation**
   - Solitaire peut synchroniser profil/XP via Xbox Live ou services Microsoft Casual Games.
   - Ajouter un mode correlation : action utilisateur -> fichiers modifies -> connexions reseau actives -> logs/ETW possibles.
   - Le bouton existant de blocage reseau peut servir d'experience controlee : XP change-t-elle hors ligne, et quand est-elle persistee ?

8. **Find What Writes sur copies affichees, mais avec classification automatique**
   - Quand une adresse candidate change mais l'ecriture ne tient pas, lancer capture courte et classer : copie affichee, cache, interpolation, source evenementielle.
   - Puis proposer automatiquement l'etape suivante : remonter instruction -> AOB -> champs proches -> module scan borne.

9. **Scenario de validation "Solitaire XP"**
   - En faire une cible de validation manuelle officielle, comme les targets synthétiques.
   - Pas obligation de reussir a modifier tout de suite, mais obligation de produire un rapport clair : couche UI, fichiers modifies, endpoints CDP, modules probables, hypotheses restantes.
   - Ce scenario forcera KillEngine a devenir bon sur les apps Store modernes.

10. **Rapport d'enquete unifie**
    - Bouton "Exporter l'enquete" qui combine : process/modules, scans tentes, candidats rejetes, WebView2 targets, UIA/OCR texte, fichiers UWP modifies, LocalSettings, hypotheses.
    - Tres utile pour passer le relais entre agents sans refaire 3 heures de terrain.

Hypothese actuelle la plus forte pour reprendre Solitaire XP : ne pas insister sur CDP gameplay. Reprendre par `LocalState`/fichiers de sauvegarde avec snapshots avant/apres gain XP, puis diff structurel. Si rien ne bouge localement, tester correlation reseau/offline. Si fichier modifie mais valeur non lisible, priorite au chantier "Save File Value Radar" + decompression/checksum.

Message aux agents : Solitaire XP n'est pas un echec a cacher ; c'est une cible-repere. Chaque blocage trouve dessus doit devenir soit une fonctionnalite KillEngine, soit une heuristique de decision, soit un test terrain documente.
