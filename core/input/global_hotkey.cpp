#include "global_hotkey.h"

#include "logging/logger.h"

#include <QCoreApplication>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace killcore {

QString HotkeyCombo::toString() const {
    QString result;
    if (ctrl)  result += "Ctrl+";
    if (alt)   result += "Alt+";
    if (shift) result += "Shift+";
    if (win)   result += "Win+";

#ifdef Q_OS_WIN
    // Mapper les VK codes vers des noms lisibles
    if (keyCode >= VK_F1 && keyCode <= VK_F24) {
        result += QStringLiteral("F%1").arg(keyCode - VK_F1 + 1);
    } else if (keyCode >= '0' && keyCode <= '9') {
        result += QChar(static_cast<char>(keyCode));
    } else if (keyCode >= 'A' && keyCode <= 'Z') {
        result += QChar(static_cast<char>(keyCode));
    } else {
        result += QStringLiteral("VK_0x%1").arg(keyCode, 0, 16);
    }
#else
    result += QStringLiteral("VK_0x%1").arg(keyCode, 0, 16);
#endif

    return result;
}

HotkeyCombo HotkeyCombo::fromString(const QString& text) {
    HotkeyCombo combo;
    const QStringList parts = text.split('+', Qt::SkipEmptyParts);

    for (const QString& part : parts) {
        const QString trimmed = part.trimmed().toLower();
        if (trimmed == "ctrl" || trimmed == "control") {
            combo.ctrl = true;
        } else if (trimmed == "alt") {
            combo.alt = true;
        } else if (trimmed == "shift") {
            combo.shift = true;
        } else if (trimmed == "win") {
            combo.win = true;
        } else if (trimmed.startsWith('f') && trimmed.size() > 1) {
            // F1-F24
            bool ok = false;
            const int num = trimmed.mid(1).toInt(&ok);
            if (ok && num >= 1 && num <= 24) {
#ifdef Q_OS_WIN
                combo.keyCode = VK_F1 + num - 1;
#endif
            }
        } else if (trimmed.size() == 1) {
            combo.keyCode = static_cast<uint32_t>(trimmed[0].toUpper().toLatin1());
        }
    }

    return combo;
}

GlobalHotkeyManager::GlobalHotkeyManager(QObject* parent)
    : QObject(parent) {
}

GlobalHotkeyManager::~GlobalHotkeyManager() {
    unregisterAll();
    if (m_filterInstalled && QCoreApplication::instance()) {
        QCoreApplication::instance()->removeNativeEventFilter(this);
    }
}

int GlobalHotkeyManager::registerHotkey(const HotkeyCombo& combo, const HotkeyAction& action) {
#ifdef Q_OS_WIN
    if (!m_filterInstalled && QCoreApplication::instance()) {
        QCoreApplication::instance()->installNativeEventFilter(this);
        m_filterInstalled = true;
    }

    const int id = m_nextId++;

    uint32_t modifiers = MOD_NOREPEAT;
    if (combo.ctrl)  modifiers |= MOD_CONTROL;
    if (combo.alt)   modifiers |= MOD_ALT;
    if (combo.shift) modifiers |= MOD_SHIFT;
    if (combo.win)   modifiers |= MOD_WIN;

    if (!RegisterHotKey(nullptr, id, modifiers, combo.keyCode)) {
        KE_LOG_WARN() << "GlobalHotkey: RegisterHotKey failed for " << combo.toString().toStdString()
                      << " (error=" << GetLastError() << ")";
        return -1;
    }

    HotkeyEntry entry;
    entry.id = id;
    entry.combo = combo;
    entry.action = action;
    m_hotkeys.append(entry);

    KE_LOG_INFO() << "GlobalHotkey: registered " << combo.toString().toStdString() << " -> " << action.label.toStdString();
    return id;
#else
    (void)combo;
    (void)action;
    return -1;
#endif
}

bool GlobalHotkeyManager::unregisterHotkey(int id) {
#ifdef Q_OS_WIN
    for (int i = 0; i < m_hotkeys.size(); ++i) {
        if (m_hotkeys[i].id == id) {
            UnregisterHotKey(nullptr, id);
            m_hotkeys.removeAt(i);
            return true;
        }
    }
    return false;
#else
    (void)id;
    return false;
#endif
}

void GlobalHotkeyManager::unregisterAll() {
#ifdef Q_OS_WIN
    for (const auto& entry : m_hotkeys) {
        UnregisterHotKey(nullptr, entry.id);
    }
    m_hotkeys.clear();
    if (m_filterInstalled && QCoreApplication::instance()) {
        QCoreApplication::instance()->removeNativeEventFilter(this);
        m_filterInstalled = false;
    }
#endif
}

QList<QPair<HotkeyCombo, HotkeyAction>> GlobalHotkeyManager::registeredHotkeys() const {
    QList<QPair<HotkeyCombo, HotkeyAction>> result;
    for (const auto& entry : m_hotkeys) {
        result.append(qMakePair(entry.combo, entry.action));
    }
    return result;
}

bool GlobalHotkeyManager::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) {
#ifdef Q_OS_WIN
    (void)result;

    if (eventType != "windows_generic_MSG" && eventType != "windows_dispatcher_MSG") {
        return false;
    }

    MSG* msg = static_cast<MSG*>(message);
    if (!msg || msg->message != WM_HOTKEY) {
        return false;
    }

    const int id = static_cast<int>(msg->wParam);
    for (const auto& entry : m_hotkeys) {
        if (entry.id == id) {
            emit hotkeyTriggered(entry.id, entry.action);
            return true;
        }
    }
#else
    (void)eventType;
    (void)message;
    (void)result;
#endif
    return false;
}

} // namespace killcore
