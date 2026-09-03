#pragma once

#include <cstdint>
#include <functional>
#include <vector>
#include <optional>
#include <QString>

namespace killcore {

/// Configuration d'une surveillance post-ecriture
struct WatchdogConfig {
    uint32_t watchDurationMs{5000};      ///< Duree de surveillance apres ecriture
    uint32_t pollIntervalMs{100};          ///< Intervalle entre verifications
    uint32_t resyncThreshold{3};         ///< Ticks consecutifs pour confirmer resync
    bool detectTwinPattern{true};        ///< Activer detection pattern paire
    uint32_t twinSearchRange{32};        ///< Range de recherche pour pattern paire (octets)
};

/// Etat d'une entree de surveillance
enum class WatchdogState {
    Watching,      ///< Surveillance en cours
    ResyncDetected,///< Resynchronisation detectee
    Stable,        ///< Valeur stable (pas de resync)
    Expired        ///< Duree ecoulee sans resync
};

/// Resultat d'une verification
struct WatchdogCheckResult {
    WatchdogState state{WatchdogState::Watching};
    bool resyncConfirmed{false};         ///< Resync confirme (threshold atteint)
    uint32_t resyncTickCount{0};         ///< Nombre de ticks de resync observes
    std::optional<uint64_t> twinAddress; ///< Adresse jumelle suggeree (si pattern detecte)
    QString suggestion;                  ///< Message de suggestion pour l'utilisateur
};

/// Entree de surveillance individuelle
struct WatchdogEntry {
    uint64_t address{0};                 ///< Adresse surveillee
    uint64_t writtenValue{0};            ///< Valeur ecrite
    uint64_t originalValue{0};           ///< Valeur originale (avant ecriture)
    uint32_t valueSize{4};               ///< Taille en octets (1, 2, 4, 8)
    uint64_t startTimeMs{0};             ///< Timestamp de debut
    uint32_t consecutiveResyncTicks{0};  ///< Compteur de ticks de resync
    uint32_t totalChecks{0};             ///< Nombre total de verifications
    bool active{false};                  ///< Entree active
};

/// Detecteur de patterns de resynchronisation
class SmartWatchdog {
public:
    SmartWatchdog() = default;
    ~SmartWatchdog() = default;

    /// Configure le watchdog
    void setConfig(const WatchdogConfig& config) { m_config = config; }
    const WatchdogConfig& config() const { return m_config; }

    /// Ajoute une entree de surveillance
    void addEntry(const WatchdogEntry& entry);

    /// Supprime une entree par adresse
    void removeEntry(uint64_t address);

    /// Verifie toutes les entrees actives avec la valeur actuelle lue
    /// @param currentTimeMs Timestamp actuel
    /// @param getCurrentValue Callback pour lire la valeur actuelle a une adresse
    /// @return Liste des resultats par entree
    std::vector<std::pair<uint64_t, WatchdogCheckResult>> checkAll(
        uint64_t currentTimeMs,
        std::function<std::optional<uint64_t>(uint64_t address, uint32_t size)> getCurrentValue
    );

    /// Verifie une entree specifique
    WatchdogCheckResult checkEntry(
        const WatchdogEntry& entry,
        uint64_t currentTimeMs,
        uint64_t currentValue
    ) const;

    /// Detecte un pattern de copie jumelle (ex: affichage vs source)
    /// @param baseAddress Adresse de base (typiquement l'adresse ecrite)
    /// @param getValueAt Callback pour lire une valeur a une adresse
    /// @return Adresse jumelle suggeree si pattern detecte
    std::optional<uint64_t> detectTwinPattern(
        uint64_t baseAddress,
        uint64_t expectedValue,
        uint32_t valueSize,
        std::function<std::optional<uint64_t>(uint64_t address)> getValueAt
    ) const;

    /// Genere une suggestion utilisateur
    QString generateSuggestion(const WatchdogCheckResult& result, uint64_t address) const;

    /// Retourne toutes les entrees actives
    const std::vector<WatchdogEntry>& entries() const { return m_entries; }

    /// Efface toutes les entrees
    void clear() { m_entries.clear(); }

private:
    WatchdogConfig m_config;
    std::vector<WatchdogEntry> m_entries;

    /// Compare deux valeurs en tenant compte de la taille
    bool valuesEqual(uint64_t a, uint64_t b, uint32_t size) const;

    /// Extrait une valeur de taille variable depuis un uint64_t
    uint64_t maskValue(uint64_t value, uint32_t size) const;
};

} // namespace killcore
