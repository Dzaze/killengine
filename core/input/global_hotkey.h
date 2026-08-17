#pragma once

#include <QObject>
#include <QAbstractNativeEventFilter>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QVariant>

#include <cstdint>
#include <functional>

namespace killcore {

/// Représente une combinaison de touches (modificateur + touche).
struct HotkeyCombo {
    uint32_t keyCode{0};     // VK_F1, VK_F2, etc.
    bool ctrl{false};
    bool alt{false};
    bool shift{false};
    bool win{false};

    QString toString() const;
    static HotkeyCombo fromString(const QString& text);
};

/// Type d'action associée à une hotkey.
enum class HotkeyActionType {
    ToggleFreeze,
    TogglePatch,
    WriteValue,
    ToggleOverlay,
    Custom,
};

/// Une action associée à une hotkey.
struct HotkeyAction {
    HotkeyActionType type{HotkeyActionType::Custom};
    QString targetId;
    QString label;
    QVariant payload;
};

/**
 * @brief Gestionnaire de hotkeys globales.
 *
 * Utilise RegisterHotKey pour capturer des touches même quand KillEngine
 * n'a pas le focus. Associe chaque hotkey à une action.
 */
class GlobalHotkeyManager : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    explicit GlobalHotkeyManager(QObject* parent = nullptr);
    ~GlobalHotkeyManager() override;

    /// Enregistre une hotkey globale. Retourne l'ID attribué, ou -1 si échec.
    int registerHotkey(const HotkeyCombo& combo, const HotkeyAction& action);

    /// Supprime une hotkey par ID.
    bool unregisterHotkey(int id);

    /// Supprime toutes les hotkeys.
    void unregisterAll();

    /// Retourne la liste des hotkeys enregistrées.
    QList<QPair<HotkeyCombo, HotkeyAction>> registeredHotkeys() const;

    /// Indique si le filtre natif est installé. Utile pour éviter les régressions de démarrage.
    bool isNativeFilterInstalled() const { return m_filterInstalled; }

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

signals:
    void hotkeyTriggered(int id, const killcore::HotkeyAction& action);

private:
    struct HotkeyEntry {
        int id;
        HotkeyCombo combo;
        HotkeyAction action;
    };

    QList<HotkeyEntry> m_hotkeys;
    int m_nextId{1};
    bool m_filterInstalled{false};
};

} // namespace killcore

Q_DECLARE_METATYPE(killcore::HotkeyAction)
