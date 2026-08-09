#pragma once

#include <QString>
#include <QList>
#include <cstdint>

namespace killcore {

/// Architecture d'un processus
enum class Architecture {
    Unknown,
    x86,
    x64
};

/// Informations sur un processus énuméré
struct ProcessModuleInfo {
    QString  name;
    QString  path;
    quint64   baseAddress{0};
    quint64   size{0};
};

struct ProcessInfo {
    uint32_t       pid{0};
    QString        name;
    QString        executablePath;
    Architecture   arch{Architecture::Unknown};
    bool           hasWindow{false};
    QList<ProcessModuleInfo> modules;
};

/**
 * @brief Énumère les processus Windows.
 *
 * Phase 1: implémentation avec CreateToolhelp32Snapshot.
 */
class ProcessEnumerator {
public:
    /// Retourne tous les processus accessibles.
    static QList<ProcessInfo> enumerate();

    /// Retourne les processus avec fenêtre visible (priorité UX).
    static QList<ProcessInfo> enumerateWithWindows();

    /// Retourne le processus au premier plan.
    static ProcessInfo foregroundProcess();

    /// Retourne les modules chargés par un processus.
    static QList<ProcessModuleInfo> enumerateModules(uint32_t pid);

    /// Convertit l'architecture en string.
    static QString architectureToString(Architecture arch);
};

} // namespace killcore
