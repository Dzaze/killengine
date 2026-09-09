#include "freeze_hotkey_overlay_manager.h"

#include "debug/breakpoint_freeze.h"
#include "logging/logger.h"
#include "localization/localization.h"
#include "memory/memory_reader.h"
#include "memory/memory_writer.h"
#include "scanner/scan_types.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QVariantList>
#include <QWidget>

#include <algorithm>
#include <utility>

namespace killengine {
namespace {
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

FreezeHotkeyOverlayManager::FreezeHotkeyOverlayManager(
    const killcore::ProcessHandle& handle,
    TelemetryCallback telemetry,
    PidCallback pid,
    AddressVerifiedCallback addressVerified,
    RestartBreakpointFreezeCallback restartBreakpointFreeze,
    ResultCallback globalHotkeyTriggered,
    ResultCallback freezeInstabilityDetected,
    QObject* parent)
    : QObject(parent)
    , m_handle(handle)
    , m_appendScanTelemetry(std::move(telemetry))
    , m_pid(std::move(pid))
    , m_hasAddressBeenWriteVerified(std::move(addressVerified))
    , m_restartBreakpointFreeze(std::move(restartBreakpointFreeze))
    , m_globalHotkeyTriggered(std::move(globalHotkeyTriggered))
    , m_freezeInstabilityDetected(std::move(freezeInstabilityDetected)) {
    m_freezeTimer.setInterval(100);
    connect(&m_freezeTimer, &QTimer::timeout, this, &FreezeHotkeyOverlayManager::applyFreezeTick);

    m_hotkeys = std::make_unique<killcore::GlobalHotkeyManager>();
    connect(m_hotkeys.get(), &killcore::GlobalHotkeyManager::hotkeyTriggered, this, [this](int id, const killcore::HotkeyAction& action) {
        QVariantMap event;
        event["id"] = id;
        event["targetId"] = action.targetId;
        event["label"] = action.label;
        event["payload"] = action.payload;
        switch (action.type) {
            case killcore::HotkeyActionType::ToggleFreeze: event["type"] = "toggle_freeze"; break;
            case killcore::HotkeyActionType::TogglePatch: event["type"] = "toggle_patch"; break;
            case killcore::HotkeyActionType::WriteValue: event["type"] = "write_value"; break;
            case killcore::HotkeyActionType::ToggleOverlay: event["type"] = "toggle_overlay"; break;
            case killcore::HotkeyActionType::Custom: event["type"] = "custom"; break;
        }
        m_appendScanTelemetry("global_hotkey_triggered", event);
        m_globalHotkeyTriggered(event);
    });
}

killcore::FreezeManager& FreezeHotkeyOverlayManager::freezeManager() {
    return m_freeze;
}

const killcore::FreezeManager& FreezeHotkeyOverlayManager::freezeManager() const {
    return m_freeze;
}

void FreezeHotkeyOverlayManager::clearFreezeState() {
    m_freeze.clear();
    m_freezeTimer.stop();
}

void FreezeHotkeyOverlayManager::stopFreezeTimer() {
    m_freezeTimer.stop();
}

QVariantMap FreezeHotkeyOverlayManager::setFreezeValue(const QString& addressHex, const QString& valueType, const QString& value, bool enabled) {
    QVariantMap result;
    result["success"] = false;

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
        return result;
    }

    if (!enabled) {
        bool wasHardwareBreakpoint = false;
        for (const auto& entry : m_freeze.entries()) {
            if (entry.address == address && entry.mode == killcore::FreezeMode::HardwareBreakpoint) {
                wasHardwareBreakpoint = true;
                break;
            }
        }
        m_freeze.remove(address);
        if (!m_freeze.hasMode(killcore::FreezeMode::Polling)) {
            m_freezeTimer.stop();
        }
        if (wasHardwareBreakpoint) {
            m_restartBreakpointFreeze(killcore::BreakpointFreezeMode::RewriteValue, nullptr);
        }
        result["success"] = true;
        result["enabled"] = false;
        return result;
    }

    killcore::ValueType type;
    if (!killcore::parseValueType(valueType, &type)) {
        result["error"] = KE_TXT("Type invalide.", "Invalid type.");
        return result;
    }

    killcore::ScanValue scanValue;
    QString parseError;
    if (!killcore::parseScanValue(value, type, &scanValue, &parseError)) {
        result["error"] = parseError;
        return result;
    }

    if (!m_hasAddressBeenWriteVerified(address)) {
        result["warning"] = KE_TXT("Cette adresse n'a jamais été écrite avec succès avant ce freeze — "
                             "si c'est un candidat frais (jamais testé par une écriture simple), "
                             "certaines cibles réagissent mal à une réécriture continue non vérifiée "
                             "(jusqu'au crash observé sur une cible réelle). Teste une écriture simple "
                             "et vérifie visuellement avant de figer, si possible.",
                             "This address has never been written successfully before this freeze. "
                             "If it is a fresh candidate that has not been tested with a simple write yet, "
                             "some targets react badly to unverified continuous rewrites "
                             "(up to a crash observed on a real target). Try one simple write "
                             "and check it visually before freezing, if possible.");
    }

    m_freeze.setEntry(address, type, killcore::scanValueToBytes(scanValue), killcore::FreezeMode::Polling);
    if (!m_freezeTimer.isActive()) {
        m_freezeTimer.start();
    }

    result["success"] = true;
    result["enabled"] = true;
    return result;
}

QVariantMap FreezeHotkeyOverlayManager::setFreezeInterval(int intervalMs) {
    QVariantMap result;

    const int clamped = (intervalMs <= 0) ? 100 : std::clamp(intervalMs, 10, 2000);

    m_freezeTimer.setInterval(clamped);

    const bool wasActive = m_freezeTimer.isActive();
    if (wasActive) {
        m_freezeTimer.stop();
        m_freezeTimer.start();
    }

    result["success"] = true;
    result["intervalMs"] = clamped;
    result["wasActive"] = wasActive;
    return result;
}

QVariantMap FreezeHotkeyOverlayManager::registerGlobalHotkey(const QString& comboText, const QVariantMap& actionMap) {
    QVariantMap result;
    result["success"] = false;
    result["combo"] = comboText;

    if (!m_hotkeys) {
        result["error"] = KE_TXT("Gestionnaire de hotkeys indisponible.", "Hotkey manager unavailable.");
        return result;
    }

    const killcore::HotkeyCombo combo = killcore::HotkeyCombo::fromString(comboText);
    if (combo.keyCode == 0) {
        result["error"] = KE_TXT("Combinaison invalide. Exemple: Ctrl+Alt+F1.", "Invalid combination. Example: Ctrl+Alt+F1.");
        return result;
    }

    const QString typeText = actionMap.value("type", "custom").toString().toLower();
    killcore::HotkeyAction action;
    action.type = killcore::HotkeyActionType::Custom;
    if (typeText == "toggle_freeze") {
        action.type = killcore::HotkeyActionType::ToggleFreeze;
    } else if (typeText == "toggle_patch") {
        action.type = killcore::HotkeyActionType::TogglePatch;
    } else if (typeText == "write_value") {
        action.type = killcore::HotkeyActionType::WriteValue;
    } else if (typeText == "toggle_overlay") {
        action.type = killcore::HotkeyActionType::ToggleOverlay;
    }
    action.targetId = actionMap.value("targetId").toString();
    action.label = actionMap.value("label", combo.toString()).toString();
    action.payload = actionMap.value("payload");

    const int id = m_hotkeys->registerHotkey(combo, action);
    if (id < 0) {
        result["error"] = KE_TXT("RegisterHotKey a échoué. La combinaison est peut-être déjà utilisée.", "RegisterHotKey failed. The combination may already be in use.");
        return result;
    }

    result["success"] = true;
    result["id"] = id;
    result["combo"] = combo.toString();
    result["type"] = typeText;
    result["targetId"] = action.targetId;
    m_appendScanTelemetry("global_hotkey_registered", result);
    return result;
}

QVariantMap FreezeHotkeyOverlayManager::unregisterGlobalHotkey(int id) {
    QVariantMap result;
    result["success"] = m_hotkeys && m_hotkeys->unregisterHotkey(id);
    result["id"] = id;
    if (!result.value("success").toBool()) {
        result["error"] = KE_TXT("Hotkey introuvable.", "Hotkey not found.");
    }
    m_appendScanTelemetry("global_hotkey_unregistered", result);
    return result;
}

QVariantMap FreezeHotkeyOverlayManager::getGlobalHotkeys() const {
    QVariantMap result;
    QVariantList hotkeys;
    if (m_hotkeys) {
        const auto entries = m_hotkeys->registeredHotkeys();
        for (int i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            QVariantMap item;
            item["index"] = i;
            item["combo"] = entry.first.toString();
            item["targetId"] = entry.second.targetId;
            item["label"] = entry.second.label;
            item["payload"] = entry.second.payload;
            hotkeys.append(item);
        }
    }
    result["success"] = true;
    result["hotkeys"] = hotkeys;
    return result;
}

QVariantMap FreezeHotkeyOverlayManager::clearGlobalHotkeys() {
    QVariantMap result;
    if (m_hotkeys) {
        m_hotkeys->unregisterAll();
    }
    result["success"] = true;
    m_appendScanTelemetry("global_hotkeys_cleared", result);
    return result;
}

QVariantMap FreezeHotkeyOverlayManager::setTrainerOverlayVisible(bool visible, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = true;
    result["visible"] = visible;

    if (visible) {
        if (!m_trainerOverlay) {
            auto* overlay = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
            overlay->setAttribute(Qt::WA_DeleteOnClose, false);
            overlay->setWindowTitle("KillEngine Trainer Overlay");
            overlay->setStyleSheet(
                "QWidget { background: rgba(22, 23, 31, 222); border: 1px solid rgba(122,162,247,110); border-radius: 8px; }"
                "QLabel { color: #c0caf5; font-family: 'Segoe UI'; font-size: 12px; }");
            auto* layout = new QVBoxLayout(overlay);
            layout->setContentsMargins(12, 10, 12, 10);
            auto* label = new QLabel(KE_TXT("KillEngine Trainer\nAucune feature active.", "KillEngine Trainer\nNo active features."), overlay);
            label->setTextFormat(Qt::PlainText);
            label->setWordWrap(true);
            layout->addWidget(label);
            m_trainerOverlay = overlay;
            m_trainerOverlayLabel = label;
        }
        const int x = options.value("x", 24).toInt();
        const int y = options.value("y", 24).toInt();
        const int width = std::clamp(options.value("width", 320).toInt(), 180, 640);
        const int height = std::clamp(options.value("height", 140).toInt(), 80, 480);
        m_trainerOverlay->setGeometry(x, y, width, height);
        m_trainerOverlay->show();
        m_trainerOverlay->raise();
    } else if (m_trainerOverlay) {
        m_trainerOverlay->hide();
    }

    m_appendScanTelemetry("trainer_overlay_visible", result);
    return result;
}

QVariantMap FreezeHotkeyOverlayManager::updateTrainerOverlay(const QVariantMap& state) {
    QVariantMap result;
    result["success"] = false;
    if (!m_trainerOverlay || !m_trainerOverlayLabel) {
        result["error"] = KE_TXT("Overlay Trainer non initialisé.", "Trainer overlay is not initialized.");
        return result;
    }

    const QString title = state.value("title", "KillEngine Trainer").toString();
    const QVariantList features = state.value("features").toList();
    QStringList lines;
    lines << title;
    for (const QVariant& item : features.mid(0, 10)) {
        const QVariantMap feature = item.toMap();
        const QString enabled = feature.value("enabled").toBool() ? "ON " : "OFF";
        lines << QString("%1  %2  %3")
            .arg(enabled, feature.value("name").toString(), feature.value("status").toString());
    }
    if (features.isEmpty()) {
        lines << KE_TXT("Aucune feature Trainer.", "No Trainer features.");
    }
    m_trainerOverlayLabel->setText(lines.join('\n'));
    result["success"] = true;
    result["lineCount"] = lines.size();
    return result;
}

void FreezeHotkeyOverlayManager::applyFreezeTick() {
    if (m_freeze.isEmpty() || m_pid() <= 0) {
        m_freezeTimer.stop();
        return;
    }

    killcore::ProcessHandle writeHandle(static_cast<uint32_t>(m_pid()), killcore::ProcessAccess::ReadWrite);
    if (!writeHandle.isValid()) {
        return;
    }

    killcore::MemoryWriter writer(writeHandle);
    killcore::MemoryReader reader(writeHandle);
    for (const auto& entry : m_freeze.entries()) {
        if (!entry.enabled || entry.mode != killcore::FreezeMode::Polling) {
            continue;
        }

        const auto read = reader.read(entry.address, static_cast<size_t>(entry.value.size()));
        const bool matched = (read.success || read.partial)
            && read.bytesRead == static_cast<size_t>(entry.value.size())
            && read.data == entry.value;

        const bool crossedThreshold = m_freeze.recordPollTick(entry.address, matched);

        writer.write(entry.address, entry.value, false);

        if (crossedThreshold) {
            const QString addressHex = QString::number(entry.address, 16).toUpper();
            const double holdRatePercent = entry.totalTicks > 0
                ? 100.0 * static_cast<double>(entry.totalTicks - entry.totalDriftTicks) / static_cast<double>(entry.totalTicks)
                : 0.0;

            QVariantMap info;
            info["address"] = addressHex;
            info["type"] = killcore::valueTypeToString(entry.type);
            info["mode"] = "polling";
            info["consecutiveDriftTicks"] = entry.consecutiveDriftTicks;
            info["totalDriftTicks"] = entry.totalDriftTicks;
            info["totalTicks"] = entry.totalTicks;
            info["holdRatePercent"] = holdRatePercent;
            info["message"] = KE_TXT(
                "Le freeze sur 0x%1 ne tient pas : la valeur repart avant chaque réécriture depuis %2 ticks d'affilée "
                "(tenue mesurée %3%). La cible réécrit probablement plus vite que l'intervalle de polling actuel.",
                "The freeze on 0x%1 is not holding: the value changes back before each rewrite for %2 ticks in a row "
                "(measured hold rate %3%). The target is probably rewriting faster than the current polling interval.")
                .arg(addressHex)
                .arg(entry.consecutiveDriftTicks)
                .arg(QString::number(holdRatePercent, 'f', 0));
            info["suggestion"] = KE_TXT("Passe en Freeze BP (bloque l'écriture à la source) ou lance Écrit par pour trouver l'instruction qui réécrit.", "Switch to Freeze BP (blocks the write at the source) or run Find What Writes to locate the rewriting instruction.");

            m_appendScanTelemetry("freeze_poll_instability", info);
            m_freezeInstabilityDetected(info);
        }
    }
}

} // namespace killengine
