# Patch P1-2 : backend ApplicationController - findWhatAccessesAsync + scanGroupScan + writeMemoryHex + dumpMemoryRegion
$ErrorActionPreference = 'Stop'

function Read-TextPreserveBom([string]$Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    $text = [System.Text.Encoding]::UTF8.GetString($bytes)
    if ($hasBom -and $text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }
    return @{ Text = $text; HasBom = $hasBom }
}

function Write-TextPreserveBom([string]$Path, [string]$Text, [bool]$HasBom) {
    $encoding = New-Object System.Text.UTF8Encoding($HasBom)
    [System.IO.File]::WriteAllText($Path, $Text, $encoding)
}

function Normalize-Crlf([string]$Text) {
    return $Text.Replace("`r`n", "`n").Replace("`n", "`r`n")
}

# ============ 1. application_controller.h ============
$hPath = 'apps\desktop\application_controller.h'
$h = Read-TextPreserveBom $hPath

if ($h.Text -notmatch 'findWhatAccessesAsync') {
    $h.Text = Normalize-Crlf $h.Text

    # --- Declarations : apres cancelFindWhatWrites ---
    $anchor1 = @'
    Q_INVOKABLE QVariantMap cancelFindWhatWrites();
'@
    $repl1 = @'
    Q_INVOKABLE QVariantMap cancelFindWhatWrites();

    /// Find What Accesses (breakpoint lecture/ecriture) : capture les instructions qui LISSENT l'adresse.
    /// Version non bloquante. Le resultat arrive via findWhatAccessesFinished.
    Q_INVOKABLE QVariantMap findWhatAccessesAsync(const QString& addressHex, const QVariantMap& options);

    /// Scan groupe : cherche N valeurs avec offsets fixes connus (ex: HP/Mana/Stamina voisins).
    /// Entrees : liste {offset, type, value} + options standards Mode Expert.
    Q_INVOKABLE QVariantMap scanGroupScan(const QVariantList& entries, const QVariantMap& options);

    /// Ecriture hexadecimale brute : "48 8B 00" -> bytes exacts a l'adresse. Sauvegarde previous pour rollback.
    Q_INVOKABLE QVariantMap writeMemoryHex(const QString& addressHex, const QString& hexString);

    /// Dump d'une region memoire vers fichier binaire (.bin) sous QStandardPaths::DocumentsLocation/KillEngine/dumps.
    Q_INVOKABLE QVariantMap dumpMemoryRegion(const QString& addressHex, int size, const QString& fileName);
'@
    if (-not $h.Text.Contains($anchor1)) { throw "Ancre 1 introuvable dans $hPath" }
    $h.Text = $h.Text.Replace($anchor1, $repl1)

    # --- Signal : apres findWhatWritesFinished ---
    $anchor2 = @'
    void findWhatWritesFinished(const QVariantMap& result);
'@
    $repl2 = @'
    void findWhatWritesFinished(const QVariantMap& result);

    /// Resultat d'une capture Find What Accesses async (kind = find_what_accesses).
    void findWhatAccessesFinished(const QVariantMap& result);
'@
    if (-not $h.Text.Contains($anchor2)) { throw "Ancre 2 introuvable dans $hPath" }
    $h.Text = $h.Text.Replace($anchor2, $repl2)

    # --- Membres : apres m_findWhatWritesInProgress ---
    $anchor3 = @'
    bool                     m_findWhatWritesInProgress{false};
'@
    $repl3 = @'
    bool                     m_findWhatWritesInProgress{false};
    bool                     m_findWhatAccessesInProgress{false};
'@
    if (-not $h.Text.Contains($anchor3)) { throw "Ancre 3 introuvable dans $hPath" }
    $h.Text = $h.Text.Replace($anchor3, $repl3)

    Write-TextPreserveBom $hPath $h.Text $h.HasBom
    Write-Host "OK: declarations backend ajoutees a $hPath"
} else {
    Write-Host "SKIP: declarations deja presentes dans $hPath"
}

# ============ 2. application_controller.cpp ============
$cPath = 'apps\desktop\application_controller.cpp'
$c = Read-TextPreserveBom $cPath

if ($c.Text -notmatch 'QVariantMap ApplicationController::findWhatAccessesAsync') {
    $c.Text = Normalize-Crlf $c.Text

    # --- Implementations : apres cancelFindWhatWrites ---
    $anchor = @'
QVariantMap ApplicationController::cancelFindWhatWrites() {
    QVariantMap result;
    result["success"] = false;
    if (!m_findWhatWritesInProgress || !m_activeDebugCancellation) {
        result["error"] = "Aucune capture Find What Writes active a annuler.";
        return result;
    }

    m_activeDebugCancellation->cancel();
    result["success"] = true;
    result["error"] = "";
    return result;
}
'@
    $impl = @'
QVariantMap ApplicationController::cancelFindWhatWrites() {
    QVariantMap result;
    result["success"] = false;
    if (!m_findWhatWritesInProgress || !m_activeDebugCancellation) {
        result["error"] = "Aucune capture Find What Writes active a annuler.";
        return result;
    }

    m_activeDebugCancellation->cancel();
    result["success"] = true;
    result["error"] = "";
    return result;
}

QVariantMap ApplicationController::findWhatAccessesAsync(const QString& addressHex, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["started"] = false;
    result["address"] = addressHex;

    if (m_findWhatAccessesInProgress || m_findWhatWritesInProgress) {
        result["error"] = "Une capture debugger est deja en cours.";
        return result;
    }
    if (!m_attached || m_pid <= 0) {
        result["error"] = "Aucun processus attache.";
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    const int sizeBytes = std::clamp(options.value("size", 4).toInt(), 1, 8);
    killcore::BreakpointSize breakpointSize = killcore::BreakpointSize::DWord;
    if (sizeBytes <= 1) {
        breakpointSize = killcore::BreakpointSize::Byte;
    } else if (sizeBytes <= 2) {
        breakpointSize = killcore::BreakpointSize::Word;
    } else if (sizeBytes <= 4) {
        breakpointSize = killcore::BreakpointSize::DWord;
    } else {
        breakpointSize = killcore::BreakpointSize::QWord;
    }

    const int timeoutMs = std::clamp(options.value("timeoutMs", 5000).toInt(), 250, 15000);
    const int maxHitsInt = std::clamp(options.value("maxHits", 10).toInt(), 1, 100);
    const int requestId = m_nextDebugRequestId++;
    const int pid = m_pid;
    const QString requestedAddress = addressHex;
    const QPointer<ApplicationController> self(this);
    auto cancellation = std::make_shared<killcore::CancellationToken>();

    m_findWhatAccessesInProgress = true;
    m_activeDebugCancellation = cancellation;

    KE_LOG_INFO() << "findWhatAccessesAsync(address=0x" << std::hex << address
                  << ", pid=" << std::dec << pid
                  << ", size=" << sizeBytes
                  << ", timeoutMs=" << timeoutMs
                  << ", maxHits=" << maxHitsInt
                  << ", requestId=" << requestId << ")";

    std::thread([self, requestId, pid, address, requestedAddress, breakpointSize, sizeBytes, timeoutMs, maxHitsInt, cancellation]() {
        const auto hits = killcore::findWhatAccesses(
            static_cast<uint32_t>(pid),
            address,
            breakpointSize,
            timeoutMs,
            static_cast<size_t>(maxHitsInt),
            cancellation.get());
        const bool cancelled = cancellation->isCancelled();

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self.data(), [self, requestId, requestedAddress, sizeBytes, timeoutMs, maxHitsInt, hits, cancelled]() {
            if (!self) {
                return;
            }

            QVariantList hitList;
            for (const auto& hit : hits) {
                QVariantMap item;
                item["address"] = QString::number(hit.address, 16).toUpper();
                item["instructionPointer"] = QString::number(hit.instructionPointer, 16).toUpper();
                item["threadId"] = static_cast<qulonglong>(hit.threadId);
                item["valueBefore"] = static_cast<qulonglong>(hit.valueBefore);
                item["valueAfter"] = static_cast<qulonglong>(hit.valueAfter);
                item["module"] = hit.module;
                item["moduleOffset"] = QString::number(hit.moduleOffset, 16).toUpper();
                hitList.append(item);
            }

            QVariantMap finished;
            finished["requestId"] = requestId;
            finished["kind"] = "find_what_accesses";
            finished["success"] = true;
            finished["address"] = requestedAddress;
            finished["hits"] = hitList;
            finished["hitCount"] = hitList.size();
            finished["size"] = sizeBytes;
            finished["timeoutMs"] = timeoutMs;
            finished["maxHits"] = maxHitsInt;
            finished["cancelled"] = cancelled;
            finished["warning"] = "Cette fonction attache KillEngine comme debugger au processus cible pendant la capture.";
            finished["error"] = cancelled
                ? "Capture Find What Accesses annulee."
                : hits.isEmpty()
                ? "Aucun acces capture pendant la fenetre d'observation."
                : QString();

            self->m_findWhatAccessesInProgress = false;
            self->m_activeDebugCancellation.reset();
            emit self->findWhatAccessesFinished(finished);
        }, Qt::QueuedConnection);
    }).detach();

    result["success"] = true;
    result["started"] = true;
    result["requestId"] = requestId;
    result["size"] = sizeBytes;
    result["timeoutMs"] = timeoutMs;
    result["maxHits"] = maxHitsInt;
    result["error"] = "";
    return result;
}

QVariantMap ApplicationController::scanGroupScan(const QVariantList& entriesList, const QVariantMap& optionsMap) {
    QVariantMap result;
    QVariantList matches;
    result["success"] = false;
    result["matches"] = matches;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attache.";
        return result;
    }

    killcore::GroupScanOptions groupOptions;
    const int maxResults = std::clamp(optionsMap.value("maxResults", 1000).toInt(), 1, 10000);
    groupOptions.maxResults = static_cast<size_t>(maxResults);
    groupOptions.maxDistance = std::clamp(optionsMap.value("maxDistance", 256).toInt(), 4, 4096);

    if (entriesList.isEmpty()) {
        result["error"] = "Aucune entree pour le scan groupe.";
        return result;
    }

    for (const auto& entryVariant : entriesList) {
        const QVariantMap entryMap = entryVariant.toMap();
        killcore::GroupScanEntry entry;

        const QString typeText = entryMap.value("type", "Int32").toString();
        if (!killcore::parseValueType(typeText, &entry.type)) {
            result["error"] = QString("Type invalide pour une entree du scan groupe : %1").arg(typeText);
            return result;
        }

        bool offsetOk = false;
        entry.offset = entryMap.value("offset").toLongLong(&offsetOk);
        if (!offsetOk) {
            result["error"] = "Offset invalide pour une entree du scan groupe.";
            return result;
        }

        killcore::ScanValue scanValue;
        QString parseError;
        const QString valueText = entryMap.value("value").toString();
        if (!killcore::parseScanValue(valueText, entry.type, &scanValue, &parseError)) {
            result["error"] = parseError;
            return result;
        }
        entry.value = scanValue.value;

        groupOptions.entries.append(entry);
    }

    killcore::ScanOptions scanOptions = scanOptionsFromSettingsAndExpertOptions(optionsMap);
    scanOptions.writableOnly = optionsMap.value("writableOnly", true).toBool();

    size_t maxEntrySize = 1;
    for (const auto& entry : groupOptions.entries) {
        maxEntrySize = std::max(maxEntrySize, killcore::valueTypeSize(entry.type));
    }

    emit scanStarted();
    emit scanProgress(0);

    QElapsedTimer timer;
    timer.start();
    killcore::MemoryReader reader(m_handle);
    const auto regions = killcore::MemoryMap::snapshot(m_handle);
    int regionsScanned = 0;
    uint64_t bytesScanned = 0;
    bool partial = false;
    QString error;

    const size_t chunkSize = 1024 * 1024;

    for (const auto& region : regions) {
        if (matches.size() >= maxResults) {
            partial = true;
            break;
        }
        if (!regionMatchesScanOptions(region, scanOptions)) {
            continue;
        }

        const uint64_t regionStart = region.baseAddress;
        const uint64_t regionEnd = region.baseAddress + region.size;
        const uint64_t effectiveStart = std::max(regionStart, scanOptions.startAddress);
        const uint64_t effectiveEnd = scanOptions.stopAddress == 0 ? regionEnd : std::min(regionEnd, scanOptions.stopAddress);
        if (effectiveEnd <= effectiveStart) {
            continue;
        }

        ++regionsScanned;
        const uint64_t regionSize = effectiveEnd - effectiveStart;
        const size_t overlap = static_cast<size_t>(groupOptions.maxDistance) + maxEntrySize;
        QByteArray previousTail;
        uint64_t offset = 0;
        while (offset < regionSize && matches.size() < maxResults) {
            const uint64_t remaining = regionSize - offset;
            const size_t toRead = static_cast<size_t>(std::min<uint64_t>(remaining, chunkSize));
            const uint64_t readAddress = effectiveStart + offset;
            const auto read = reader.readChunked(readAddress, toRead, chunkSize);
            if (!read.success && !read.partial) {
                break;
            }

            QByteArray buffer = previousTail + read.data;
            const uint64_t bufferBase = readAddress - static_cast<uint64_t>(previousTail.size());
            bytesScanned += read.bytesRead;
            auto scan = killcore::scanGroupInBuffer(buffer, bufferBase, groupOptions);
            if (!scan.success && !scan.error.isEmpty()) {
                error = scan.error;
                partial = true;
                break;
            }
            if (scan.partial) {
                partial = true;
            }
            for (const auto& matchInfo : scan.matches) {
                if (matches.size() >= maxResults) {
                    partial = true;
                    break;
                }
                QVariantMap match;
                match["address"] = QString::number(matchInfo.address, 16).toUpper();
                match["type"] = killcore::valueTypeToString(matchInfo.type);
                match["confidence"] = matchInfo.confidence;
                match["variantLabel"] = matchInfo.variantLabel;
                match["regionBase"] = QString::number(region.baseAddress, 16).toUpper();
                match["protection"] = killcore::protectionToString(region.protection);
                match["memoryType"] = killcore::memoryTypeToString(region.type);
                match["writable"] = region.writable;
                matches.append(match);
            }

            previousTail = read.data.size() > static_cast<qsizetype>(overlap)
                ? read.data.right(static_cast<qsizetype>(overlap))
                : read.data;
            offset += read.bytesRead;
            if (read.bytesRead == 0 || read.partial) {
                break;
            }
        }

        const int percent = regions.isEmpty()
            ? 100
            : std::clamp((regionsScanned * 100) / std::max(1, static_cast<int>(regions.size())), 0, 99);
        emit scanProgress(percent);
        if (!error.isEmpty()) {
            break;
        }
    }

    emit scanProgress(100);

    result["success"] = error.isEmpty();
    result["partial"] = partial || matches.size() >= maxResults;
    result["matches"] = matches;
    result["matchesFound"] = matches.size();
    result["maxResults"] = maxResults;
    result["regionsScanned"] = regionsScanned;
    result["bytesScanned"] = static_cast<qulonglong>(bytesScanned);
    result["elapsedMs"] = static_cast<int>(timer.elapsed());
    result["entriesCount"] = groupOptions.entries.size();
    result["error"] = error;
    return result;
}

QVariantMap ApplicationController::writeMemoryHex(const QString& addressHex, const QString& hexString) {
    QVariantMap result;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attache.";
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    // Parser la chaine hex : accepte "48 8B 00", "488B00", "48 8b 00"
    QString cleaned = hexString.simplified().remove(' ').remove('\t').remove('\n').remove('\r').remove(',');
    if (cleaned.size() % 2 != 0) {
        result["error"] = "Chaine hexadecimale invalide : nombre impair de caracteres.";
        return result;
    }
    if (cleaned.isEmpty()) {
        result["error"] = "Chaine hexadecimale vide.";
        return result;
    }
    if (cleaned.size() > 4096) {
        result["error"] = "Chaine hexadecimale trop longue (max 2048 octets).";
        return result;
    }
    const QByteArray bytes = QByteArray::fromHex(cleaned.toLatin1());
    if (bytes.isEmpty()) {
        result["error"] = "Chaine hexadecimale invalide.";
        return result;
    }

    killcore::MemoryWriter writer(m_handle);
    const auto writeResult = writer.write(address, bytes, true);

    result["success"] = writeResult.success;
    result["verified"] = writeResult.verified;
    result["address"] = QString::number(address, 16).toUpper();
    result["bytesWritten"] = static_cast<int>(writeResult.bytesWritten);
    result["requestedBytes"] = bytes.size();
    result["protectionChanged"] = writeResult.protectionChanged;
    result["previousHex"] = QString::fromLatin1(writeResult.previousValue.toHex(' ').toUpper());
    result["newHex"] = QString::fromLatin1(bytes.toHex(' ').toUpper());
    result["error"] = writeResult.errorMessage;

    if (writeResult.success) {
        KE_LOG_INFO() << "writeMemoryHex: " << writeResult.bytesWritten << " octets ecrits a 0x" << std::hex << address;
    }
    return result;
}

QVariantMap ApplicationController::dumpMemoryRegion(const QString& addressHex, int size, const QString& fileName) {
    QVariantMap result;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attache.";
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    const int boundedSize = std::clamp(size, 1, 16 * 1024 * 1024);

    QString safeName = fileName.simplified();
    if (safeName.isEmpty()) {
        safeName = QString("dump_%1_%2.bin")
            .arg(QString::number(address, 16).toUpper())
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    }
    safeName.remove('<').remove('>').remove(':').remove('"').remove('/').remove('\\').remove('|').remove('?').remove('*');
    if (!safeName.endsWith(".bin", Qt::CaseInsensitive)) {
        safeName += ".bin";
    }

    const QString dumpDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
        + "/KillEngine/dumps";
    QDir().mkpath(dumpDir);
    const QString filePath = dumpDir + "/" + safeName;

    killcore::MemoryReader reader(m_handle);
    const auto read = reader.readChunked(address, static_cast<size_t>(boundedSize), 1024 * 1024);
    if (!read.success && !read.partial) {
        result["error"] = read.errorMessage.isEmpty() ? QString("Lecture memoire echouee.") : read.errorMessage;
        return result;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        result["error"] = QString("Impossible de creer le fichier : %1").arg(filePath);
        return result;
    }
    const qint64 written = file.write(read.data);
    file.close();
    if (written != read.data.size()) {
        result["error"] = QString("Ecriture fichier incomplete : %1/%2 octets").arg(written).arg(read.data.size());
        return result;
    }

    result["success"] = true;
    result["address"] = QString::number(address, 16).toUpper();
    result["size"] = static_cast<int>(read.bytesRead);
    result["partial"] = read.partial;
    result["filePath"] = filePath;
    result["fileName"] = safeName;
    result["error"] = "";
    KE_LOG_INFO() << "dumpMemoryRegion: " << read.bytesRead << " octets dumps depuis 0x" << std::hex << address
                  << " vers " << filePath.toStdString();
    return result;
}
'@
    if (-not $c.Text.Contains($anchor)) { throw "Ancre implementation introuvable dans $cPath" }
    $c.Text = $c.Text.Replace($anchor, $impl)

    # --- Include QStandardPaths si absent ---
    if ($c.Text -notmatch 'QStandardPaths') {
        $anchorInc = @'
#include <QElapsedTimer>
'@
        $replInc = @'
#include <QElapsedTimer>
#include <QStandardPaths>
'@
        if (-not $c.Text.Contains($anchorInc)) { throw "Ancre include QStandardPaths introuvable dans $cPath" }
        $c.Text = $c.Text.Replace($anchorInc, $replInc)
        Write-Host "OK: include QStandardPaths ajoute"
    }

    Write-TextPreserveBom $cPath $c.Text $c.HasBom
    Write-Host "OK: implementations backend ajoutees a $cPath"
} else {
    Write-Host "SKIP: implementations deja presentes dans $cPath"
}