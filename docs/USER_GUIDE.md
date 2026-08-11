# KillEngine - Guide utilisateur V1

KillEngine est un outil local Windows pour trouver et modifier des valeurs en mémoire dans un processus que tu possèdes ou que tu contrôles. Le mode normal passe par l'Assistant : tu décris la valeur, KillEngine choisit les scans et te guide jusqu'aux adresses finales.

## Avant de commencer

Utilise KillEngine uniquement sur des logiciels locaux, hors ligne, et dans un cadre autorisé. Certains jeux, anti-cheats ou logiciels protégés peuvent bloquer l'accès mémoire ou sanctionner ce type d'outil.

Prépare aussi le programme cible avant de scanner :

- lance le jeu ou l'application ;
- affiche la valeur à trouver dans le jeu si elle est connue ;
- garde la valeur stable pendant le premier scan ;
- évite de scanner un processus qui vient de se fermer ou de redémarrer.

## Lancer KillEngine

Depuis une version portable :

```text
KillEngine.exe
```

Depuis le dossier de build local :

```text
build\bin\KillEngine.exe
```

Si l'application ne démarre pas, ouvre `Paramètres > Diagnostic` après un prochain lancement réussi, ou regarde les logs dans le dossier local de l'application Windows. Les exports diagnostic regroupent les logs, le debug Smart Search et les rapports de crash récents.

## Attacher un processus

1. Ouvre l'onglet `Processus`.
2. Clique sur `Rafraîchir`.
3. Sélectionne le processus du jeu ou de l'application.
4. Clique sur `Attacher`.

Le nom du processus attaché apparaît ensuite dans les autres vues. Si le processus n'apparaît pas, lance-le d'abord avec une fenêtre visible, puis rafraîchis.

## Utiliser l'Assistant

L'Assistant est le flux recommandé.

Exemples de demandes :

```text
j'ai 41250 en argent
j'ai 900 en score je veux le passer à 2000
j'utilise ces mémoires 0x2d3a80afb0c 0x2d3f7e25594
passe-les à 800
j'ai une nouvelle recherche à faire, la valeur est 30
```

### Recherche guidée

1. Donne la valeur actuelle et, si tu veux, la valeur cible.
2. KillEngine lance un scan exact.
3. Change la valeur dans le jeu.
4. Clique sur `J'ai changé` ou donne la nouvelle valeur dans le chat.
5. KillEngine réduit les candidats.
6. S'il reste 1 à 4 candidats, KillEngine peut écrire automatiquement la valeur cible.

Les dernières adresses auto-écrites restent actives dans la conversation. Tu peux donc demander ensuite `passe-les à 3000` sans repasser par un profil.

### Nouvelle recherche

Si tu veux chercher autre chose, sois explicite :

```text
j'ai une nouvelle recherche à faire, la valeur est 4
il faut chercher ailleurs, nouveau scan pour 120
```

Cela évite que l'Assistant réutilise les adresses actives de la conversation ou d'un profil.

### Adresses données dans le chat

Tu peux donner une ou plusieurs adresses mémoire directement :

```text
0x2d3a80afb0c et 0x2d3f7e25594
```

KillEngine les sélectionne comme cibles actives, puis attend la valeur à écrire.

## Profils

Les profils servent à retrouver plus vite des adresses connues lors d'une prochaine session.

Flux typique :

1. Trouve une ou plusieurs adresses avec l'Assistant ou le Mode Expert.
2. Ouvre `Profils`.
3. Crée ou sélectionne un profil, par exemple `solitaire`.
4. Donne un nom de cible, par exemple `score`.
5. Sauvegarde la cible courante ou les cibles sélectionnées.
6. Lors d'une autre session, sélectionne le profil et clique sur `Activer` ou `Activer tout`.

Une cible activée depuis un profil peut être utilisée dans l'Assistant :

```text
passe le score à 100000
```

Un profil peut contenir plusieurs cibles. Si une adresse ne se résout plus, relance une recherche : le jeu a probablement changé de module, de session ou de structure mémoire.

## Mode Expert

Le Mode Expert donne accès aux opérations manuelles.

### Scan exact

Utilise-le quand tu connais la valeur exacte.

1. Saisis la valeur.
2. Choisis le type (`Int32`, `Int64`, `Float32`, `Float64`).
3. Clique sur `Scanner`.

Les options avancées permettent de limiter une plage d'adresses, définir un alignement ou filtrer les régions mémoire.

### Next Scan

Utilise-le pour réduire une liste de candidats.

- `Exact` : garde les candidats qui valent la nouvelle valeur.
- `Changed` : garde les candidats qui ont changé.
- `Unchanged` : garde les candidats inchangés.
- `Increased` : garde les candidats qui ont augmenté.
- `Decreased` : garde les candidats qui ont diminué.
- `Delta` : garde les candidats qui ont changé d'un delta donné.

### Unknown Initial Value

Utilise ce mode quand tu ne connais pas la valeur initiale.

1. Clique sur `Capturer`.
2. Change la valeur dans le jeu.
3. Choisis un mode (`Changed`, `Increased`, etc.).
4. Clique sur `Comparer`.
5. Répète jusqu'à obtenir peu de candidats.

Le Mode Expert propose aussi des boutons guidés : `ça augmente`, `ça diminue`, `stable`, `ça change`. Après une capture, clique simplement sur ce que tu observes ; KillEngine lance la comparaison initiale, puis raffine les candidats existants aux étapes suivantes. L'historique affiche chaque étape et le nombre de candidats avant/après.

### Annulation

Les scans exacts, next scan et unknown tournent en worker thread. Pendant un scan actif, le bouton `Annuler` demande l'arrêt propre de l'opération. Une annulation ne remplace pas la liste de candidats par des résultats partiels.

### Candidats et écritures

La liste des candidats permet :

- filtrer par adresse ;
- sélectionner une ou plusieurs adresses ;
- envoyer les adresses sélectionnées à l'Assistant ;
- écrire une valeur manuellement ;
- rollback la dernière écriture ;
- activer ou arrêter un freeze.

## Paramètres

La page `Paramètres` permet de régler :

- le stockage temporaire : taille active, fichiers orphelins et bouton `Nettoyer maintenant` ;

- la langue ;
- le type de scan par défaut ;
- la limite maximum de résultats ;
- la taille de chunk mémoire ;
- le fast scan par défaut ;
- les options de debug Smart Search ;
- les chemins préparés pour le modèle IA local.

Le Model Manager complet n'est pas encore finalisé. Si aucun modèle ou runtime local n'est disponible, KillEngine utilise le planner déterministe.

## Diagnostics

Dans `Paramètres > Diagnostic`, tu peux :

- lire les dernières lignes du log principal ;
- voir le chemin du JSONL Smart Search ;
- exporter un bundle diagnostic compressé ;
- vider les événements Smart Search affichés.

Utilise l'export diagnostic quand l'Assistant choisit une mauvaise action, quand un scan semble incohérent, ou quand une écriture échoue.

## Conseils de dépannage

### Aucun candidat trouvé

- Vérifie le bon processus.
- Essaie un autre type (`Int32` est le plus courant pour score/argent, mais pas garanti).
- Si la valeur affichée est arrondie, essaie `Float32` ou `Float64`.
- Relance un scan exact après un changement de niveau, de partie ou de menu.

### Trop de candidats

- Change la valeur dans le jeu, puis fais un next scan.
- Répète jusqu'à obtenir quelques candidats.
- Utilise `Increased` ou `Decreased` quand tu ne connais pas la nouvelle valeur exacte.

### Une adresse ne marche plus

Les adresses absolues changent souvent entre deux sessions. Utilise les profils pour stocker des locators `module_offset`, mais relance une recherche si le jeu change sa structure mémoire.

### L'Assistant réutilise les mauvaises adresses

Demande clairement une nouvelle recherche :

```text
nouvelle recherche, je cherche la valeur 30
```

Tu peux aussi vider les cibles actives depuis le bandeau de mémoire active dans l'Assistant.

### Écriture non vérifiée

- Le processus peut refuser l'écriture.
- La valeur peut être recalculée immédiatement par le jeu.
- L'adresse peut ne plus être valide.
- Essaie de refaire le scan ou d'utiliser plusieurs candidats finaux si l'Assistant les propose.

## Build et package développeur

Build complet :

```powershell
cd ui
npm install
npm run build
cd ..
.\scripts\configure.ps1
.\scripts\build.ps1
ctest --test-dir build --output-on-failure
```

Package portable :

```powershell
.\scripts\package-windows.ps1
```

Inclure un modèle GGUF local dans le package :

```powershell
.\scripts\package-windows.ps1 -IncludeModel
```

## Limites V1

- La progression de scan est encore grossière.
- Le scan multi-type automatique complet n'est pas finalisé.
- Le Model Manager complet reste à brancher.
- Les tests d'intégration automatisés restent à écrire.
- L'outil cible les usages locaux Windows user-mode.
