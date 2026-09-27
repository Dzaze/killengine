#include "diagnostics/diagnostic_report_builder.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>

using namespace killcore;

namespace {

QHash<QString, QString> sampleRoots() {
    QHash<QString, QString> roots;
    roots.insert(QStringLiteral("C:/Users/SentinelUser"), QStringLiteral("<user_home>"));
    roots.insert(QStringLiteral("C:/Temp/SentinelTemp"), QStringLiteral("<temp_dir>"));
    return roots;
}

DiagnosticProvenance sampleProvenance() {
    DiagnosticProvenance p;
    p.engineVersion = QStringLiteral("0.1.0");
    p.buildId = QStringLiteral("");
    p.executableSha256 = QStringLiteral("deadbeef");
    p.uiBundleOrigin = QStringLiteral("packaged");
    p.uiFingerprintStatus = QStringLiteral("matches");
    p.osName = QStringLiteral("Windows");
    p.architecture = QStringLiteral("x86_64");
    return p;
}

} // namespace

TEST(DiagnosticReportBuilder, RedactsKnownRootsCaseInsensitive) {
    const QString input = "Log at c:/users/sentineluser/appdata AND C:/Users/SentinelUser/game.exe";
    const QString redacted = redactKnownRoots(input, sampleRoots());
    EXPECT_FALSE(redacted.contains("SentinelUser", Qt::CaseInsensitive));
    EXPECT_TRUE(redacted.contains("<user_home>"));
}

TEST(DiagnosticReportBuilder, RedactsLongerRootBeforeShorterOne) {
    QHash<QString, QString> roots;
    roots.insert(QStringLiteral("C:/Users/SentinelUser"), QStringLiteral("<user_home>"));
    roots.insert(QStringLiteral("C:/Users/SentinelUser/AppData/Local/Temp"), QStringLiteral("<temp_dir>"));
    const QString input = "C:/Users/SentinelUser/AppData/Local/Temp/scratch.log";
    const QString redacted = redactKnownRoots(input, roots);
    EXPECT_TRUE(redacted.startsWith("<temp_dir>"));
    EXPECT_FALSE(redacted.contains("SentinelUser"));
}

TEST(DiagnosticReportBuilder, RedactionNoOpOnEmptyInput) {
    EXPECT_EQ(redactKnownRoots("", sampleRoots()), "");
}

TEST(BoundNarrativeField, AcceptsTextWithinLimit) {
    const auto result = boundNarrativeField("Steps: click, scan, observe.");
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.text, "Steps: click, scan, observe.");
}

TEST(BoundNarrativeField, RejectsTextOverLimitExplicitlyNeverSilentlyTruncates) {
    const QString huge(DiagnosticReportLimits::kMaxNarrativeBytes + 100, QChar('a'));
    const auto result = boundNarrativeField(huge);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.isEmpty());
    EXPECT_TRUE(result.text.isEmpty());
}

TEST(BuildDiagnosticReport, SecretSentinelAbsentFromNarrativeSectionsAndProvenance) {
    DiagnosticNarrative narrative;
    narrative.steps = "Ouvrir C:/Users/SentinelUser/project puis scanner.";
    narrative.expected = "Rien de special.";
    narrative.observed = "Crash observe.";

    QList<DiagnosticRawSection> sections;
    DiagnosticRawSection logSection;
    logSection.id = "log";
    logSection.title = "killengine.log";
    logSection.content = "[INFO] loaded from C:/Users/SentinelUser/AppData/model.gguf";
    sections.append(logSection);

    auto provenance = sampleProvenance();
    provenance.uiAssetFiles.append("C:/Users/SentinelUser/repo/ui/dist/assets/index-ABC.js");

    const auto result = buildDiagnosticReport(
        QUuid::createUuid().toString(QUuid::WithoutBraces), 1000,
        narrative, provenance, sections, {}, sampleRoots());

    ASSERT_TRUE(result.ok);
    for (const auto& section : result.report.sections) {
        EXPECT_FALSE(QString::fromUtf8(section.content).contains("SentinelUser", Qt::CaseInsensitive))
            << "section " << section.id.toStdString() << " leaked the sentinel";
    }
    for (const auto& file : result.report.provenance.uiAssetFiles) {
        EXPECT_FALSE(file.contains("SentinelUser", Qt::CaseInsensitive));
    }
}

TEST(BuildDiagnosticReport, UsefulDataAndCorrelatedPseudonymsSurvive) {
    DiagnosticNarrative narrative;
    narrative.steps = "Attacher C:/Users/SentinelUser/target.exe";
    QList<DiagnosticRawSection> sections;
    DiagnosticRawSection section;
    section.id = "log";
    section.title = "killengine.log";
    section.content = "attach ok C:/Users/SentinelUser/target.exe pid=1234";
    sections.append(section);

    const auto result = buildDiagnosticReport(
        "r1", 1000, narrative, sampleProvenance(), sections, {}, sampleRoots());

    ASSERT_TRUE(result.ok);
    const QString logText = QString::fromUtf8(result.report.sections.last().content);
    EXPECT_TRUE(logText.contains("pid=1234"));
    EXPECT_TRUE(logText.contains("<user_home>"));
    EXPECT_TRUE(result.report.narrative.steps.contains("<user_home>"));
}

TEST(BuildDiagnosticReport, ManifestIsAlwaysFirstSection) {
    const auto result = buildDiagnosticReport(
        "r1", 1000, DiagnosticNarrative{}, sampleProvenance(), {}, {}, {});
    ASSERT_TRUE(result.ok);
    ASSERT_FALSE(result.report.sections.isEmpty());
    EXPECT_EQ(result.report.sections.first().id, "manifest");
}

TEST(BuildDiagnosticReport, SectionOverMaxBytesIsTruncatedHonestlyNotRefused) {
    DiagnosticRawSection section;
    section.id = "big";
    section.title = "big.log";
    section.content = QString(DiagnosticReportLimits::kMaxSectionBytes + 1000, QChar('x'));
    const auto result = buildDiagnosticReport(
        "r1", 1000, DiagnosticNarrative{}, sampleProvenance(), {section}, {}, {});
    ASSERT_TRUE(result.ok);
    const auto& big = result.report.sections.last();
    EXPECT_EQ(big.id, "big");
    EXPECT_TRUE(big.truncated);
    EXPECT_GT(big.omittedBytes, 0);
    EXPECT_LE(big.content.size(), DiagnosticReportLimits::kMaxSectionBytes);
}

TEST(BuildDiagnosticReport, SectionCountCappedAt32IncludingManifest) {
    QList<DiagnosticRawSection> sections;
    for (int i = 0; i < 40; ++i) {
        DiagnosticRawSection section;
        section.id = QStringLiteral("s%1").arg(i);
        section.title = section.id;
        section.content = "x";
        sections.append(section);
    }
    const auto result = buildDiagnosticReport(
        "r1", 1000, DiagnosticNarrative{}, sampleProvenance(), sections, {}, {});
    ASSERT_TRUE(result.ok);
    EXPECT_LE(result.report.sections.size(), DiagnosticReportLimits::kMaxSections);
}

TEST(BuildDiagnosticReport, TotalBytesNeverExceedsCap) {
    QList<DiagnosticRawSection> sections;
    for (int i = 0; i < 10; ++i) {
        DiagnosticRawSection section;
        section.id = QStringLiteral("s%1").arg(i);
        section.title = section.id;
        // Chaque section reste sous la borne individuelle, mais 10 x 250 Kio
        // dépasse le budget total de 2 Mio.
        section.content = QString(250 * 1024, QChar('y'));
        sections.append(section);
    }
    const auto result = buildDiagnosticReport(
        "r1", 1000, DiagnosticNarrative{}, sampleProvenance(), sections, {}, {});
    ASSERT_TRUE(result.ok);
    EXPECT_LE(result.report.totalBytes, DiagnosticReportLimits::kMaxTotalBytes);
}

TEST(BuildDiagnosticReport, EventsAreSortedByTimestampAndRedacted) {
    QList<DiagnosticEvent> events;
    DiagnosticEvent e1;
    e1.source = "activity"; e1.kind = "scan_exact"; e1.state = "completed"; e1.timestampMs = 2000;
    e1.summary = "done at C:/Users/SentinelUser";
    DiagnosticEvent e2;
    e2.source = "actionLog"; e2.kind = "watch"; e2.state = "info"; e2.timestampMs = 1000;
    e2.summary = "watched address";
    events.append(e1);
    events.append(e2);

    const auto result = buildDiagnosticReport(
        "r1", 1000, DiagnosticNarrative{}, sampleProvenance(), {}, events, sampleRoots());
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.report.events.size(), 2);
    EXPECT_EQ(result.report.events.first().timestampMs, 1000);
    EXPECT_EQ(result.report.events.last().timestampMs, 2000);
    EXPECT_FALSE(result.report.events.last().summary.contains("SentinelUser"));
}

TEST(BuildDiagnosticReport, PayloadHashStableForSameContentDifferentForDifferentContent) {
    const auto resultA = buildDiagnosticReport(
        "r1", 1000, DiagnosticNarrative{}, sampleProvenance(), {}, {}, {});
    const auto resultB = buildDiagnosticReport(
        "r2", 1000, DiagnosticNarrative{}, sampleProvenance(), {}, {}, {});
    // Contenu identique hormis reportId (qui n'entre pas dans le hash de
    // section content -- seul le manifeste texte differe legerement via
    // reportId, donc les hashes DOIVENT differer ici).
    EXPECT_NE(resultA.report.payloadSha256, resultB.report.payloadSha256);

    const auto resultA2 = buildDiagnosticReport(
        "r1", 1000, DiagnosticNarrative{}, sampleProvenance(), {}, {}, {});
    EXPECT_EQ(resultA.report.payloadSha256, resultA2.report.payloadSha256);
}

// ---------------------------------------------------------------------------
// readSectionPage
// ---------------------------------------------------------------------------

DiagnosticSection makeSection(const QByteArray& content) {
    DiagnosticSection section;
    section.id = "s";
    section.content = content;
    return section;
}

TEST(ReadSectionPage, ReturnsFullContentWhenUnderLimit) {
    const auto section = makeSection("hello world");
    const auto page = readSectionPage(section, 0, 1000);
    EXPECT_TRUE(page.ok);
    EXPECT_EQ(page.data, QByteArray("hello world"));
    EXPECT_EQ(page.nextOffset, -1);
}

TEST(ReadSectionPage, PaginatesAcrossMultipleCalls) {
    const QByteArray content(50, 'z');
    const auto section = makeSection(content);
    const auto page1 = readSectionPage(section, 0, 20);
    EXPECT_EQ(page1.data.size(), 20);
    EXPECT_EQ(page1.nextOffset, 20);
    const auto page2 = readSectionPage(section, page1.nextOffset, 20);
    EXPECT_EQ(page2.data.size(), 20);
    EXPECT_EQ(page2.nextOffset, 40);
    const auto page3 = readSectionPage(section, page2.nextOffset, 20);
    EXPECT_EQ(page3.data.size(), 10);
    EXPECT_EQ(page3.nextOffset, -1);
}

TEST(ReadSectionPage, NeverSplitsAMultiByteUtf8Character) {
    // "café" -- le é est encode sur 2 octets en UTF-8 (0xC3 0xA9).
    const QByteArray content = QString("caf\u00e9 more text after").toUtf8();
    // Coupe volontairement en plein milieu du caractere multi-octet (juste
    // apres le premier octet de é, à l'offset 4 = 'c','a','f',0xC3,[0xA9]).
    const auto section = makeSection(content);
    const auto page = readSectionPage(section, 4, 1);
    ASSERT_TRUE(page.ok);
    // Le decodage du fragment retourne ne doit jamais planter/produire un
    // caractere de remplacement en tete a cause d'une coupure invalide.
    const QString decoded = QString::fromUtf8(page.data);
    EXPECT_FALSE(decoded.isEmpty());
    EXPECT_FALSE(decoded.contains(QChar(0xFFFD)));
}

TEST(ReadSectionPage, RejectsOffsetOutOfBounds) {
    const auto section = makeSection("short");
    const auto page = readSectionPage(section, 100, 10);
    EXPECT_FALSE(page.ok);
}

TEST(ReadSectionPage, OffsetAtExactEndReturnsEmptyPageNotError) {
    const auto section = makeSection("abc");
    const auto page = readSectionPage(section, 3, 10);
    EXPECT_TRUE(page.ok);
    EXPECT_TRUE(page.data.isEmpty());
    EXPECT_EQ(page.nextOffset, -1);
}

TEST(ReadSectionPage, LimitClampedToMaxPageBytes) {
    const QByteArray content(DiagnosticReportLimits::kMaxPageBytes + 5000, 'a');
    const auto section = makeSection(content);
    const auto page = readSectionPage(section, 0, DiagnosticReportLimits::kMaxPageBytes + 5000);
    EXPECT_LE(page.data.size(), DiagnosticReportLimits::kMaxPageBytes);
}

// ---------------------------------------------------------------------------
// hashFileBounded
// ---------------------------------------------------------------------------

TEST(HashFileBounded, ComputesRealSha256ForARealFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("sample.bin");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("hello diagnostic world");
    file.close();

    const QString hashA = hashFileBounded(path);
    EXPECT_FALSE(hashA.isEmpty());
    EXPECT_EQ(hashA.size(), 64); // hex sha256

    // Meme contenu -> meme hash (stabilite), deterministe et reproductible.
    const QString hashB = hashFileBounded(path);
    EXPECT_EQ(hashA, hashB);
}

TEST(HashFileBounded, EmptyForMissingFile) {
    EXPECT_TRUE(hashFileBounded("C:/definitely/not/a/real/path/xyz.bin").isEmpty());
}

TEST(HashFileBounded, EmptyForEmptyPath) {
    EXPECT_TRUE(hashFileBounded("").isEmpty());
}
