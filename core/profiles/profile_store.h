#pragma once

#include "locator.h"
#include "scanner/scan_types.h"

#include <QString>
#include <QStringList>
#include <QList>
#include <QJsonObject>
#include <cstdint>

namespace killcore {

/// Optional R2 metadata. A historical observation never certifies a new session.
/// expectedBytes describes deliberately stable bytes, not a fluctuating counter.
struct ProfileResolutionPlan {
    bool invalid{false}; // Malformed imported metadata must not weaken checks.
    QString discoveryMethod;
    QString expectedBytes;
    QString validationTest;
    QString executableHash;
    QString recordedSession;
    QString recordedAt;
    QString evidenceNote;
    QJsonObject baselineObservation;
    QJsonObject moduleHashes;
    QList<Locator> alternatives;
    QStringList alternativeNames;
};

struct ProfileResolutionContext {
    QString executableName;
    QString executableHash;
    QString session;
    QJsonObject moduleHashes;
};

/// Identité minimale du process attaché (AM-5, docs/PHASE_TRACKER.md,
/// 16/09/2026) : mêmes deux champs que ProfileResolutionContext::executableHash/
/// session, exposés seuls pour un appelant qui n'a pas besoin des empreintes de
/// modules complètes (ex. tamponner une preuve d'effet avec une identité de
/// session/version réelle, sans dépendre du sous-système de résolution de
/// locators). Vide si `process` n'est pas valide.
struct ProcessIdentity {
    QString executableHash;
    QString sessionId;
};
ProcessIdentity currentProcessIdentity(const ProcessHandle& process);

/// Pure decision function. Observations correspond to primary + alternatives.
/// Returns machine codes, observations and a required test; never authorizes a write.
QJsonObject diagnoseProfileResolution(const QString& expectedExecutable,
                                     const QString& legacyExecutableHash,
                                     const Locator& primary,
                                     const ProfileResolutionPlan& plan,
                                     const ProfileResolutionContext& context,
                                     const QList<LocatorProbe>& observations);

/// PRODUIT-R section R4 (docs/POWER_UP_ROADMAP.md) : mémoire d'enquête réutilisable
/// associée à une cible/patch de profil, distincte du carnet d'hypothèses
/// (ai/investigation_notebook.*, raisonnement en session, jamais persisté) et du
/// registre de preuves d'effet (ai/effect_proof_ledger.*, également en mémoire).
/// Un ProfileTarget/ProfileCodePatch accumule ici des notes texte qui survivent
/// aux redémarrages via le .keprofile existant -- rien ici ne duplique le
/// stockage ou les scores de PHASE 120 (carnet) ni de Pattern Learning
/// (core/pattern_learning/game_profile_database.*, indexé par nom de jeu, pas
/// par cible/preuve).
enum class KnowledgeNoteKind {
    ExplainedFailure,        // une piste testée qui n'a pas marché, et pourquoi
    SuccessCondition,        // ce qui a fallu pour qu'une résolution tienne
    DiscriminatingExperiment,// un test qui a permis de départager des hypothèses
                             // concurrentes (vocabulaire conseillé : les codes
                             // "experiment" de core/scanner/visual_change_correlator.cpp,
                             // ex. change_maximum_keep_current -- non imposé ici,
                             // profiles/ ne dépend pas de scanner/)
    Recheck,                 // un doute explicite à revérifier avant de réutiliser
};

QString knowledgeNoteKindToString(KnowledgeNoteKind kind);
/// Retourne false (et laisse *kind inchangé) si le texte ne correspond à aucun
/// type connu -- permet de distinguer un type explicite d'un type absent.
bool knowledgeNoteKindFromString(const QString& text, KnowledgeNoteKind* kind);

struct KnowledgeNote {
    QString id;
    bool invalid{false};      // métadonnées importées mal formées, conservées mais signalées
    KnowledgeNoteKind kind{KnowledgeNoteKind::Recheck};
    QString description;      // pourquoi cette note existe
    QString experiment;       // optionnel : quelle expérience a permis de trancher
    QString evidenceNote;     // optionnel : preuve/observation associée
    QString sessionId;        // session d'enregistrement (PID+création), vide si non rattaché
    QString executableHash;   // version de la cible au moment de l'enregistrement, vide si inconnue
    QString recordedAt;       // horodatage ISO UTC
};

/// Fonction pure, lecture seule : sépare les notes encore valables (version
/// courante identique) de celles rendues incertaines par un changement de
/// version (`stale`) ou jamais versionnées (`unversioned`, ni confirmées ni
/// contredites par cette dimension). Ne modifie jamais les notes stockées --
/// un changement de version signale la connaissance à revérifier, il ne la
/// supprime pas et ne continue pas à lui faire confiance en silence.
QJsonObject evaluateKnowledgeNotes(const QList<KnowledgeNote>& notes, const QString& currentExecutableHash);

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
    ProfileResolutionPlan resolutionPlan;
    QList<KnowledgeNote> knowledgeNotes; // R4, borné à kMaxKnowledgeNotesPerEntry
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
    ProfileResolutionPlan resolutionPlan;
    QList<KnowledgeNote> knowledgeNotes; // R4, borné à kMaxKnowledgeNotesPerEntry
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

    /// Read-only diagnostics, capped at 256 entries and 8 alternatives per entry.
    static QJsonObject diagnose(const Profile& profile, const ProcessHandle& process);

    /// Configure an entry without modifying its locator or writing target memory.
    /// Alternative names refer to entries of the same kind in this profile.
    static bool setResolutionPlan(Profile* profile, const QString& entryKind,
                                  const QString& entryName, const QJsonObject& options,
                                  const ProcessHandle& process, QString* error);

    static constexpr int kMaxKnowledgeNotesPerEntry = 50;

    /// R4 : ajoute une note de connaissance à une cible/patch existante (jamais
    /// une nouvelle entrée). `options` attend {kind, description, experiment?,
    /// evidenceNote?}. Si `process` est attaché, la note est versionnée avec
    /// l'empreinte exécutable/session courante ; sinon elle reste non versionnée
    /// (jamais contredite par un changement de version, honnête plutôt que faux).
    /// Retourne l'id généré, ou une chaîne vide avec *error rempli en cas d'échec.
    static QString addKnowledgeNote(Profile* profile, const QString& entryKind,
                                    const QString& entryName, const QJsonObject& options,
                                    const ProcessHandle& process, QString* error);

    /// Lecture seule : notes de la cible/patch + leur fraîcheur face à la version
    /// actuellement attachée (process invalide => fraîcheur non évaluée, notes
    /// retournées telles quelles).
    static QJsonObject getKnowledgeNotes(const Profile& profile, const QString& entryKind,
                                         const QString& entryName, const ProcessHandle& process);
};

} // namespace killcore
