#include "diagnostic_report_builder.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
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

/// AUDIT-PIPE-A4 : composition des deux passes de rédaction (racines/secrets
/// connus par substitution littérale, puis motifs de credentials génériques)
/// -- point d'entrée unique utilisé par buildDiagnosticReport() ci-dessous
/// pour ne jamais oublier l'une des deux passes à un des points d'appel.
QString redactAll(const QString& input, const QHash<QString, QString>& rootsToLabels) {
    return redactSecretPatterns(redactKnownRoots(input, rootsToLabels));
}

/// AUDIT-PIPE-A5 : coupe `text` à `maxChars` caractères UTF-16 sans jamais
/// laisser un substitut haut orphelin en fin de chaîne (une paire substitut
/// coupée en deux produirait un caractère invalide/de remplacement selon
/// l'encodeur JSON en aval) -- même esprit que truncateUtf8ToBoundary
/// ci-dessus, mais pour une coupure en caractères plutôt qu'en octets bruts,
/// utilisée pour borner un résumé d'événement AVANT sérialisation JSON (une
/// troncature après coup casserait le JSON, voir buildDiagnosticReport).
QString truncateUtf16ToBoundary(const QString& text, int maxChars) {
    if (text.size() <= maxChars) {
        return text;
    }
    int cut = maxChars;
    if (cut > 0 && text.at(cut - 1).isHighSurrogate()) {
        --cut;
    }
    return text.left(cut);
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

QString redactSecretPatterns(const QString& input) {
    if (input.isEmpty()) {
        return input;
    }
    QString output = input;

    // "Authorization: Bearer <token>" ou "Bearer <token>" isolé -- ne rédige
    // que le token, "Bearer" reste un signal utile au diagnostic. Jeu de
    // caractères couvrant base64/base64url + JWT (points de séparation).
    static const QRegularExpression bearerPattern(
        QStringLiteral("\\bBearer\\s+([-A-Za-z0-9_.~+/=]{6,})"),
        QRegularExpression::CaseInsensitiveOption);
    output.replace(bearerPattern, QStringLiteral("Bearer <redacted>"));

    // "api_key=...", "apiKey: ...", "password=...", "token=...", etc. -- la
    // valeur va jusqu'au prochain espace/virgule/point-virgule, ou est entre
    // guillemets. Le mot-clé est conservé (groupe 1), seule la valeur
    // (groupe 2) est rédigée.
    static const QRegularExpression assignmentPattern(
        QStringLiteral(
            "\\b(api[_-]?key|secret|access[_-]?key|client[_-]?secret|password|passwd|pwd|token)"
            "\\s*[:=]\\s*(\"[^\"]*\"|'[^']*'|[^\\s,;]+)"),
        QRegularExpression::CaseInsensitiveOption);
    output.replace(assignmentPattern, QStringLiteral("\\1=<redacted>"));

    return output;
}

BoundedNarrativeResult boundNarrativeTotal(const DiagnosticNarrative& narrative) {
    BoundedNarrativeResult result;
    const qint64 totalBytes = static_cast<qint64>(narrative.steps.toUtf8().size())
        + static_cast<qint64>(narrative.expected.toUtf8().size())
        + static_cast<qint64>(narrative.observed.toUtf8().size());
    if (totalBytes > DiagnosticReportLimits::kMaxNarrativeBytes) {
        result.ok = false;
        result.error = QStringLiteral(
            "Récit trop long (%1 octets au total pour steps+expected+observed, max %2 octets). "
            "Raccourcis-le avant de préparer l'aperçu.")
            .arg(totalBytes)
            .arg(DiagnosticReportLimits::kMaxNarrativeBytes);
        return result;
    }
    result.narrative = narrative;
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

    report.narrative.steps = redactAll(narrative.steps, rootsToLabels);
    report.narrative.expected = redactAll(narrative.expected, rootsToLabels);
    report.narrative.observed = redactAll(narrative.observed, rootsToLabels);

    report.provenance = provenance;
    report.provenance.uiAssetFiles.clear();
    for (const auto& file : provenance.uiAssetFiles) {
        // Noms de fichiers d'assets UI seulement -- pas de texte libre,
        // redactKnownRoots (chemins) suffit, redactSecretPatterns n'y a rien
        // à faire et ajouterait un coût de regex inutile ici.
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
        event.summary = redactAll(event.summary, rootsToLabels);
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
        const QString redactedText = redactAll(raw.content, rootsToLabels);
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

    // AUDIT-PIPE-A5 : chaque résumé est d'abord capé en LONGUEUR (avant
    // sérialisation), pour qu'un seul événement pathologiquement long ne
    // puisse pas à lui seul faire déborder le manifeste. Le compte
    // d'événements est ensuite réduit si besoin juste en dessous (jamais une
    // troncature d'octets JSON après coup).
    constexpr int kMaxEventSummaryChars = 500;
    QList<QVariantMap> eventMapsOldestFirst;
    eventMapsOldestFirst.reserve(report.events.size());
    for (const auto& event : report.events) {
        QVariantMap eventMap;
        eventMap["source"] = event.source;
        eventMap["operationId"] = event.operationId;
        eventMap["kind"] = event.kind;
        eventMap["state"] = event.state;
        eventMap["summary"] = truncateUtf16ToBoundary(event.summary, kMaxEventSummaryChars);
        eventMap["timestampMs"] = event.timestampMs;
        eventMapsOldestFirst.append(eventMap);
    }

    QVariantMap manifestMap;
    manifestMap["schemaVersion"] = 1;
    manifestMap["reportId"] = reportId;
    manifestMap["preparedAt"] = QDateTime::fromMSecsSinceEpoch(preparedAtMs, Qt::UTC).toString(Qt::ISODateWithMs);
    manifestMap["formatNotice"] = QStringLiteral(
        "Fichier .kezdiag KillEngine : payload compresse (qCompress, format Qt) contenant des sections "
        "texte separees par des lignes '===== <nom> ====='. La premiere section (manifest.json) decrit "
        "la provenance (version/empreintes), le scenario Etapes/Attendu/Observe rapporte par la personne "
        "qui a prepare ce rapport, et la liste des sections/evenements inclus. Les reglages inclus sont "
        "une selection positive de champs non sensibles (pas un export complet des parametres), et le "
        "journal texte n'est inclus que si explicitement demande. La cle API externe actuellement "
        "enregistree (si presente) et les motifs de credentials clairement identifiables dans le texte "
        "libre (jeton 'Bearer ...', assignations 'mot_de_passe=...'/'jeton=...'/'cle_api=...') sont "
        "rediges automatiquement -- AUDIT-PIPE-A4 : cette redaction ne peut pas garantir la detection "
        "d'un secret arbitraire ecrit sous une forme non reconnue dans un texte libre. Les chemins "
        "personnels (dossier utilisateur/temp/installation) sont remplaces par des etiquettes stables "
        "entre crochets, ex. <user_home>. Les evenements les plus anciens et/ou leurs resumes peuvent "
        "etre coupes pour tenir sous la limite de taille d'une section (voir eventsOmittedForSizeCap et "
        "eventsIncludedCount ci-dessous) -- jamais le JSON de ce manifeste lui-meme.");
    manifestMap["narrative"] = narrativeMap;
    manifestMap["provenance"] = provenanceMap;
    manifestMap["sectionsIncluded"] = sectionIdsList;
    manifestMap["sectionsOmittedForSizeCap"] = sectionsOmittedForCap;

    // AUDIT-PIPE-A5 : borner le CONTENU avant sérialisation JSON, jamais
    // tronquer les octets JSON après coup (produit un JSON invalide --
    // reproduit en audit : "Unterminated string" en plein milieu d'un
    // résumé d'événement coupé sur un manifeste de 200 événements longs).
    // Retire les événements les plus anciens un par un tant que le JSON
    // sérialisé dépasse kMaxSectionBytes ; le manifeste lui-même (hors
    // événements) reste petit et fixe (narration déjà bornée par l'appelant,
    // provenance de taille constante), donc cette boucle converge toujours
    // sans avoir besoin de secours par troncature d'octets.
    bool eventsOmittedForSizeCap = false;
    QByteArray manifestBytes;
    while (true) {
        QVariantList eventsList;
        eventsList.reserve(eventMapsOldestFirst.size());
        for (const auto& eventMap : eventMapsOldestFirst) {
            eventsList.append(eventMap);
        }
        manifestMap["events"] = eventsList;
        manifestMap["eventsIncludedCount"] = eventMapsOldestFirst.size();
        manifestMap["eventsOmittedForSizeCap"] = eventsOmittedForSizeCap;
        manifestBytes = QJsonDocument(QJsonObject::fromVariantMap(manifestMap)).toJson(QJsonDocument::Indented);
        if (manifestBytes.size() <= DiagnosticReportLimits::kMaxSectionBytes || eventMapsOldestFirst.isEmpty()) {
            break;
        }
        eventMapsOldestFirst.removeFirst();
        eventsOmittedForSizeCap = true;
    }

    DiagnosticSection manifestSection;
    manifestSection.id = QStringLiteral("manifest");
    manifestSection.title = QStringLiteral("manifest.json");
    manifestSection.content = manifestBytes;
    // Jamais de troncature d'octets bruts pour le manifeste (voir ci-dessus) :
    // `truncated`/`omittedBytes` ne reflètent donc que les événements omis
    // pour tenir dans la borne, pas une coupure JSON.
    manifestSection.truncated = eventsOmittedForSizeCap;
    manifestSection.omittedBytes = 0;

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
    for (const auto& section : finalSections) {
        totalBytes += section.content.size();
    }

    report.sections = finalSections;
    report.totalBytes = totalBytes;
    // AUDIT-PIPE-A5 : hash des octets EXACTS qu'un export réel écrirait sur
    // disque (avant compression) -- assembleExportPayload() est la même
    // fonction utilisée par apps/desktop/settings_diagnostics_manager.cpp
    // pour l'export réel. Avant ce correctif, le hash portait sur une
    // concaténation id+content qui ne correspondait à aucun fichier réel.
    report.payloadSha256 = QString::fromLatin1(
        QCryptographicHash::hash(assembleExportPayload(finalSections), QCryptographicHash::Sha256).toHex());
    return result;
}

QByteArray assembleExportPayload(const QList<DiagnosticSection>& sections) {
    QByteArray payload;
    for (const auto& section : sections) {
        payload.append("\n===== ");
        payload.append((section.title.isEmpty() ? section.id : section.title).toUtf8());
        payload.append(" =====\n");
        payload.append(section.content);
        if (!payload.endsWith('\n')) {
            payload.append('\n');
        }
    }
    return payload;
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
