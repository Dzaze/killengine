#include "code_patch_manager.h"

#include "inject/function_hook.h"
#include "logging/logger.h"
#include "memory/memory_reader.h"
#include "patch/aob_scanner.h"
#include "patch/code_patch.h"
#include "patch/instruction_patch_suggester.h"
#include "process/process_enumerator.h"

#include <QStringList>
#include <QVariantList>

#include <algorithm>
#include <utility>

namespace killengine {
namespace {

QVariantMap aobPatternQualityToVariantMap(const killcore::AobPatternQuality& quality) {
    QVariantMap item;
    item["score"] = quality.score;
    item["level"] = quality.level;
    item["warning"] = quality.warning;
    item["patternBytes"] = quality.patternBytes;
    item["fixedBytes"] = quality.fixedBytes;
    item["wildcardBytes"] = quality.wildcardBytes;
    item["uniqueFixedBytes"] = quality.uniqueFixedBytes;
    item["fixedRatio"] = quality.fixedRatio;
    item["trainerSafe"] = quality.trainerSafe;
    return item;
}

QString codeReadProtectionHint(uint32_t errorCode) {
    if (errorCode == 299) {
        return QStringLiteral(
            "Le code de ce module semble protégé contre la lecture externe "
            "(fréquent sur les exécutables Microsoft Store/UWP signés). "
            "Génération de signature/patch impossible sur cette instruction — "
            "essaie Freeze ou une écriture groupée sur la donnée plutôt qu'un "
            "patch du code.");
    }
    return QString();
}

bool parseHexAddress(const QString& addressHex, uint64_t* address) {
    if (!address) {
        return false;
    }

    QString normalized = addressHex.trimmed();
    if (normalized.startsWith("0x", Qt::CaseInsensitive)) {
        normalized = normalized.mid(2);
    }

    bool ok = false;
    const uint64_t parsed = normalized.toULongLong(&ok, 16);
    if (!ok || parsed == 0) {
        return false;
    }

    *address = parsed;
    return true;
}

} // namespace

CodePatchManager::CodePatchManager(
    const killcore::ProcessHandle& handle,
    TelemetryCallback telemetry,
    IsAttachedCallback isAttached,
    PidCallback pid)
    : m_handle(handle)
    , m_appendScanTelemetry(std::move(telemetry))
    , m_isAttached(std::move(isAttached))
    , m_pid(std::move(pid)) {
}

void CodePatchManager::clearSessionState() {
    m_activeCodePatches.clear();
    m_activeFunctionHooks.clear();
    m_lastAutoAsmResult.reset();
}

bool CodePatchManager::isCodePatchActive(uint64_t address) const {
    return m_activeCodePatches.contains(address);
}
QVariantMap CodePatchManager::scanAobPattern(const QString& patternText, const QVariantMap& optionsMap) {
    QVariantMap result;
    result["success"] = false;
    result["pattern"] = patternText;

    if (!m_isAttached() || m_pid() <= 0) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    const auto pattern = killcore::parseAobPattern(patternText);
    if (!pattern.isValid()) {
        result["error"] = pattern.error;
        return result;
    }

    killcore::AobScanOptions options;
    options.executableOnly = optionsMap.value("executableOnly", true).toBool();
    options.writableOnly = optionsMap.value("writableOnly", false).toBool();
    options.imageOnly = optionsMap.value("imageOnly", true).toBool();
    options.maxResults = std::clamp(optionsMap.value("maxResults", 200).toInt(), 1, 10000);

    const QString startText = optionsMap.value("startAddress").toString().trimmed();
    const QString stopText = optionsMap.value("stopAddress").toString().trimmed();
    if (!startText.isEmpty() && !parseHexAddress(startText, &options.startAddress)) {
        result["error"] = "Adresse de début invalide.";
        return result;
    }
    if (!stopText.isEmpty() && !parseHexAddress(stopText, &options.stopAddress)) {
        result["error"] = "Adresse de fin invalide.";
        return result;
    }
    if (options.startAddress > 0 && options.stopAddress > 0 && options.startAddress >= options.stopAddress) {
        result["error"] = "La plage AOB est invalide.";
        return result;
    }

    KE_LOG_INFO() << "scanAobPattern(patternBytes=" << pattern.bytes.size()
                  << ", executableOnly=" << options.executableOnly
                  << ", imageOnly=" << options.imageOnly
                  << ", maxResults=" << options.maxResults << ")";

    const auto scan = killcore::scanAobPattern(m_handle, pattern, options);
    QVariantList matches;
    const auto modules = killcore::ProcessEnumerator::enumerateModules(static_cast<uint32_t>(m_pid()));
    for (const auto& match : scan.matches) {
        QVariantMap item;
        item["address"] = QString::number(match.address, 16).toUpper();
        item["regionBase"] = QString::number(match.regionBase, 16).toUpper();
        item["regionSize"] = static_cast<qulonglong>(match.regionSize);
        item["protection"] = killcore::protectionToString(match.protection);
        item["memoryType"] = killcore::memoryTypeToString(match.memoryType);
        for (const auto& module : modules) {
            if (match.address >= module.baseAddress && match.address < module.baseAddress + module.size) {
                item["module"] = module.name;
                item["moduleOffset"] = QString::number(match.address - module.baseAddress, 16).toUpper();
                break;
            }
        }
        matches.append(item);
    }

    result["success"] = scan.success;
    result["partial"] = scan.partial;
    result["error"] = scan.error;
    result["bytesScanned"] = static_cast<qulonglong>(scan.bytesScanned);
    result["regionsScanned"] = scan.regionsScanned;
    result["matchesFound"] = scan.matchesFound;
    result["matches"] = matches;
    result["patternBytes"] = static_cast<int>(pattern.bytes.size());
    result["executableOnly"] = options.executableOnly;
    result["imageOnly"] = options.imageOnly;
    const auto quality = killcore::evaluateAobPatternQuality(pattern);
    result["signatureQuality"] = aobPatternQualityToVariantMap(quality);
    result["signatureRisk"] = quality.level;
    result["signatureWarning"] = quality.warning;
    m_appendScanTelemetry("aob_scan", result);
    return result;
}

QVariantMap CodePatchManager::generateAobSignature(const QString& addressHex, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["address"] = addressHex;

    if (!m_isAttached() || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    const int beforeBytes = std::clamp(options.value("beforeBytes", 0).toInt(), 0, 32);
    const int length = std::clamp(options.value("length", 24).toInt(), 4, 64);
    const uint64_t startAddress = address > static_cast<uint64_t>(beforeBytes)
        ? address - static_cast<uint64_t>(beforeBytes)
        : address;

    killcore::MemoryReader reader(m_handle);
    const auto read = reader.readChunked(startAddress, static_cast<size_t>(length), 4096);
    if (!read.success && read.bytesRead == 0) {
        const QString hint = codeReadProtectionHint(read.errorCode);
        if (!hint.isEmpty()) {
            result["error"] = hint;
            result["codeReadProtected"] = true;
        } else {
            result["error"] = read.errorMessage.isEmpty() ? QString("Lecture des octets d'instruction impossible.") : read.errorMessage;
        }
        return result;
    }

    QVariantMap moduleInfo;
    const auto modules = killcore::ProcessEnumerator::enumerateModules(static_cast<uint32_t>(m_pid()));
    for (const auto& module : modules) {
        if (startAddress >= module.baseAddress && startAddress < module.baseAddress + module.size) {
            moduleInfo["module"] = module.name;
            moduleInfo["moduleOffset"] = QString::number(startAddress - module.baseAddress, 16).toUpper();
            break;
        }
    }

    result["success"] = true;
    result["partial"] = read.partial;
    result["startAddress"] = QString::number(startAddress, 16).toUpper();
    result["instructionAddress"] = QString::number(address, 16).toUpper();
    result["bytesRead"] = static_cast<int>(read.bytesRead);
    result["requestedBytes"] = static_cast<int>(read.requestedBytes);
    result["hex"] = QString::fromLatin1(read.data.toHex(' ').toUpper());
    result["pattern"] = killcore::bytesToAobPattern(read.data);
    result["patternBytes"] = static_cast<int>(read.data.size());
    const auto rawPattern = killcore::parseAobPattern(result.value("pattern").toString());
    const auto quality = killcore::evaluateAobPatternQuality(rawPattern);
    result["signatureQuality"] = aobPatternQualityToVariantMap(quality);
    result["signatureRisk"] = quality.level;
    result["module"] = moduleInfo.value("module");
    result["moduleOffset"] = moduleInfo.value("moduleOffset");
    result["error"] = read.errorMessage;
    result["warning"] = QString("Signature exacte brute. %1 Elle peut nécessiter des wildcards si l'instruction contient offsets/relocations.").arg(quality.warning);
    m_appendScanTelemetry("aob_signature", result);
    return result;
}

QVariantMap CodePatchManager::applyCodePatch(const QString& addressHex, const QString& bytesText, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["address"] = addressHex;
    result["patchBytes"] = bytesText;

    if (!m_isAttached() || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = "Adresse invalide.";
        return result;
    }
    if (m_activeCodePatches.contains(address)) {
        result["error"] = "Un patch actif existe déjà à cette adresse. Restaure-le avant d'en appliquer un autre.";
        result["active"] = true;
        return result;
    }

    const auto patch = killcore::parsePatchBytes(bytesText);
    if (!patch.isValid()) {
        result["error"] = patch.error;
        return result;
    }
    if (patch.bytes.size() > 64) {
        result["error"] = "Patch trop long pour cette version expérimentale (64 bytes maximum).";
        return result;
    }

    const bool verify = options.value("verify", true).toBool();
    const auto applied = killcore::applyCodePatch(m_handle, address, patch.bytes, verify);
    result["success"] = applied.success;
    result["verified"] = applied.verified;
    result["protectionChanged"] = applied.protectionChanged;
    result["bytesWritten"] = static_cast<int>(applied.bytesWritten);
    result["originalBytes"] = QString::fromLatin1(applied.previousBytes.toHex(' ').toUpper());
    result["writtenBytes"] = QString::fromLatin1(patch.bytes.toHex(' ').toUpper());
    result["error"] = applied.error;

    if (applied.success) {
        ActiveCodePatch active;
        active.address = address;
        active.originalBytes = applied.previousBytes;
        active.patchBytes = patch.bytes;
        m_activeCodePatches.insert(address, active);
        result["active"] = true;
        KE_LOG_WARN() << "Code patch applied at 0x" << std::hex << address
                      << " bytes=" << std::dec << patch.bytes.size()
                      << " protectionChanged=" << applied.protectionChanged;
    }

    return result;
}

QVariantMap CodePatchManager::suggestCodePatches(const QString& addressHex, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["address"] = addressHex;

    if (!m_isAttached() || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    const int maxBytes = std::clamp(options.value("maxBytes", 16).toInt(), 8, 64);
    killcore::MemoryReader reader(m_handle);
    const auto read = reader.readChunked(address, static_cast<size_t>(maxBytes), 4096);
    if (!read.success && read.bytesRead == 0) {
        const QString hint = codeReadProtectionHint(read.errorCode);
        if (!hint.isEmpty()) {
            result["error"] = hint;
            result["codeReadProtected"] = true;
        } else {
            result["error"] = read.errorMessage.isEmpty() ? QString("Lecture instruction impossible.") : read.errorMessage;
        }
        return result;
    }

    const auto instruction = killcore::decodeX64InstructionLength(read.data);
    result["instructionSuccess"] = instruction.success;
    result["instructionLength"] = instruction.length;
    result["mnemonicHint"] = instruction.mnemonicHint;
    result["disassembly"] = instruction.disassembly;
    result["decoder"] = instruction.decoder;
    result["category"] = instruction.category;
    result["stableAobPattern"] = instruction.stableAobPattern;
    const auto stablePattern = killcore::parseAobPattern(instruction.stableAobPattern);
    const auto stableQuality = killcore::evaluateAobPatternQuality(stablePattern);
    result["signatureQuality"] = aobPatternQualityToVariantMap(stableQuality);
    result["signatureRisk"] = stableQuality.level;
    result["bytesRead"] = static_cast<int>(read.bytesRead);
    result["bytes"] = QString::fromLatin1(read.data.left(instruction.length > 0 ? instruction.length : read.data.size()).toHex(' ').toUpper());
    // Registre+deplacement de l'operande memoire destination (vide si non
    // exploitable) : permet au frontend de proposer "Forcer une valeur (hook)"
    // meme quand l'instruction n'a pas d'immediat a substituer directement
    // (source registre) — voir ApplicationController::forceWriteInstructionValue.
    result["memBaseRegister"] = instruction.memBaseRegister;
    result["memDisplacement"] = static_cast<qlonglong>(instruction.memDisplacement);

    if (!instruction.success) {
        result["error"] = instruction.error;
        return result;
    }

    QVariantList suggestions;
    const auto patchSuggestions = killcore::suggestInstructionPatches(instruction);
    for (const auto& suggestion : patchSuggestions) {
        QVariantMap item;
        item["label"] = suggestion.label;
        item["bytesText"] = suggestion.bytesText;
        item["description"] = suggestion.description;
        item["category"] = suggestion.category;
        item["riskLevel"] = suggestion.riskLevel;
        item["risky"] = suggestion.risky;
        item["needsValueInput"] = suggestion.needsValueInput;
        item["valueOffset"] = suggestion.valueOffset;
        item["valueSize"] = suggestion.valueSize;
        suggestions.append(item);
    }

    result["success"] = true;
    result["suggestions"] = suggestions;
    result["warning"] = "Décodage x64 ciblé et expérimental. Vérifie toujours les bytes avant d'appliquer.";
    m_appendScanTelemetry("aob_patch_suggest", result);
    return result;
}

QVariantMap CodePatchManager::restoreCodePatch(const QString& addressHex) {
    QVariantMap result;
    result["success"] = false;
    result["address"] = addressHex;

    if (!m_isAttached() || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    const auto it = m_activeCodePatches.constFind(address);
    if (it == m_activeCodePatches.constEnd()) {
        result["error"] = "Aucun patch actif connu à cette adresse.";
        result["active"] = false;
        return result;
    }

    const auto restored = killcore::restoreCodePatch(m_handle, address, it->originalBytes, true);
    result["success"] = restored.success;
    result["verified"] = restored.verified;
    result["protectionChanged"] = restored.protectionChanged;
    result["bytesWritten"] = static_cast<int>(restored.bytesWritten);
    result["restoredBytes"] = QString::fromLatin1(it->originalBytes.toHex(' ').toUpper());
    result["error"] = restored.error;

    if (restored.success) {
        m_activeCodePatches.remove(address);
        result["active"] = false;
        KE_LOG_WARN() << "Code patch restored at 0x" << std::hex << address;
    } else {
        result["active"] = true;
    }

    return result;
}

QVariantMap CodePatchManager::installFunctionHook(const QString& targetAddressHex, const QString& hookAddressHex) {
    QVariantMap result;
    result["success"] = false;
    result["targetAddress"] = targetAddressHex;
    result["hookAddress"] = hookAddressHex;

    if (!m_isAttached() || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    uint64_t targetAddress = 0;
    uint64_t hookAddress = 0;
    if (!parseHexAddress(targetAddressHex, &targetAddress) || !parseHexAddress(hookAddressHex, &hookAddress)) {
        result["error"] = "Adresse invalide.";
        return result;
    }
    if (m_activeFunctionHooks.contains(targetAddress)) {
        result["error"] = "Un hook actif existe déjà sur cette adresse. Retire-le avant d'en installer un autre.";
        result["active"] = true;
        return result;
    }

    const auto installed = killcore::installInlineHook(m_handle, targetAddress, hookAddress);
    result["success"] = installed.success;
    result["error"] = installed.error;
    result["trampolineAddress"] = QString::number(installed.trampolineAddress, 16).toUpper();
    result["originalBytes"] = QString::fromLatin1(installed.originalBytes.toHex(' ').toUpper());

    if (installed.success) {
        ActiveFunctionHook active;
        active.targetAddress = targetAddress;
        active.hookFunctionAddress = hookAddress;
        active.trampolineAddress = installed.trampolineAddress;
        active.originalBytes = installed.originalBytes;
        m_activeFunctionHooks.insert(targetAddress, active);
        result["active"] = true;
        KE_LOG_WARN() << "Function hook installed at 0x" << std::hex << targetAddress
                      << " -> 0x" << hookAddress;
    }

    m_appendScanTelemetry("function_hook_install", result);
    return result;
}

QVariantMap CodePatchManager::removeFunctionHook(const QString& targetAddressHex) {
    QVariantMap result;
    result["success"] = false;
    result["targetAddress"] = targetAddressHex;

    if (!m_isAttached() || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    uint64_t targetAddress = 0;
    if (!parseHexAddress(targetAddressHex, &targetAddress)) {
        result["error"] = "Adresse invalide.";
        return result;
    }

    const auto it = m_activeFunctionHooks.constFind(targetAddress);
    if (it == m_activeFunctionHooks.constEnd()) {
        result["error"] = "Aucun hook actif connu à cette adresse.";
        result["active"] = false;
        return result;
    }

    const auto removed = killcore::removeInlineHook(m_handle, targetAddress, it->originalBytes);
    result["success"] = removed.success;
    result["error"] = removed.error;
    result["restoredBytes"] = QString::fromLatin1(it->originalBytes.toHex(' ').toUpper());

    if (removed.success) {
        m_activeFunctionHooks.remove(targetAddress);
        result["active"] = false;
        KE_LOG_WARN() << "Function hook removed at 0x" << std::hex << targetAddress;
    } else {
        result["active"] = true;
    }

    m_appendScanTelemetry("function_hook_remove", result);
    return result;
}

namespace {

QVariantMap autoAsmScriptToPreviewVariant(const killcore::AutoAsmScript& script) {
    QVariantMap preview;
    preview["parseSuccess"] = script.success;
    preview["parseError"] = script.error;
    preview["parseErrorLine"] = script.errorLine;

    QVariantList instructions;
    for (const auto& instruction : script.instructions) {
        instructions.append(QVariantMap{
            {"line", instruction.line},
            {"target", instruction.target},
        });
    }
    preview["instructionCount"] = script.instructions.size();
    preview["instructions"] = instructions;

    QVariantList allocations;
    for (const auto& allocation : script.allocations) {
        allocations.append(QVariantMap{{"name", allocation.name}, {"size", static_cast<qulonglong>(allocation.size)}});
    }
    preview["allocations"] = allocations;

    QVariantList labels;
    for (const auto& label : script.labels) {
        labels.append(label.name);
    }
    preview["labels"] = labels;

    return preview;
}

} // namespace

QVariantMap CodePatchManager::parseAutoAssemblerScript(const QString& scriptText) const {
    QVariantMap result;
    const auto script = killcore::parseAutoAsmScript(scriptText);
    result = autoAsmScriptToPreviewVariant(script);
    result["success"] = script.success;

    if (script.success) {
        // Resout les modules references par "module"+offset: si un processus
        // est attache, pour que l'apercu montre les vraies adresses/bytes de
        // ces blocs plutot que de les rejeter faute de contexte. Les labels
        // lies a un alloc() (ex: "newmem:") ne peuvent pas etre resolus ici
        // (l'allocation reelle n'a lieu qu'a l'execution) — ils restent
        // affiches a l'offset 0 par defaut, ce qui reste suffisant pour
        // verifier le contenu compile avant d'executer pour de vrai.
        killcore::AutoAsmCompileContext context;
        if (m_isAttached() && m_pid() != 0) {
            const auto modules = killcore::ProcessEnumerator::enumerateModules(static_cast<uint32_t>(m_pid()));
            for (const auto& instr : script.instructions) {
                if (instr.type != killcore::AutoAsmInstructionType::ModuleLabel) continue;
                if (context.moduleBaseAddresses.contains(instr.target)) continue;
                for (const auto& module : modules) {
                    if (module.name.compare(instr.target, Qt::CaseInsensitive) == 0) {
                        context.moduleBaseAddresses.insert(instr.target, module.baseAddress);
                        break;
                    }
                }
            }
        }

        const auto compiled = killcore::compileAutoAsmScript(script, 0, context);
        result["compileSuccess"] = compiled.success;
        result["compileError"] = compiled.error;
        result["compileErrorLine"] = compiled.errorLine;

        QStringList regionSummaries;
        for (const auto& region : compiled.regions) {
            regionSummaries.append(QStringLiteral("0x%1: %2")
                .arg(QString::number(region.baseAddress, 16).toUpper(),
                     QString::fromLatin1(region.code.toHex(' ').toUpper())));
        }
        result["compiledBytes"] = regionSummaries.join('\n');
        result["compiledRegionCount"] = compiled.regions.size();
    }
    return result;
}

QVariantMap CodePatchManager::executeAutoAssemblerScript(const QString& scriptText) {
    QVariantMap result;
    result["success"] = false;

    if (!m_isAttached() || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    const auto script = killcore::parseAutoAsmScript(scriptText);
    if (!script.success) {
        result["error"] = script.error;
        result["errorLine"] = script.errorLine;
        return result;
    }
    if (m_lastAutoAsmResult.has_value()) {
        result["error"] = "Un script auto-assembler est déjà actif. Restaure-le avant d'en exécuter un autre.";
        result["active"] = true;
        return result;
    }

    const auto executed = killcore::executeAutoAsmScript(m_handle, script);
    result["success"] = executed.success;
    result["error"] = executed.error;
    result["errorLine"] = executed.errorLine;

    QVariantList patchedRegions;
    for (const auto& region : executed.patchedRegions) {
        patchedRegions.append(QVariantMap{
            {"address", QString::number(region.address, 16).toUpper()},
            {"size", region.size},
            {"wasAllocated", region.wasAllocated},
        });
    }
    result["patchedRegions"] = patchedRegions;
    if (!executed.patchedRegions.isEmpty()) {
        // Alias pratique vers la premiere region, pour un script simple a une
        // seule region (le cas le plus courant) sans obliger l'appelant a
        // depouiller patchedRegions.
        result["patchAddress"] = QString::number(executed.patchedRegions.first().address, 16).toUpper();
        result["patchSize"] = executed.patchedRegions.first().size;
    }

    if (executed.success) {
        m_lastAutoAsmResult = executed;
        result["active"] = true;
        KE_LOG_WARN() << "Auto-assembler script executed, " << executed.patchedRegions.size() << " region(s) written";
    }

    m_appendScanTelemetry("auto_assembler_execute", result);
    return result;
}

QVariantMap CodePatchManager::restoreAutoAssemblerScript() {
    QVariantMap result;
    result["success"] = false;

    if (!m_isAttached() || !m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }
    if (!m_lastAutoAsmResult.has_value()) {
        result["error"] = "Aucun script auto-assembler actif à restaurer.";
        result["active"] = false;
        return result;
    }

    const bool restored = killcore::restoreAutoAsmScript(m_handle, *m_lastAutoAsmResult);
    result["success"] = restored;

    if (restored) {
        QVariantList restoredAddresses;
        for (const auto& region : m_lastAutoAsmResult->patchedRegions) {
            restoredAddresses.append(QString::number(region.address, 16).toUpper());
        }
        result["restoredAddresses"] = restoredAddresses;
        m_lastAutoAsmResult.reset();
        result["active"] = false;
        KE_LOG_WARN() << "Auto-assembler script restored.";
    } else {
        result["error"] = "Échec de la restauration du script auto-assembler.";
        result["active"] = true;
    }

    m_appendScanTelemetry("auto_assembler_restore", result);
    return result;
}

} // namespace killengine
