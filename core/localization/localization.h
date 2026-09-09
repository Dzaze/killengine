#pragma once

#include <QString>

namespace killcore {

/**
 * @brief Localisation legere du texte genere par le backend (chat/IA).
 *
 * Contexte (docs/AI_CHAT_LOCALIZATION_ROADMAP.md, PHASE L1) : le texte que
 * l'Assistant affiche dans le chat (messages/rationales/erreurs) est
 * construit deterministiquement en C++ -- le modele local (llama.cpp/Qwen)
 * ne produit jamais de prose visible par l'utilisateur, seulement un objet
 * JSON de tool-call (voir ai/llama_runtime.cpp::buildPrompt). Localiser ce
 * texte est donc une question de traduction de templates C++, pas de
 * prompt-engineering multi-langue.
 *
 * Le choix de langue de l'utilisateur est deja persiste par
 * apps/desktop/settings_diagnostics_manager.cpp sous la cle QSettings
 * "ui/language" (le meme reglage lu/ecrit par getSettings()/saveSettings(),
 * reflete cote frontend par ui/src/stores/settings.ts::appLanguage) --
 * aucune nouvelle plomberie IPC n'etait necessaire pour ce module.
 */

/// Langue UI courante ("fr" ou "en"), lue depuis QSettings("ui/language").
/// Toute valeur autre que "en" retombe sur "fr" (comportement historique du
/// reglage, deja utilise par settings_diagnostics_manager.cpp).
QString currentUiLanguage();

/// Retourne `en` si la langue UI courante est l'anglais, sinon `fr`.
/// Usage direct: localizedText("Texte francais.", "English text.")
/// Usage courant (voir macro KE_TXT ci-dessous) pour eviter QStringLiteral a
/// chaque site d'appel.
QString localizedText(const QString& fr, const QString& en);

} // namespace killcore

/// Raccourci d'appel, coherent avec le style KE_LOG_* (core/logging/logger.h).
/// Usage: result["message"] = KE_TXT("Aucun processus attaché.", "No process attached.");
/// Les deux arguments restent des QString classiques : .arg(...) fonctionne
/// normalement sur le resultat, y compris pour interpoler une valeur dans le
/// texte traduit.
#define KE_TXT(fr, en) ::killcore::localizedText(QStringLiteral(fr), QStringLiteral(en))
