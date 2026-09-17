// PORT-3a (docs/PORTABILITY_ROADMAP.md, 17/09/2026) : QSettings::sync() ne
// retourne rien -- ce test vérifie que killcore::commitSettingsSync() détecte
// bien un vrai échec de synchronisation (dossier non inscriptible) plutôt que
// de faire confiance à un sync() muet.

#include "settings/settings_persistence.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

TEST(SettingsPersistenceTest, SucceedsOnWritableLocationWithNoError) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QSettings settings(dir.filePath("test.ini"), QSettings::IniFormat);
    settings.setValue("a/b", "c");

    QString error;
    EXPECT_TRUE(killcore::commitSettingsSync(settings, &error));
    EXPECT_TRUE(error.isEmpty());
}

// Refus d'écriture simulé par un fichier en lecture seule (jamais un vrai
// remplissage de disque ni un changement des droits d'un dossier utilisateur
// partagé), conformément à la clôture demandée dans PORTABILITY_ROADMAP.md.
TEST(SettingsPersistenceTest, FailsWithNonEmptyErrorWhenIniFileIsReadOnly) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString iniPath = dir.filePath("test.ini");

    {
        QFile file(iniPath);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("[general]\na=1\n");
        file.close();
    }
    ASSERT_TRUE(QFile::setPermissions(iniPath, QFileDevice::ReadOwner | QFileDevice::ReadUser
                                              | QFileDevice::ReadGroup | QFileDevice::ReadOther));

    QSettings settings(iniPath, QSettings::IniFormat);
    settings.setValue("a", "2");
    QString error;
    const bool persisted = killcore::commitSettingsSync(settings, &error);

    // Restaure les droits avant toute assertion supplémentaire : ne jamais
    // laisser un fichier en lecture seule empêcher le nettoyage de
    // QTemporaryDir, même si une assertion ci-dessous échoue.
    QFile::setPermissions(iniPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                  | QFileDevice::ReadUser | QFileDevice::WriteUser
                                  | QFileDevice::ReadGroup | QFileDevice::ReadOther);

    EXPECT_FALSE(persisted);
    EXPECT_FALSE(error.isEmpty());
}
