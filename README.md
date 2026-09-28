# ⚡ KillEngine

**Repérez ce qui change en mémoire. Comprenez d'où vient la valeur. Gardez une solution réutilisable.**

KillEngine est un outil d'analyse et d'instrumentation de processus Windows. Son Assistant vous aide à partir d'une valeur visible à l'écran, à choisir une recherche et à affiner les résultats. Les vues Investigation et Expert permettent ensuite de vérifier chaque piste, tandis que les profils et le Trainer conservent ce qui fonctionne.

Le modèle d'IA local est inclus dans l'archive portable complète. Vous pouvez aussi avancer entièrement à la main.

## Commencer

1. Téléchargez **`KillEngine-portable.zip`** dans les fichiers de la version publiée.
2. Extrayez **tout** le ZIP dans un dossier où vous pouvez enregistrer des fichiers.
3. Lancez **`KillEngine.exe`**. Aucun installateur n'est nécessaire.
4. Cliquez sur **« Essayer le tutoriel »** en bas de la barre latérale pour découvrir le parcours sur une cible de démonstration isolée.

**Système requis : Windows 10 ou 11, 64 bits.** Le ZIP complet comprend l'interface, les dépendances et le modèle IA local. Réglages, profils et données de travail sont conservés avec le dossier portable ; les exceptions liées à certains modules Windows et aux clés API sont expliquées dans le [guide utilisateur](docs/USER_GUIDE.md).

## De la valeur affichée à une piste vérifiée

Dans le tutoriel, une petite cible affiche une valeur de santé. Cherchez cette valeur, faites-la varier avec le bouton **Dégâts**, puis lancez un scan suivant : KillEngine réduit les adresses candidates à mesure que vous observez ce qui change. Vous pouvez inspecter la piste obtenue, préparer une écriture, en vérifier l'effet et sauvegarder le résultat dans un profil.

Ce parcours s'appuie sur de vraies lectures et écritures de la cible de démonstration. Il permet de se familiariser avec l'outil avant d'analyser vos propres applications.

## Choisissez votre façon de travailler

| Vue | Ce qu'elle apporte |
| --- | --- |
| **Assistant** | Décrivez la valeur recherchée en français ou en anglais ; l'IA locale propose et guide les scans. |
| **Investigation** | Suivez les étapes, hypothèses et résultats de votre recherche. |
| **Expert** | Pilotez les scans, l'inspection mémoire, les pointeurs, la surveillance et les outils de diagnostic. |
| **Trainer** | Regroupez des actions dans des fonctions réutilisables, à activer ou restaurer. |

KillEngine propose aussi des scans de valeur initiale inconnue, l'affinage après changement, la comparaison de candidats, les profils et un centre d'activité pour suivre les opérations en cours. Les actions qui modifient la mémoire passent par des étapes de vérification ou de confirmation selon le workflow.

## Pour qui ?

KillEngine s'adresse aux personnes qui analysent des applications qu'elles possèdent ou sont autorisées à étudier : développement et débogage, QA, recherche sur des logiciels anciens, modding autorisé et exploration de la mémoire en environnement contrôlé.

**Statut : version bêta 0.1.0.** Le package Windows portable est actuellement non signé. Certaines fonctions avancées dépendent du processus cible ou d'un module optionnel ; aucun outil de mémoire ne peut garantir le même résultat sur tous les programmes.

## Documentation

- [Guide utilisateur en français](docs/USER_GUIDE.md) · [User guide in English](docs/USER_GUIDE_EN.md)

## Construire depuis les sources

La version portable évite toute compilation. Pour développer KillEngine, prévoyez Visual Studio Build Tools (MSVC C++20), CMake 3.21+, Qt 6.8 et Node.js 20+.

```powershell
cd ui
npm install
cd ..
.\scripts\configure.ps1
.\scripts\build.ps1
.\build\bin\killengine_unit_tests.exe
```

Pour créer le package Windows complet :

```powershell
.\scripts\package-windows.ps1
```

Le dossier `dist\KillEngine-portable\` et l'archive `dist\KillEngine-portable.zip` sont alors produits. Pour valider un build local, lancez `.\scripts\release-check.ps1`.

**Architecture :** moteur mémoire C++20 et API Win32, Assistant local via llama.cpp, interface Qt 6.8 et Vue 3.

## Licence et contribution

KillEngine est distribué sous [licence MIT](LICENSE). Les contributions et retours d'expérience sont bienvenus via le dépôt du projet.


