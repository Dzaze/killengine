#pragma once

#include <QString>

namespace killcore {

/// Résolveur commun de racine/chemins portables (PORT-2a,
/// docs/PORTABILITY_ROADMAP.md#port-2). Point d'entrée unique pour tout
/// composant qui doit lire/écrire une donnée relative à l'exécutable plutôt
/// qu'un dossier utilisateur Windows (%LOCALAPPDATA%/registre) -- remplace les
/// sites qui recalculaient chacun QDir(applicationDirPath()).filePath(...)
/// séparément (main.cpp, crash_handler.cpp, pattern_learning_manager.cpp,
/// settings_diagnostics_manager.cpp, profile_store.cpp).
///
/// La racine ne dépend jamais du répertoire courant du process
/// (QDir::currentPath()) : uniquement de QCoreApplication::applicationDirPath(),
/// sauf override explicite de test. Dépend uniquement de Qt6::Core (voit
/// core/CMakeLists.txt : ajouté à la cible killcore_logging, pas killcore, pour
/// rester utilisable sans dépendance inverse de killcore_logging vers tout
/// killcore).
class PortablePaths {
public:
    /// Racine portable courante (dossier de l'exécutable, ou l'override de
    /// test s'il est actif).
    static QString root();

    /// Sous-dossier de la racine portable, créé s'il n'existe pas déjà.
    /// Accepte un chemin à plusieurs segments (ex. "data/profiles").
    /// ex : ensureSubdir("logs") -> "<root>/logs"
    static QString ensureSubdir(const QString& relativeSubdir);

    /// Chemin d'un fichier sous un sous-dossier portable, le sous-dossier
    /// étant créé s'il n'existe pas déjà.
    /// ex : filePath("logs", "app.log") -> "<root>/logs/app.log"
    static QString filePath(const QString& relativeSubdir, const QString& fileName);

    /// Test-only : force la racine retournée par root()/ensureSubdir()/
    /// filePath() à un dossier explicite (fixture isolée), indépendamment de
    /// applicationDirPath(). Passer une chaîne vide annule l'override et
    /// revient au comportement par défaut.
    static void setTestRootOverride(const QString& root);

    /// UX-PRODUIT-15 -- distinct de setTestRootOverride (jamais utilisé en
    /// production) : override de production pour un processus enfant isolé
    /// (session tutoriel), appelé UNE SEULE FOIS au tout début de main.cpp,
    /// avant QSettings::setPath/Logger/WebEngine/ApplicationController.
    /// Storage statique séparé de l'override de test : root() vérifie
    /// l'override de test en premier (comportement des tests inchangé), puis
    /// celui-ci, sinon applicationDirPath() comme aujourd'hui. L'instance
    /// normale n'appelle jamais cette méthode -- purement additif, aucun
    /// consommateur existant de root()/ensureSubdir()/filePath() n'est
    /// affecté tant qu'elle n'est pas appelée. Passer une chaîne vide annule
    /// l'override.
    static void setProductionRootOverride(const QString& root);
};

} // namespace killcore
