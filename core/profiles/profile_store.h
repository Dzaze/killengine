#pragma once

#include "locator.h"
#include "scanner/scan_types.h"

#include <QString>
#include <QStringList>
#include <QList>
#include <QJsonObject>
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
    QStringList dependsOn;    // noms d'autres cibles du profil à activer avant celle-ci
    QString   ghidraSymbol;   // nom importé depuis Ghidra, sans renommer la cible KillEngine
    QString   ghidraNote;
};

struct ProfileCodePatch {
    QString name;
    QString module;
    uint64_t moduleOffset{0};
    QString aobPattern;
    QString patchBytes;
    QString originalBytes;
    QString disassembly;
    QString riskLevel;
    QString description;
    QString ghidraSymbol;
    QString ghidraNote;
    int signatureScore{0};
    QString signatureLevel;
    QString signatureWarning;
    int signatureFixedBytes{0};
    int signatureWildcardBytes{0};
    int signatureUniqueFixedBytes{0};
    double signatureFixedRatio{0.0};
    bool trainerSafe{false};
    int signatureMatches{0};
};

/**
 * @brief Script auto-assembleur (mini-langage CE-style) sauvegardé, rejouable sans
 *        retaper le texte à chaque session.
 */
struct ProfileAutoAsmScript {
    QString name;
    QString scriptText;
    QString description;
    QString riskLevel;
};

/**
 * @brief Script Lua externe sauvegardé (voir docs/POWER_UP_ROADMAP.md section K),
 *        rejouable sans retaper le texte à chaque session.
 */
struct ProfileLuaScript {
    QString name;
    QString scriptText;
    QString description;
    qint64  savedAtEpochMs{0};
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
    QList<ProfileCodePatch> patches;
    QList<ProfileAutoAsmScript> autoAsmScripts;
    QList<ProfileLuaScript> luaScripts;

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
    struct PointerMapImportResult {
        int imported{0};
        int replaced{0};
        int skipped{0};
        QStringList messages;
    };

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

    /// Exporte les cibles pointer_chain d'un profil en JSON partageable.
    static QJsonObject exportPointerMap(const Profile& profile);

    /// Fusionne une pointer map exportée dans un profil existant ou nouveau.
    static PointerMapImportResult mergePointerMap(Profile* profile, const QJsonObject& pointerMap, bool replaceExisting);
};

} // namespace killcore
