#include "localization.h"

#include <QSettings>

namespace killcore {

QString currentUiLanguage() {
    return QSettings().value("ui/language", "fr").toString() == "en" ? "en" : "fr";
}

QString localizedText(const QString& fr, const QString& en) {
    return currentUiLanguage() == "en" ? en : fr;
}

} // namespace killcore
