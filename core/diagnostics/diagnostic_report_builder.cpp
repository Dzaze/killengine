#include "diagnostic_report_builder.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>

namespace killcore {

namespace {

/// Coupe `bytes` à `maxLen` sans jamais laisser un caractère UTF-8
/// multi-octet à cheval sur la frontière. Retourne le nombre d'octets omis.
qint64 truncateUtf8ToBoundary(QByteArray& bytes, qint64 maxLen) {
    if (bytes.size() <= maxLen) {
        return 0;
    }
    qint64 cut = maxLen;
    while (cut > 0 && (static_cast<unsigned char>(bytes.at(cut)) & 0xC0) == 0x80) {
        --cut;
    }
    const qint64 omitted = bytes.size() - cut;
    bytes = bytes.left(cut);
    return omitted;
}

} // namespace

QString redactKnownRoots(const QString& input, const QHash<QString, QString>& rootsToLabels) {
    if (input.isEmpty() || rootsToLabels.isEmpty()) {
        return input;
    }
    QList<QString> roots = rootsToLabels.keys();
    // Racines les plus longues en premier -- une racine temp souvent nichée
    // sous le profil utilisateur ne doit pas être masquée par un remplacement
    // plus court appliqué avant elle.
    std::sort(roots.begin(), roots.end(), [](const QString& a, const QString& b) {
        return a.size() > b.size();
    });
    QString output = input;
    for (const auto& root : roots) {
        if (root.isEmpty()) {
            continue;
        }
        output.replace(root, rootsToLabels.value(root), Qt::CaseInsensitive);
    }
    return output;
}

BoundedTextResult boundNarrativeField(const QString& text) {
    BoundedTextResult result;
    const QByteArray encoded = text.toUtf8();
    if (encoded.size() > DiagnosticReportLimits::kMaxNarrativeBytes) {
        result.ok = false;
        result.error = QStringLiteral("Texte trop long (%1 octets, max %2 octets). Raccourcis-le avant de préparer l'aperçu.")
            .arg(encoded.size())
            .arg(DiagnosticReportLimits::kMaxNarrativeBytes);
        return result;
    }
    result.text = text;
    return result;
}

BuildResult buildDiagnosticReport(
    const QString& reportId,
    qint64 preparedAtMs,
    const DiagnosticNarrative& narrative,
    const DiagnosticProvenance& provenance,
    const QList<DiagnosticRawSection>& rawSections,
    const QList<DiagnosticEvent>& events,
    const QHash<QString, QString>& rootsToLabels) {
    BuildResult result;
    PreparedDiagnosticReport& report = result.report;
    report.reportId = reportId;
    report.preparedAtMs = preparedAtMs;

    report.narrative.steps = redactKnownRoots(narrative.steps, rootsToLabels);
    report.narrative.expected = redactKnownRoots(narrative.expected, rootsToLabels);
    report.narrative.observed = redactKnownRoots(narrative.observed, rootsToLabels);

    report.provenance = provenance;
    report.provenance.uiAssetFiles.clear();
    for (const auto& file : provenance.uiAssetFiles) {
        report.provenance.uiAssetFiles.append(redactKnownRoots(file, rootsToLabels));
    }

    // Événements : rédaction du résumé, tri chronologique, cap défensif
    // (200 ActivityRegistry + 200 actionLog dans le pire cas déjà borné en
    // amont -- ce cap-ci n'est qu'un filet de sécurité).
    QList<DiagnosticEvent> sortedEvents = events;
    std::sort(sortedEvents.begin(), sortedEvents.end(), [](const DiagnosticEvent& a, const DiagnosticEvent& b) {
        return a.timestampMs < b.timestampMs;
    });
    for (auto& event : sortedEvents) {
        event.summary = redactKnownRoots(event.summary, rootsToLabels);
    }
    constexpr int kMaxEvents = 400;
    if (sortedEvents.size() > kMaxEvents) {
        sortedEvents = sortedEvents.mid(sortedEvents.size() - kMaxEvents);
    }
    report.events = sortedEvents;

    // Sections de données (hors manifeste, qui occupe toujours le premier
    // emplacement) : rédaction + troncature honnête PAR SECTION (256 Kio).
    // Le budget TOTAL (2 Mio, manifeste inclus) est appliqué dans une passe
    // finale plus bas, une fois la taille réelle du manifeste connue -- le
    // faire ici sous-estimerait le total en ignorant le manifeste.
    QList<DiagnosticSection> dataSections;
    bool sectionsOmittedForCap = false;
    for (const auto& raw : rawSections) {
        if (dataSections.size() >= DiagnosticReportLimits::kMaxSections - 1) {
            sectionsOmittedForCap = true;
            break;
        }
        const QString redactedText = redactKnownRoots(raw.content, rootsToLabels);
        QByteArray bytes = redactedText.toUtf8();

        DiagnosticSection section;
        section.id = raw.id;
        section.title = raw.title;
        section.note = raw.omissionNote;
        section.truncated = raw.sourceOmittedData;

        section.omittedBytes += truncateUtf8ToBoundary(bytes, DiagnosticReportLimits::kMaxSectionBytes);
        if (section.omittedBytes > 0) {
            section.truncated = true;
        }

        section.content = bytes;
        dataSections.append(section);
    }

    // Manifeste : provenance, narration, notice de format lisible sans
    // dépendre d'un outil externe, liste des sections/événements inclus.
    QVariantMap narrativeMap;
    narrativeMap["steps"] = report.narrative.steps;
    narrativeMap["expected"] = report.narrative.expected;
    narrativeMap["observed"] = report.narrative.observed;

    QVariantMap provenanceMap;
    provenanceMap["engineVersion"] = report.provenance.engineVersion;
    provenanceMap["buildId"] = report.provenance.buildId;
    provenanceMap["executableSha256"] = report.provenance.executableSha256;
    provenanceMap["uiBundleOrigin"] = report.provenance.uiBundleOrigin;
    provenanceMap["uiAssetFiles"] = QVariant::fromValue(QStringList(report.provenance.uiAssetFiles));
    provenanceMap["uiFingerprintStatus"] = report.provenance.uiFingerprintStatus;
    provenanceMap["os"] = report.provenance.osName;
    provenanceMap["architecture"] = report.provenance.architecture;

    QVariantList sectionIdsList;
    for (const auto& section : dataSections) {
        sectionIdsList.append(section.id);
    }

    QVariantList eventsList;
    for (const auto& event : report.events) {
        QVariantMap eventMap;
        eventMap["source"] = event.source;
        eventMap["operationId"] = event.operationId;
        eventMap["kind"] = event.kind;
        eventMap["state"] = event.state;
        eventMap["summary"] = event.summary;
        eventMap["timestampMs"] = event.timestampMs;
        eventsList.append(eventMap);
    }

    QVariantMap manifestMap;
    manifestMap["schemaVersion"] = 1;
    manifestMap["reportId"] = reportId;
    manifestMap["preparedAt"] = QDateTime::fromMSecsSinceEpoch(preparedAtMs, Qt::UTC).toString(Qt::ISODateWithMs);
    manifestMap["formatNotice"] = QStringLiteral(
        "Fichier .kezdiag KillEngine : payload compresse (qCompress, format Qt) contenant des sections "
        "texte separees par des lignes '===== <nom> ====='. La premiere section (manifest.json) decrit "
        "la provenance (version/empreintes), le scenario Etapes/Attendu/Observe rapporte par la personne "
        "qui a prepare ce rapport, et la liste des sections/evenements inclus. Aucune donnee sensible "
        "(mot de passe, jeton API, contenu de sauvegarde, memoire brute) n'est incluse par defaut ; les "
        "chemins personnels (dossier utilisateur/temp/installation) sont remplaces par des etiquettes "
        "stables entre crochets, ex. <user_home>.");
    manifestMap["narrative"] = narrativeMap;
    manifestMap["provenance"] = provenanceMap;
    manifestMap["sectionsIncluded"] = sectionIdsList;
    manifestMap["sectionsOmittedForSizeCap"] = sectionsOmittedForCap;
    manifestMap["events"] = eventsList;

    DiagnosticSection manifestSection;
    manifestSection.id = QStringLiteral("manifest");
    manifestSection.title = QStringLiteral("manifest.json");
    manifestSection.content = QJsonDocument(QJsonObject::fromVariantMap(manifestMap)).toJson(QJsonDocument::Indented);
    manifestSection.omittedBytes = truncateUtf8ToBoundary(manifestSection.content, DiagnosticReportLimits::kMaxSectionBytes);
    manifestSection.truncated = manifestSection.omittedBytes > 0;

    // Budget TOTAL, manifeste inclus (toujours en premier donc presque
    // toujours entièrement conservé) : accumule dans l'ordre, tronque la
    // première section qui ferait dépasser, abandonne tout ce qui suit --
    // c'est ici, pas dans la boucle des sections de données ci-dessus, que
    // ce budget doit être appliqué pour être exact.
    QList<DiagnosticSection> candidateSections;
    candidateSections.append(manifestSection);
    candidateSections.append(dataSections);

    QList<DiagnosticSection> finalSections;
    qint64 runningTotal = 0;
    for (auto section : candidateSections) {
        if (runningTotal >= DiagnosticReportLimits::kMaxTotalBytes) {
            sectionsOmittedForCap = true;
            break;
        }
        const qint64 remaining = DiagnosticReportLimits::kMaxTotalBytes - runningTotal;
        if (section.content.size() > remaining) {
            section.omittedBytes += truncateUtf8ToBoundary(section.content, remaining);
            section.truncated = true;
            sectionsOmittedForCap = true;
            finalSections.append(section);
            runningTotal += section.content.size();
            break;
        }
        runningTotal += section.content.size();
        finalSections.append(section);
    }

    qint64 totalBytes = 0;
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    for (const auto& section : finalSections) {
        totalBytes += section.content.size();
        hasher.addData(section.id.toUtf8());
        hasher.addData(section.content);
    }

    report.sections = finalSections;
    report.totalBytes = totalBytes;
    report.payloadSha256 = QString::fromLatin1(hasher.result().toHex());
    return result;
}

SectionPage readSectionPage(const DiagnosticSection& section, qint64 offset, qint64 limit) {
    SectionPage page;
    const QByteArray& data = section.content;

    if (offset < 0 || offset > data.size()) {
        page.ok = false;
        page.error = QStringLiteral("Offset hors bornes.");
        return page;
    }

    const qint64 boundedLimit = std::min<qint64>(
        (limit > 0) ? limit : DiagnosticReportLimits::kMaxPageBytes,
        DiagnosticReportLimits::kMaxPageBytes);

    qint64 start = offset;
    while (start > 0 && start < data.size() && (static_cast<unsigned char>(data.at(start)) & 0xC0) == 0x80) {
        --start;
    }

    if (start >= data.size()) {
        page.nextOffset = -1;
        return page;
    }

    qint64 end = std::min<qint64>(data.size(), start + boundedLimit);
    while (end > start && end < data.size() && (static_cast<unsigned char>(data.at(end)) & 0xC0) == 0x80) {
        --end;
    }
    if (end <= start) {
        // Le caractère multi-octet qui commence à `start` est plus grand que
        // la page demandée : ne jamais couper en plein milieu (ça produirait
        // une séquence UTF-8 invalide). Avance jusqu'à la fin de CE caractère
        // complet plutôt que de revenir à start+boundedLimit (qui recoupait
        // exactement au même endroit invalide) -- légèrement au-delà de la
        // limite demandée, jamais un souci en usage réel où limit >> 4 octets.
        end = start + 1;
        while (end < data.size() && (static_cast<unsigned char>(data.at(end)) & 0xC0) == 0x80) {
            ++end;
        }
    }

    page.data = data.mid(start, end - start);
    page.nextOffset = (end < data.size()) ? end : -1;
    return page;
}

QString hashFileBounded(const QString& path) {
    if (path.isEmpty()) {
        return QString();
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    constexpr qint64 kMaxBytes = 64 * 1024 * 1024;
    if (file.size() > kMaxBytes) {
        return QString();
    }

    QElapsedTimer timer;
    timer.start();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (timer.elapsed() > 1500) {
            return QString();
        }
        const QByteArray block = file.read(64 * 1024);
        if (block.isEmpty()) {
            break;
        }
        hash.addData(block);
    }
    if (file.error() != QFileDevice::NoError) {
        return QString();
    }
    return QString::fromLatin1(hash.result().toHex());
}

} // namespace killcore
