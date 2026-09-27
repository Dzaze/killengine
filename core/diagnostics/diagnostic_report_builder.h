#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>

namespace killcore {

/**
 * @brief UX-PRODUIT-17 -- bornes dures du rapport de problème, partagées par
 * l'assembleur (ci-dessous) et la couche Qt (apps/desktop/settings_diagnostics_manager.*)
 * qui fait les lectures fichier réelles avant de les lui passer.
 */
struct DiagnosticReportLimits {
    static constexpr qint64 kMaxSectionBytes = 256 * 1024;
    static constexpr int kMaxSections = 32;
    static constexpr qint64 kMaxTotalBytes = 2 * 1024 * 1024;
    static constexpr qint64 kMaxNarrativeBytes = 64 * 1024;
    static constexpr qint64 kMaxPageBytes = 16 * 1024;
    static constexpr qint64 kTtlMs = 10 * 60 * 1000;
};

struct DiagnosticNarrative {
    QString steps;
    QString expected;
    QString observed;
};

/// Une section déjà lue (bornée côté appelant -- ce module ne fait aucune E/S
/// fichier) prête à être rédigée/bornée/assemblée.
struct DiagnosticRawSection {
    QString id;
    QString title;
    QString content;
    /// true si l'appelant a déjà dû omettre des données à la source (ex. tail
    /// de log au-delà d'une fenêtre) -- reporté tel quel dans la section finale.
    bool sourceOmittedData{false};
    QString omissionNote;
};

struct DiagnosticProvenance {
    QString engineVersion;
    /// Vide sauf variable d'environnement KILLENGINE_BUILD_ID définie -- jamais deviné.
    QString buildId;
    /// Vide si non calculable (fichier trop gros / temps dépassé / absent).
    QString executableSha256;
    /// "packaged" | "dev" | "qrc-placeholder" | "none".
    QString uiBundleOrigin;
    QStringList uiAssetFiles;
    /// "matches" | "changed_on_disk" | "unknown".
    QString uiFingerprintStatus;
    QString osName;
    QString architecture;
};

/// Un événement structuré de session, fusionné depuis ActivityRegistry
/// (moteur) et le store actionLog (UI) -- voir buildDiagnosticReport.
struct DiagnosticEvent {
    /// "activity" | "actionLog".
    QString source;
    QString operationId;
    QString kind;
    QString state;
    QString summary;
    qint64 timestampMs{0};
};

struct DiagnosticSection {
    QString id;
    QString title;
    QByteArray content;
    bool truncated{false};
    qint64 omittedBytes{0};
    QString note;
};

struct PreparedDiagnosticReport {
    QString reportId;
    qint64 preparedAtMs{0};
    DiagnosticNarrative narrative;
    DiagnosticProvenance provenance;
    QList<DiagnosticSection> sections;
    QList<DiagnosticEvent> events;
    qint64 totalBytes{0};
    QString payloadSha256;
};

struct BuildResult {
    bool ok{true};
    QString error;
    PreparedDiagnosticReport report;
};

struct BoundedTextResult {
    bool ok{true};
    QString text;
    QString error;
};

struct SectionPage {
    QByteArray data;
    /// -1 si la fin de la section est atteinte.
    qint64 nextOffset{-1};
    bool ok{true};
    QString error;
};

/// Remplace chaque racine connue (clé non vide de `rootsToLabels`) par son
/// label stable partout où elle apparaît dans `input`. Les racines les plus
/// longues sont substituées en premier pour éviter qu'une racine courte
/// masque une racine plus longue qui la contient (ex. dossier temp sous le
/// profil utilisateur).
QString redactKnownRoots(const QString& input, const QHash<QString, QString>& rootsToLabels);

/// Borne `text` à DiagnosticReportLimits::kMaxNarrativeBytes en UTF-8.
/// `ok=false` (jamais de troncature silencieuse) si le texte encodé dépasse
/// la borne -- l'appelant doit proposer un texte plus court.
BoundedTextResult boundNarrativeField(const QString& text);

/// Assemble le rapport complet : rédaction (narrative + sections + valeurs
/// texte des événements), troncature explicite section par section au-delà
/// de kMaxSectionBytes (honnête : `truncated`/`omittedBytes`, jamais un refus
/// pour ce cas précis), arrêt à kMaxSections sections et kMaxTotalBytes au
/// total, calcul du SHA-256 du payload concaténé (manifeste + sections, dans
/// l'ordre). Les narrations doivent déjà avoir passé boundNarrativeField.
BuildResult buildDiagnosticReport(
    const QString& reportId,
    qint64 preparedAtMs,
    const DiagnosticNarrative& narrative,
    const DiagnosticProvenance& provenance,
    const QList<DiagnosticRawSection>& rawSections,
    const QList<DiagnosticEvent>& events,
    const QHash<QString, QString>& rootsToLabels);

/// Lecture paginée d'une section déjà assemblée, bornée à
/// DiagnosticReportLimits::kMaxPageBytes. `offset` est ajusté à la frontière
/// UTF-8 valide la plus proche <= offset demandé (jamais de coupure en plein
/// milieu d'un caractère multi-octet).
SectionPage readSectionPage(const DiagnosticSection& section, qint64 offset, qint64 limit);

/// SHA-256 d'un fichier, borné en temps (1.5s) et en taille (64 Mio) -- même
/// philosophie que resolutionFileHash (core/profiles/profile_store.cpp),
/// cloné en petit ici plutôt que réutilisé pour ne pas coupler ce module neuf
/// à profile_store.*. Retourne une chaîne vide si non calculable.
QString hashFileBounded(const QString& path);

} // namespace killcore
