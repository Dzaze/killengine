#pragma once

#include "locator.h"
#include "scanner/scan_types.h"

#include <QString>
#include <QList>
#include <cstdint>

namespace killcore {

/**
 * @brief Une cible découverte et sauvegardée dans un profil.
 */
struct ProfileTarget {
    QString   name;           // ex: "Money", "Health"
    ValueType type{ValueType::Int32};
    Locator   locator;
    QString   description;    // optionnel
};

/**
 * @brief Profil réutilisable pour un jeu/exécutable.
 *
 * Permet de retrouver des cibles après redémarrage via les locators module_offset.
 */
struct Profile {
    QString              gameName;       // ex: "KillEngine Test Target"
    QString              executableName; // ex: "KillEngineTestTarget.exe"
    QString              executableHash; // SHA-256 de l'exécutable (vide si non calculé)
    QList<ProfileTarget> targets;

    /// Version du format de profil (pour migrations futures).
    static constexpr int FORMAT_VERSION = 1;
};

/**
 * @brief Stockage et gestion des profils KillEngine.
 *
 * Les profils sont stockés dans %LOCALAPPDATA%\KillEngine\Profiles\
 * au format JSON (.keprofile).
 */
class ProfileStore {
public:
    /// Retourne le dossier de stockage des profils.
    static QString profilesDir();

    /// Crée le dossier de stockage si nécessaire.
    static bool ensureProfilesDir();

    /// Sauvegarde un profil dans un fichier .keprofile.
    static bool save(const Profile& profile, const QString& filename);

    /// Charge un profil depuis un fichier .keprofile.
    static bool load(const QString& filename, Profile* profile);

    /// Liste tous les profils disponibles (.keprofile dans le dossier).
    static QStringList listProfiles();

    /// Retourne le chemin complet d'un profil par son nom.
    static QString profilePath(const QString& profileName);

    /// Supprime un profil.
    static bool remove(const QString& profileName);
};

} // namespace killcore