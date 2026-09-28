#include "workspace_revision_store.h"

#include "../logging/logger.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>

namespace killcore {

namespace {

constexpr int kSchemaVersion = 1;

const QRegularExpression& idPattern() {
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    return pattern;
}

bool isSafeId(const QString& id) {
    return idPattern().match(id).hasMatch();
}

QString sha256Hex(const QByteArray& bytes) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(bytes);
    return QString::fromLatin1(hash.result().toHex());
}

bool isProtectionReason(WorkspaceRevisionReason reason) {
    return reason == WorkspaceRevisionReason::BeforeImport || reason == WorkspaceRevisionReason::BeforeRestore;
}

/// AUDIT-PIPE-A7 : validation ÉLÉMENTAIRE du payload workspace (JSON objet,
/// version numérique supportée, lastPresetId textuel) -- délibérément PAS la
/// validation métier profonde par section (investigation/trainer/structures/
/// bookmarks/audit), qui reste l'unique responsabilité de
/// validateWorkspaceImport() côté TS (ui/src/stores/workspaceImportValidation.ts),
/// déjà exécutée à la restauration (workspaceSession.ts::importWorkspaceJson
/// avec source='revision_restore'). Dupliquer cette validation profonde ici
/// créerait exactement le risque que la fiche demande d'éviter : deux
/// validateurs métier qui peuvent diverger. Ce contrôle-ci n'attrape que ce
/// qu'aucune restauration ne pourra jamais rendre lisible (JSON cassé, racine
/// du mauvais type, champs racine élémentaires absents/mal typés) --
/// suffisant pour ne plus stocker/lister comme "valid" un payload qui ne
/// pourra jamais être restauré, sans prétendre garantir la restaurabilité
/// complète.
bool isPlausibleWorkspacePayload(const QString& payloadJson, QString* errorOut) {
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(payloadJson.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (errorOut) *errorOut = QStringLiteral("JSON invalide : %1").arg(parseError.errorString());
        return false;
    }
    if (!doc.isObject()) {
        if (errorOut) {
            *errorOut = QStringLiteral("La racine doit être un objet JSON (racine %1 reçue).")
                .arg(doc.isArray() ? QStringLiteral("tableau") : QStringLiteral("scalaire"));
        }
        return false;
    }
    const QJsonObject root = doc.object();
    // Version en dur ICI, à tenir synchronisée avec
    // WORKSPACE_IMPORT_SUPPORTED_VERSIONS (workspaceImportValidation.ts) --
    // volontairement un simple entier plutôt qu'une structure dupliquée, pour
    // limiter le risque de divergence à un seul point de comparaison.
    if (!root.value(QStringLiteral("version")).isDouble() || root.value(QStringLiteral("version")).toInt(-1) != 1) {
        if (errorOut) *errorOut = QStringLiteral("Champ 'version' manquant, non numérique, ou non supporté.");
        return false;
    }
    if (!root.value(QStringLiteral("lastPresetId")).isString()) {
        if (errorOut) *errorOut = QStringLiteral("Champ 'lastPresetId' manquant ou non textuel.");
        return false;
    }
    return true;
}

/// Lit et valide une seule enveloppe .kwrev (JSON, schéma, motif, SHA-256).
/// Utilisée à la fois pour lister (index reconstruit depuis les enveloppes,
/// jamais depuis un manifeste séparé) et pour lire une révision précise --
/// une seule implémentation de la validation, pas deux qui pourraient dériver.
WorkspaceRevisionListEntry parseEnvelopeFile(const QString& path, const QString& fallbackId) {
    WorkspaceRevisionListEntry entry;
    entry.metadata.id = fallbackId;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        entry.valid = false;
        entry.error = QStringLiteral("Fichier illisible.");
        return entry;
    }
    // AUDIT-PIPE-A7 : vérifie la taille AVANT readAll() -- une enveloppe
    // externe/corrompue anormalement grosse ne doit jamais être chargée
    // intégralement en mémoire juste pour être rejetée ensuite.
    if (file.size() > kWorkspaceRevisionMaxEnvelopeFileBytes) {
        entry.valid = false;
        entry.error = QStringLiteral("Fichier de révision anormalement volumineux (%1 octets, max %2).")
            .arg(file.size())
            .arg(kWorkspaceRevisionMaxEnvelopeFileBytes);
        return entry;
    }
    const QByteArray raw = file.readAll();
    file.close();

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        entry.valid = false;
        entry.error = QStringLiteral("JSON invalide : %1").arg(parseError.errorString());
        return entry;
    }

    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("schemaVersion")).toInt(-1) != kSchemaVersion) {
        entry.valid = false;
        entry.error = QStringLiteral("Version de schéma inconnue.");
        return entry;
    }

    WorkspaceRevisionReason reason = WorkspaceRevisionReason::Automatic;
    if (!workspaceRevisionReasonFromString(root.value(QStringLiteral("reason")).toString(), &reason)) {
        entry.valid = false;
        entry.error = QStringLiteral("Motif de révision inconnu.");
        return entry;
    }

    const QString payload = root.value(QStringLiteral("payload")).toString();
    const QString expectedSha = root.value(QStringLiteral("payloadSha256")).toString();
    const QString actualSha = sha256Hex(payload.toUtf8());
    if (expectedSha.isEmpty() || actualSha.compare(expectedSha, Qt::CaseInsensitive) != 0) {
        entry.valid = false;
        entry.error = QStringLiteral("Somme de contrôle incorrecte (fichier corrompu ou tronqué).");
        return entry;
    }

    // Métadonnées peuplées AVANT le contrôle de plausibilité qui suit --
    // AUDIT-PIPE-A7 : une révision dont l'enveloppe est intacte (SHA correct,
    // ci-dessus) mais dont le PAYLOAD n'est pas un workspace plausible reste
    // identifiable (id/date/motif/taille) dans la liste, jamais masquée.
    const QString contentId = root.value(QStringLiteral("id")).toString();
    entry.metadata.id = isSafeId(contentId) ? contentId : fallbackId;
    entry.metadata.createdAtUtc = root.value(QStringLiteral("createdAtUtc")).toString();
    entry.metadata.reason = reason;
    entry.metadata.projectContext = root.value(QStringLiteral("projectContext")).toString();
    entry.metadata.targetName = root.value(QStringLiteral("targetName")).toString();
    entry.metadata.payloadSizeBytes = static_cast<qint64>(payload.toUtf8().size());
    entry.metadata.payloadSha256 = expectedSha;

    // AUDIT-PIPE-A7 : intégrité d'enveloppe confirmée (SHA ci-dessus) ne
    // prouve pas que le PAYLOAD est un workspace lisible -- distinct et
    // vérifié séparément ici, jamais confondu ("valid:true" pour une
    // enveloppe intacte contenant "{"/"[]" était exactement le défaut
    // reproduit dans cette fiche).
    QString payloadError;
    if (!isPlausibleWorkspacePayload(payload, &payloadError)) {
        entry.valid = false;
        entry.error = QStringLiteral("Enveloppe intacte, contenu du workspace invalide : %1").arg(payloadError);
        return entry;
    }

    entry.valid = true;
    return entry;
}

} // namespace

QString workspaceRevisionReasonToString(WorkspaceRevisionReason reason) {
    switch (reason) {
        case WorkspaceRevisionReason::Automatic: return QStringLiteral("automatic");
        case WorkspaceRevisionReason::BeforeImport: return QStringLiteral("before_import");
        case WorkspaceRevisionReason::BeforeRestore: return QStringLiteral("before_restore");
        case WorkspaceRevisionReason::Manual: return QStringLiteral("manual");
    }
    return QStringLiteral("automatic");
}

bool workspaceRevisionReasonFromString(const QString& text, WorkspaceRevisionReason* out) {
    if (!out) return false;
    if (text == QStringLiteral("automatic")) { *out = WorkspaceRevisionReason::Automatic; return true; }
    if (text == QStringLiteral("before_import")) { *out = WorkspaceRevisionReason::BeforeImport; return true; }
    if (text == QStringLiteral("before_restore")) { *out = WorkspaceRevisionReason::BeforeRestore; return true; }
    if (text == QStringLiteral("manual")) { *out = WorkspaceRevisionReason::Manual; return true; }
    return false;
}

WorkspaceRevisionStore::WorkspaceRevisionStore(QString directory)
    : m_directory(std::move(directory)) {
}

QString WorkspaceRevisionStore::filePathForId(const QString& id) const {
    return m_directory + QStringLiteral("/") + id + QStringLiteral(".kwrev");
}

// Aucun cache persistant : le répertoire reste l'unique source de vérité à
// chaque appel (même philosophie que ProfileStore::listProfiles, qui rescanne
// systématiquement). Un cache aurait pu masquer une modification externe du
// répertoire (autre instance, fichier corrompu déposé après coup) -- le
// détecter est justement le but des vérifications SHA/schéma ci-dessous.
// Le volume attendu (≤ 20+20+10 fichiers, 10 Mio chacun au pire) rend ce
// rescan négligeable face à la fréquence d'appel réelle (actions utilisateur
// explicites, jamais un poll).
void WorkspaceRevisionStore::rebuildIndexIfNeeded() const {
    m_indexCache.clear();

    QDir dir(m_directory);
    if (dir.exists()) {
        const QFileInfoList entries = dir.entryInfoList({QStringLiteral("*.kwrev")}, QDir::Files);
        for (const auto& info : entries) {
            m_indexCache.append(parseEnvelopeFile(info.absoluteFilePath(), info.completeBaseName()));
        }
    }

    std::sort(m_indexCache.begin(), m_indexCache.end(), [](const WorkspaceRevisionListEntry& a, const WorkspaceRevisionListEntry& b) {
        return a.metadata.createdAtUtc > b.metadata.createdAtUtc;   // plus récente d'abord
    });
}

WorkspaceRevisionCreateResult WorkspaceRevisionStore::create(WorkspaceRevisionReason reason,
                                                               const QString& projectContext,
                                                               const QString& targetName,
                                                               const QString& payloadJson) {
    WorkspaceRevisionCreateResult result;

    const QByteArray payloadBytes = payloadJson.toUtf8();
    if (payloadBytes.size() > kWorkspaceRevisionMaxPayloadBytes) {
        result.error = QStringLiteral("Le contenu à sauvegarder dépasse la limite de 10 Mio par révision.");
        return result;
    }

    // AUDIT-PIPE-A7 : validation élémentaire AVANT toute progression (quota,
    // index, écriture disque) -- un payload structurellement invalide
    // (JSON cassé, racine non-objet, champs élémentaires absents/mal typés)
    // ne doit jamais consommer un slot de quota ni être stocké comme une
    // révision "valid". Ne duplique pas la validation métier profonde par
    // section, qui reste côté TS (voir isPlausibleWorkspacePayload ci-dessus).
    QString payloadError;
    if (!isPlausibleWorkspacePayload(payloadJson, &payloadError)) {
        result.error = QStringLiteral("Contenu du workspace invalide : %1").arg(payloadError);
        return result;
    }

    rebuildIndexIfNeeded();

    // Les manuelles ne sont JAMAIS purgées automatiquement (fiche point 3) --
    // le cap de 10 est donc appliqué en REFUSANT une 11e création plutôt
    // qu'en évinçant silencieusement une ancienne sauvegarde intentionnelle
    // de l'utilisateur. Automatique/protection restent purgeables après coup
    // (voir enforceQuotas), donc pas de refus symétrique pour elles ici.
    if (reason == WorkspaceRevisionReason::Manual) {
        int manualCount = 0;
        for (const auto& entry : m_indexCache) {
            if (entry.metadata.reason == WorkspaceRevisionReason::Manual) ++manualCount;
        }
        if (manualCount >= kWorkspaceRevisionManualCap) {
            result.error = QStringLiteral("Limite de %1 révisions manuelles atteinte -- supprimez-en une avant d'en créer une nouvelle.").arg(kWorkspaceRevisionManualCap);
            return result;
        }
    }

    if (!QDir(m_directory).exists() && !QDir().mkpath(m_directory)) {
        result.error = QStringLiteral("Impossible de créer le répertoire d'historique.");
        return result;
    }

    WorkspaceRevisionMetadata metadata;
    metadata.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    metadata.createdAtUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    metadata.reason = reason;
    metadata.projectContext = projectContext;
    metadata.targetName = targetName;
    metadata.payloadSizeBytes = static_cast<qint64>(payloadBytes.size());
    metadata.payloadSha256 = sha256Hex(payloadBytes);

    QJsonObject root;
    root[QStringLiteral("schemaVersion")] = kSchemaVersion;
    root[QStringLiteral("id")] = metadata.id;
    root[QStringLiteral("createdAtUtc")] = metadata.createdAtUtc;
    root[QStringLiteral("reason")] = workspaceRevisionReasonToString(reason);
    root[QStringLiteral("projectContext")] = metadata.projectContext;
    root[QStringLiteral("targetName")] = metadata.targetName;
    root[QStringLiteral("payloadSha256")] = metadata.payloadSha256;
    root[QStringLiteral("payload")] = payloadJson;

    const QByteArray encoded = QJsonDocument(root).toJson(QJsonDocument::Compact);

    QSaveFile file(filePathForId(metadata.id));
    if (!file.open(QIODevice::WriteOnly)) {
        result.error = QStringLiteral("Impossible d'écrire la révision (disque ?).");
        return result;
    }
    if (file.write(encoded) != encoded.size() || !file.commit()) {
        result.error = QStringLiteral("Échec de l'écriture de la révision.");
        return result;
    }

    // Écriture réussie : jamais annulée par la purge qui suit.
    enforceQuotas(reason);

    result.success = true;
    result.metadata = metadata;
    KE_LOG_INFO() << "WorkspaceRevisionStore: created revision " << metadata.id.toStdString()
                  << " (" << workspaceRevisionReasonToString(reason).toStdString() << ", "
                  << metadata.payloadSizeBytes << " bytes)";
    return result;
}

void WorkspaceRevisionStore::enforceQuotas(WorkspaceRevisionReason justWrittenReason) {
    // Les manuelles ne participent jamais à une éviction automatique -- déjà
    // bornées en amont par le refus de création dans create().
    if (justWrittenReason != WorkspaceRevisionReason::Manual) {
        rebuildIndexIfNeeded();

        const int cap = isProtectionReason(justWrittenReason) ? kWorkspaceRevisionProtectionCap : kWorkspaceRevisionAutomaticCap;
        auto sameCategory = [&](const WorkspaceRevisionListEntry& entry) {
            return isProtectionReason(justWrittenReason)
                ? isProtectionReason(entry.metadata.reason)
                : entry.metadata.reason == WorkspaceRevisionReason::Automatic;
        };

        int categoryCount = 0;
        for (const auto& entry : m_indexCache) {
            if (sameCategory(entry)) ++categoryCount;
        }
        while (categoryCount > cap) {
            // m_indexCache est trié plus récent d'abord -- le dernier élément
            // qui matche est donc le plus ancien.
            int oldestIndex = -1;
            for (int i = m_indexCache.size() - 1; i >= 0; --i) {
                if (sameCategory(m_indexCache.at(i))) { oldestIndex = i; break; }
            }
            if (oldestIndex < 0) break;
            QFile::remove(filePathForId(m_indexCache.at(oldestIndex).metadata.id));
            m_indexCache.removeAt(oldestIndex);
            --categoryCount;
        }
    }

    // Budget total en octets, toutes catégories -- ne purge jamais les
    // manuelles (seul remove() explicite les retire).
    rebuildIndexIfNeeded();
    qint64 total = 0;
    for (const auto& entry : m_indexCache) total += entry.metadata.payloadSizeBytes;
    while (total > kWorkspaceRevisionMaxTotalBytes) {
        int oldestIndex = -1;
        for (int i = m_indexCache.size() - 1; i >= 0; --i) {
            if (m_indexCache.at(i).metadata.reason != WorkspaceRevisionReason::Manual) { oldestIndex = i; break; }
        }
        if (oldestIndex < 0) break;   // plus rien d'évictable (que des manuelles)
        total -= m_indexCache.at(oldestIndex).metadata.payloadSizeBytes;
        QFile::remove(filePathForId(m_indexCache.at(oldestIndex).metadata.id));
        m_indexCache.removeAt(oldestIndex);
    }
}

QList<WorkspaceRevisionListEntry> WorkspaceRevisionStore::list(int offset, int limit) const {
    rebuildIndexIfNeeded();
    QList<WorkspaceRevisionListEntry> result;
    const int safeOffset = std::max(0, offset);
    const int safeLimit = limit > 0 ? limit : 20;
    for (int i = safeOffset; i < m_indexCache.size() && result.size() < safeLimit; ++i) {
        result.append(m_indexCache.at(i));
    }
    return result;
}

WorkspaceRevisionReadResult WorkspaceRevisionStore::read(const QString& id) const {
    WorkspaceRevisionReadResult result;
    if (!isSafeId(id)) {
        result.error = QStringLiteral("Identifiant de révision invalide.");
        return result;
    }
    const QString path = filePathForId(id);
    if (!QFile::exists(path)) {
        result.error = QStringLiteral("Révision introuvable.");
        return result;
    }

    const WorkspaceRevisionListEntry entry = parseEnvelopeFile(path, id);
    if (!entry.valid) {
        result.error = entry.error;
        return result;
    }

    QFile file(path);
    file.open(QIODevice::ReadOnly);
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    result.metadata = entry.metadata;
    result.payloadJson = root.value(QStringLiteral("payload")).toString();
    result.success = true;
    return result;
}

bool WorkspaceRevisionStore::remove(const QString& id, QString* error) {
    if (!isSafeId(id)) {
        if (error) *error = QStringLiteral("Identifiant de révision invalide.");
        return false;
    }
    const QString path = filePathForId(id);
    if (!QFile::exists(path)) {
        if (error) *error = QStringLiteral("Révision introuvable.");
        return false;
    }
    if (!QFile::remove(path)) {
        if (error) *error = QStringLiteral("Impossible de supprimer le fichier de révision.");
        return false;
    }
    return true;
}

} // namespace killcore
