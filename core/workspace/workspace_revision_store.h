#pragma once

#include <QList>
#include <QString>

#include <cstdint>

namespace killcore {

/// UX-PRODUIT-13 -- Historique automatique et récupération du workspace.
/// Motif de la révision qui a produit une entrée : détermine son quota
/// d'éviction (voir kWorkspaceRevisionAutomaticCap et consorts ci-dessous).
enum class WorkspaceRevisionReason {
    Automatic,
    BeforeImport,
    BeforeRestore,
    Manual,
};

QString workspaceRevisionReasonToString(WorkspaceRevisionReason reason);
bool workspaceRevisionReasonFromString(const QString& text, WorkspaceRevisionReason* out);

struct WorkspaceRevisionMetadata {
    QString id;                 // QUuid sans accolades
    QString createdAtUtc;       // ISO 8601 UTC (Qt::ISODateWithMs) -- trie lexicographiquement dans l'ordre chronologique
    WorkspaceRevisionReason reason{WorkspaceRevisionReason::Automatic};
    QString projectContext;     // facultatif
    QString targetName;         // facultatif, nom de process historique
    qint64 payloadSizeBytes{0};
    QString payloadSha256;      // hex minuscule, sur les octets UTF-8 exacts du payload
};

struct WorkspaceRevisionListEntry {
    WorkspaceRevisionMetadata metadata;
    bool valid{true};
    QString error;   // peuplé si valid=false (JSON invalide, schéma inconnu, SHA incorrect...)
};

struct WorkspaceRevisionCreateResult {
    bool success{false};
    QString error;
    WorkspaceRevisionMetadata metadata;   // valide seulement si success
};

struct WorkspaceRevisionReadResult {
    bool success{false};
    QString error;
    WorkspaceRevisionMetadata metadata;
    QString payloadJson;   // valide seulement si success -- texte JSON exact du snapshot frontend
};

constexpr int kWorkspaceRevisionAutomaticCap = 20;
constexpr int kWorkspaceRevisionProtectionCap = 20;   // BeforeImport + BeforeRestore combinés
constexpr int kWorkspaceRevisionManualCap = 10;
constexpr qint64 kWorkspaceRevisionMaxPayloadBytes = 10LL * 1024 * 1024;
constexpr qint64 kWorkspaceRevisionMaxTotalBytes = 100LL * 1024 * 1024;
/// AUDIT-PIPE-A7 : borne défensive sur la taille du FICHIER .kwrev lu (pas
/// seulement le payload) -- une enveloppe externe/corrompue anormalement
/// grosse ne doit jamais être chargée intégralement en mémoire avant d'être
/// rejetée. Généreux au-delà de kWorkspaceRevisionMaxPayloadBytes pour
/// couvrir l'échappement JSON du payload (jusqu'à ~2x dans le pire cas) plus
/// l'enveloppe (métadonnées, toujours petites).
constexpr qint64 kWorkspaceRevisionMaxEnvelopeFileBytes = 32LL * 1024 * 1024;

/// Registre de révisions immuables du workspace, persistées sur disque
/// portable (un fichier JSON par révision, écriture atomique QSaveFile --
/// même motif que core/profiles/profile_store.cpp::save). Aucun manifeste :
/// l'index est reconstruit en scannant le répertoire et en lisant chaque
/// enveloppe -- une révision illisible/tronquée/à la somme de contrôle
/// incorrecte est isolée avec une erreur dans la liste plutôt que de casser
/// l'énumération des autres (contrairement à ProfileStore::listProfiles, qui
/// ne valide pas le contenu au listing -- nouveauté ajoutée ici).
///
/// Pure logique, pas de QObject : instance possédée directement par
/// ApplicationController (pas de glue Qt dédiée, contrairement à
/// ActivityManager de UX-PRODUIT-12 -- ce store est un CRUD synchrone sans
/// notification à pousser).
class WorkspaceRevisionStore {
public:
    explicit WorkspaceRevisionStore(QString directory);

    /// Vérifie la taille AVANT d'écrire (refus net si payloadJson dépasse
    /// kWorkspaceRevisionMaxPayloadBytes, jamais de troncature). Si
    /// l'écriture disque échoue, aucune révision existante n'est touchée.
    /// Si elle réussit, l'éviction par quota s'applique ENSUITE (l'écriture
    /// réussie ne peut jamais être annulée par un manque de place ultérieur).
    WorkspaceRevisionCreateResult create(WorkspaceRevisionReason reason,
                                          const QString& projectContext,
                                          const QString& targetName,
                                          const QString& payloadJson);

    /// Plus récente d'abord. Une entrée invalide (voir WorkspaceRevisionListEntry::valid)
    /// reste dans la liste avec son erreur, jamais silencieusement filtrée.
    QList<WorkspaceRevisionListEntry> list(int offset, int limit) const;

    WorkspaceRevisionReadResult read(const QString& id) const;

    /// Suppression explicite uniquement -- jamais appelée automatiquement
    /// pour une révision manuelle (voir enforceQuotas).
    bool remove(const QString& id, QString* error = nullptr);

private:
    void rebuildIndexIfNeeded() const;
    void enforceQuotas(WorkspaceRevisionReason justWrittenReason);
    QString filePathForId(const QString& id) const;

    QString m_directory;
    mutable QList<WorkspaceRevisionListEntry> m_indexCache;   // plus récente en tête -- reconstruit à chaque appel, voir .cpp
};

} // namespace killcore
