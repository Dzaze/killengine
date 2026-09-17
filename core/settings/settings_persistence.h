#pragma once

#include "localization/localization.h"

#include <QSettings>
#include <QString>

namespace killcore {

/// PORT-3a (docs/PORTABILITY_ROADMAP.md, 17/09/2026) : QSettings::sync() ne
/// retourne rien -- un dossier non inscriptible (mode portable extrait dans
/// un emplacement en lecture seule, disque plein, etc.) échoue silencieusement
/// sans vérifier `settings.status()` explicitement après. Chaque setter qui
/// annonçait `success=true` juste après `sync()` (saveSettings, setUiLanguage,
/// setApiKey, clearApiKey, setActiveBackend) partageait ce même défaut ;
/// centralisé ici pour qu'ils se comportent tous pareil, sans élévation
/// automatique ni tentative de contournement du dossier non inscriptible.
/// Utilitaire Qt::Core pur (aucune dépendance à ApplicationController), placé
/// dans `killcore_logging` (voir core/CMakeLists.txt) pour rester testable
/// sans lier tout `apps/desktop`.
inline bool commitSettingsSync(QSettings& settings, QString* errorMessage) {
    settings.sync();
    if (settings.status() == QSettings::NoError) {
        return true;
    }
    if (errorMessage) {
        *errorMessage = settings.status() == QSettings::AccessError
            ? KE_TXT("Impossible d'enregistrer les réglages : dossier non accessible en écriture.",
                     "Unable to save settings: directory not writable.")
            : KE_TXT("Erreur lors de l'enregistrement des réglages.",
                     "Error while saving settings.");
    }
    return false;
}

} // namespace killcore
