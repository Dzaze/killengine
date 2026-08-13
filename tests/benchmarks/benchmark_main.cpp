// =============================================================================
// KillEngine — Benchmark de performance (Phase 15)
//
// Exécutable autonome qui mesure les performances des composants core sur :
//   - KillEngineTestTarget.exe (scan exact, scan multi-type, unknown snapshot)
//   - gros volumes de candidats synthétiques (stockage file-backed, pagination, tri)
//
// Sortie : lignes clé=valeur sur stdout pour être consommées par le script
// PowerShell scripts/benchmark-performance.ps1.
//
// Usage :
//   KillEngineBenchmark.exe [--large-candidates N] [--json]
// =============================================================================

#include "process/process_handle.h"
#include "memory/memory_reader.h"
#include "scanner/performance_profile.h"
#include "scanner/scan_engine.h"
#include "scanner/scan_types.h"
#include "candidates/candidate_store.h"
#include "snapshot/snapshot_store.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QThread>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

// ---------------------------------------------------------------------------
// Utilitaires de mesure
// ---------------------------------------------------------------------------

class Stopwatch {
public:
    void start() {
        m_begin = std::chrono::steady_clock::now();
    }
    double elapsedMs() const {
        const auto end = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(end - m_begin).count();
    }
private:
    std::chrono::steady_clock::time_point m_begin;
};

struct MetricRow {
    QString name;
    double durationMs{0.0};
    size_t candidates{0};
    size_t bytes{0};
    QString note;
};

static std::vector<MetricRow> g_metrics;

static void recordMetric(const QString& name, double durationMs, size_t candidates, size_t bytes, const QString& note = {}) {
    MetricRow row;
    row.name = name;
    row.durationMs = durationMs;
    row.candidates = candidates;
    row.bytes = bytes;
    row.note = note;
    g_metrics.push_back(row);
}

static void printHuman() {
    std::cout << "\n=== KillEngine Benchmark Results ===\n";
    std::cout << "------------------------------------------------------------\n";
    printf("%-32s %10s %12s %12s %s\n", "scenario", "ms", "candidates", "bytes", "note");
    std::cout << "------------------------------------------------------------\n";
    for (const auto& row : g_metrics) {
        printf("%-32s %10.2f %12zu %12zu %s\n",
               row.name.toUtf8().constData(),
               row.durationMs,
               row.candidates,
               row.bytes,
               row.note.toUtf8().constData());
    }
    std::cout << "------------------------------------------------------------\n";

    // Lignes clé=valeur parsables par PowerShell
    std::cout << "\n# Metrics (key=value):\n";
    for (const auto& row : g_metrics) {
        const QByteArray name = row.name.toUtf8();
        const double seconds = row.durationMs / 1000.0;
        const double perSec = (row.durationMs > 0.0 && row.candidates > 0)
            ? (row.candidates / seconds)
            : 0.0;
        const double bytesPerSec = (row.durationMs > 0.0 && row.bytes > 0)
            ? (row.bytes / seconds)
            : 0.0;
        printf("METRIC\t%s\tduration_ms=%.2f\tcandidates=%zu\tbytes=%zu\tcandidates_per_s=%.0f\tbytes_per_s=%.0f\tnote=%s\n",
               name.constData(),
               row.durationMs,
               row.candidates,
               row.bytes,
               perSec,
               bytesPerSec,
               row.note.toUtf8().constData());
    }
}

static void printJson() {
    QJsonObject root;
    root["machine"] = QCoreApplication::applicationDirPath();

    const auto sys = killcore::detectSystemPerformanceInfo();
    QJsonObject sysObj;
    sysObj["logical_processors"] = static_cast<qint64>(sys.logicalProcessors);
    sysObj["available_memory_bytes"] = static_cast<qint64>(sys.availableMemoryBytes);
    root["system"] = sysObj;

    QJsonArray metrics;
    for (const auto& row : g_metrics) {
        QJsonObject m;
        m["name"] = row.name;
        m["duration_ms"] = row.durationMs;
        m["candidates"] = static_cast<qint64>(row.candidates);
        m["bytes"] = static_cast<qint64>(row.bytes);
        m["note"] = row.note;
        metrics.append(m);
    }
    root["metrics"] = metrics;

    std::cout << QJsonDocument(root).toJson(QJsonDocument::Indented).toStdString();
}

// ---------------------------------------------------------------------------
// Processus cible
// ---------------------------------------------------------------------------

class TestTargetProcess {
public:
    bool start() {
        const QString targetPath = QDir(QCoreApplication::applicationDirPath()).filePath("KillEngineTestTarget.exe");
        m_process.setProgram(targetPath);
        m_process.start();
        if (m_process.waitForStarted(5000)) {
            QThread::msleep(800);
            return true;
        }
        return false;
    }

    ~TestTargetProcess() {
        if (m_process.state() != QProcess::NotRunning) {
            m_process.terminate();
            if (!m_process.waitForFinished(3000)) {
                m_process.kill();
                m_process.waitForFinished(3000);
            }
        }
    }

    uint32_t pid() const {
        return static_cast<uint32_t>(m_process.processId());
    }

private:
    QProcess m_process;
};

// ---------------------------------------------------------------------------
// Helpers de scan
// ---------------------------------------------------------------------------

static killcore::ScanResult scanInt32Value(killcore::ProcessHandle& handle, int32_t expected, killcore::ScanOptions& options) {
    killcore::ScanValue value;
    QString parseError;
    if (!killcore::parseScanValue(QString::number(expected), killcore::ValueType::Int32, &value, &parseError)) {
        std::cerr << "parseScanValue failed: " << parseError.toStdString() << "\n";
        return {};
    }
    killcore::ScanEngine scanner(handle);
    return scanner.exactScan(value, options);
}

// ---------------------------------------------------------------------------
// Benchmarks sur KillEngineTestTarget
// ---------------------------------------------------------------------------

static void benchmarkExactScan(killcore::ProcessHandle& handle) {
    killcore::ScanOptions options;
    options.maxResults = 1000000;

    Stopwatch sw;
    sw.start();
    const auto scan = scanInt32Value(handle, 41250, options);
    const double ms = sw.elapsedMs();

    if (!scan.success) {
        recordMetric("exact_scan_int32", ms, 0, scan.bytesScanned, QString("FAILED: %1").arg(scan.errorMessage));
        return;
    }

    const QString note = scan.partial ? "partial=true" : "ok";
    recordMetric("exact_scan_int32", ms, scan.matchesFound, scan.bytesScanned, note);
}

static void benchmarkMultiTypeScan(killcore::ProcessHandle& handle) {
    QList<killcore::ScanEngine::MultiTypeMatch> variants;
    QString parseError;

    killcore::ScanValue intValue;
    if (!killcore::parseScanValue("41250", killcore::ValueType::Int32, &intValue, &parseError)) return;
    variants.append({intValue, "Int32", false});

    killcore::ScanValue floatValue;
    if (!killcore::parseScanValue("41250", killcore::ValueType::Float32, &floatValue, &parseError)) return;
    variants.append({floatValue, "Float32", false});

    killcore::ScanValue int64Value;
    if (!killcore::parseScanValue("41250", killcore::ValueType::Int64, &int64Value, &parseError)) return;
    variants.append({int64Value, "Int64", true});

    killcore::ScanOptions options;
    options.maxResults = 1000000;
    killcore::ScanEngine scanner(handle);

    Stopwatch sw;
    sw.start();
    const auto scan = scanner.exactScanMultiType(variants, options);
    const double ms = sw.elapsedMs();

    if (!scan.success) {
        recordMetric("multi_type_scan", ms, 0, scan.bytesScanned, QString("FAILED: %1").arg(scan.errorMessage));
        return;
    }

    recordMetric("multi_type_scan", ms, scan.matchesFound, scan.bytesScanned, "3 variants");
}

static void benchmarkUnknownSnapshot(killcore::ProcessHandle& handle) {
    // Capture un snapshot de la mémoire du target (limité à 256 Mo)
    killcore::SnapshotStore snapshot;
    killcore::ScanOptions captureOptions;

    Stopwatch cap;
    cap.start();
    const auto capture = snapshot.capture(handle, 256 * 1024 * 1024, nullptr, captureOptions);
    const double captureMs = cap.elapsedMs();

    if (!capture.success) {
        recordMetric("unknown_capture", captureMs, 0, capture.bytesCaptured, QString("FAILED: %1").arg(capture.errorMessage));
        return;
    }

    recordMetric("unknown_capture", captureMs, capture.regionsCaptured, capture.bytesCaptured,
                 snapshot.usesMappedStorage() ? "mapped" : "temp_file");

    // Comparaison "changed" : teste la capacité à scanner un snapshot
    Stopwatch cmp;
    cmp.start();
    const auto comparison = snapshot.compare(handle, killcore::ValueType::Int32, killcore::NextScanMode::Changed);
    const double cmpMs = cmp.elapsedMs();

    if (!comparison.success) {
        recordMetric("unknown_compare_changed", cmpMs, 0, comparison.checkedBytes,
                     QString("FAILED: %1").arg(comparison.errorMessage));
        return;
    }

    recordMetric("unknown_compare_changed", cmpMs, comparison.matchesFound, comparison.checkedBytes, "ok");
}

// ---------------------------------------------------------------------------
// Benchmark gros volumes de candidats synthétiques
// ---------------------------------------------------------------------------

static void benchmarkSyntheticCandidates(size_t count) {
    // Génère `count` candidats synthétiques, force le stockage file-backed,
    // puis mesure la pagination, le tri par adresse et par confiance.
    std::vector<killcore::Candidate> input;
    input.reserve(count);

    std::srand(42);
    for (size_t i = 0; i < count; ++i) {
        killcore::Candidate c;
        c.address = static_cast<uint64_t>(std::rand()) << 32 | static_cast<uint64_t>(std::rand());
        c.type = killcore::ValueType::Int32;
        c.confidence = static_cast<double>(std::rand()) / RAND_MAX;
        int32_t v = std::rand();
        c.lastValue = QByteArray(reinterpret_cast<const char*>(&v), sizeof(v));
        input.emplace_back(c);
    }

    // Insertion + persistance file-backed
    killcore::CandidateStore store;
    store.setFileBackedThreshold(1); // force file-backed dès que le jeu synthétique dépasse 1 entrée

    Stopwatch ins;
    ins.start();
    QList<killcore::Candidate> qInput;
    qInput.reserve(static_cast<int>(count));
    for (const auto& c : input) {
        qInput.append(c);
    }
    store.replaceCandidates(qInput);
    const double insMs = ins.elapsedMs();

    const bool fileBacked = store.isFileBacked();
    const size_t storageBytes = store.storageBytes();
    const size_t memBytes = store.estimatedMemoryBytes();

    recordMetric("candidates_replace", insMs, store.size(), storageBytes,
                 fileBacked ? QString("file_backed; ram=%1").arg(memBytes) : "in_memory");

    // Pagination : lit 10 pages de 200 candidats
    Stopwatch pg;
    pg.start();
    size_t pageTotal = 0;
    const size_t pageCount = 10;
    const size_t pageSize = 200;
    for (size_t p = 0; p < pageCount; ++p) {
        const auto page = store.page(p, pageSize);
        pageTotal += page.candidates.size();
    }
    const double pgMs = pg.elapsedMs();
    recordMetric("candidates_paginate", pgMs, pageTotal, storageBytes,
                 QString("%1 pages x %2").arg(pageCount).arg(pageSize));

    // Next scan simulé en streaming file-backed : conserve environ la moitié
    // des candidats à partir d'un champ réellement stocké dans le format compact.
    Stopwatch ns;
    ns.start();
    QList<killcore::Candidate> survivors;
    {
        const auto snapshot = store.streamSnapshot();
        QString err;
        killcore::CandidateStore::forEachCandidate(snapshot, [&](const killcore::Candidate& c) {
            if ((c.address & 1ull) == 0) {
                survivors.append(c);
            }
            return true;
        }, &err);
    }
    const double nsMs = ns.elapsedMs();
    recordMetric("candidates_next_scan_filter", nsMs, static_cast<size_t>(survivors.size()), storageBytes,
                 "stream address parity");

    // Tri par adresse
    Stopwatch sa;
    sa.start();
    store.sortByAddress(true);
    const double saMs = sa.elapsedMs();
    recordMetric("candidates_sort_address", saMs, store.size(), storageBytes, "ascending");

    // Tri par confiance
    Stopwatch sc;
    sc.start();
    store.sortByConfidence();
    const double scMs = sc.elapsedMs();
    recordMetric("candidates_sort_confidence", scMs, store.size(), storageBytes, "desc");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("KillEngineBenchmark");

    size_t largeCandidates = 500000;
    bool jsonOutput = false;

    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "--large-candidates" && i + 1 < argc) {
            largeCandidates = static_cast<size_t>(QString::fromLocal8Bit(argv[++i]).toULongLong());
        } else if (arg == "--json") {
            jsonOutput = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "KillEngineBenchmark [--large-candidates N] [--json]\n";
            return 0;
        }
    }

    const auto sys = killcore::detectSystemPerformanceInfo();
    const auto profile = killcore::makePerformanceProfile(killcore::PerformanceMode::Auto, sys);
    std::cout << "KillEngine Benchmark\n";
    std::cout << "  logical processors : " << sys.logicalProcessors << "\n";
    std::cout << "  available memory   : " << (sys.availableMemoryBytes / (1024 * 1024)) << " MB\n";
    std::cout << "  resolved perf mode : " << killcore::performanceModeToString(profile.resolvedMode) << "\n";
    std::cout << "  worker threads     : " << profile.workerThreads << "\n";
    std::cout << "  chunk size         : " << profile.chunkSize << " bytes\n";
    std::cout << "  synthetic count    : " << largeCandidates << "\n";
    std::cout << "\n";

    // --- Benchmarks sur KillEngineTestTarget ---
    TestTargetProcess target;
    if (!target.start()) {
        std::cerr << "ERROR: KillEngineTestTarget.exe did not start.\n";
        std::cerr << "Expected at: " << QDir(QCoreApplication::applicationDirPath())
                     .filePath("KillEngineTestTarget.exe").toStdString() << "\n";
        return 2;
    }

    std::cout << "KillEngineTestTarget started, pid=" << target.pid() << "\n";

    {
        killcore::ProcessHandle handle(target.pid(), killcore::ProcessAccess::ReadOnly);
        if (!handle.isValid()) {
            std::cerr << "ERROR: Could not open KillEngineTestTarget process.\n";
            return 3;
        }

        std::cout << "Running exact scan benchmark...\n";
        benchmarkExactScan(handle);

        std::cout << "Running multi-type scan benchmark...\n";
        benchmarkMultiTypeScan(handle);

        std::cout << "Running unknown snapshot benchmark...\n";
        benchmarkUnknownSnapshot(handle);
    }

    // --- Benchmark gros volumes de candidats synthétiques ---
    std::cout << "Running synthetic candidate benchmark (" << largeCandidates << " entries)...\n";
    benchmarkSyntheticCandidates(largeCandidates);

    if (jsonOutput) {
        printJson();
    } else {
        printHuman();
    }

    return 0;
}
