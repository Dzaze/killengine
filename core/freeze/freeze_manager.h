#pragma once

#include "scanner/scan_types.h"

#include <QByteArray>
#include <QList>

#include <cstdint>

namespace killcore {

enum class FreezeMode {
    Polling,
    HardwareBreakpoint,
};

struct FreezeEntry {
    uint64_t address{0};
    ValueType type{ValueType::Int32};
    QByteArray value;
    bool enabled{true};
    FreezeMode mode{FreezeMode::Polling};

    // Fiabilite (Phase 18) : un freeze par polling reecrit a intervalle fixe,
    // mais rien ne garantit qu'il tienne entre deux ticks si le jeu reecrit
    // plus vite. Ces compteurs, mis a jour par FreezeManager::recordPollTick,
    // detectent ce cas SANS que l'utilisateur ait besoin de le signaler.
    int consecutiveDriftTicks{0};   ///< Ticks consecutifs ou la valeur lue avant reecriture ne correspondait pas
    int totalDriftTicks{0};         ///< Total de ticks en derive depuis l'activation
    int totalTicks{0};              ///< Total de ticks observes depuis l'activation
    bool flaggedUnstable{false};    ///< Deja signale une fois (evite de re-notifier a chaque tick)
};

/// Nombre de ticks de derive consecutifs avant de considerer une entree
/// polling comme instable. Assez haut pour ignorer un hoquet ponctuel du
/// scheduler, assez bas pour rester reactif (a 100ms/tick par defaut,
/// 5 ticks = 500ms avant notification).
constexpr int kFreezePollDriftThreshold = 5;

class FreezeManager {
public:
    void clear();
    void setEntry(uint64_t address, ValueType type, const QByteArray& value, FreezeMode mode = FreezeMode::Polling);
    void remove(uint64_t address);
    void removeByMode(FreezeMode mode);
    bool isEmpty() const;
    bool hasMode(FreezeMode mode) const;
    const QList<FreezeEntry>& entries() const;
    QList<FreezeEntry> entriesForMode(FreezeMode mode) const;

    /// Enregistre le resultat d'un tick de polling pour une adresse : la
    /// valeur lue juste avant reecriture correspondait-elle a la cible ?
    /// Retourne true la premiere fois que cette entree franchit le seuil
    /// d'instabilite (pour notifier une seule fois, pas a chaque tick).
    bool recordPollTick(uint64_t address, bool valueMatchedBeforeRewrite);

private:
    QList<FreezeEntry> m_entries;
};

} // namespace killcore
