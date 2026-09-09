#include <gtest/gtest.h>
#include "localization/localization.h"
#include <QSettings>

namespace {

// Sauvegarde/restaure "ui/language" autour du test, meme pattern que
// ScopedModelDisabled (tests/unit/test_ai_tools.cpp) pour ne pas polluer le
// reglage reel de la machine qui execute les tests.
class ScopedUiLanguage {
public:
    explicit ScopedUiLanguage(const QString& language) {
        QSettings settings;
        m_previous = settings.value("ui/language");
        settings.setValue("ui/language", language);
        settings.sync();
    }

    ~ScopedUiLanguage() {
        QSettings settings;
        if (m_previous.isValid()) {
            settings.setValue("ui/language", m_previous);
        } else {
            settings.remove("ui/language");
        }
        settings.sync();
    }

private:
    QVariant m_previous;
};

} // namespace

TEST(LocalizationTest, DefaultsToFrenchWhenSettingAbsent) {
    QSettings settings;
    const QVariant previous = settings.value("ui/language");
    settings.remove("ui/language");
    settings.sync();

    EXPECT_EQ(killcore::currentUiLanguage(), "fr");
    EXPECT_EQ(killcore::localizedText("bonjour", "hello"), "bonjour");

    if (previous.isValid()) {
        settings.setValue("ui/language", previous);
        settings.sync();
    }
}

TEST(LocalizationTest, ReturnsFrenchTextWhenLanguageIsFr) {
    ScopedUiLanguage lang("fr");
    EXPECT_EQ(killcore::currentUiLanguage(), "fr");
    EXPECT_EQ(killcore::localizedText("bonjour", "hello"), "bonjour");
}

TEST(LocalizationTest, ReturnsEnglishTextWhenLanguageIsEn) {
    ScopedUiLanguage lang("en");
    EXPECT_EQ(killcore::currentUiLanguage(), "en");
    EXPECT_EQ(killcore::localizedText("bonjour", "hello"), "hello");
}

TEST(LocalizationTest, UnknownLanguageValueFallsBackToFrench) {
    ScopedUiLanguage lang("de");
    EXPECT_EQ(killcore::currentUiLanguage(), "fr");
    EXPECT_EQ(killcore::localizedText("bonjour", "hello"), "bonjour");
}

TEST(LocalizationTest, KeTxtMacroInterpolatesWithArg) {
    ScopedUiLanguage lang("en");
    const QString result = KE_TXT("Valeur: %1", "Value: %1").arg(42);
    EXPECT_EQ(result, "Value: 42");
}
