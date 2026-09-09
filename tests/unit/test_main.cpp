#include <gtest/gtest.h>

#include <QCoreApplication>

namespace {

// PHASE (08/09/2026, docs/AI_CHAT_LOCALIZATION_ROADMAP.md, L1) : sans
// QCoreApplication ni organizationName/applicationName, QSettings() n'a
// aucune identite valide pour choisir un fichier/registre -- toute lecture
// retombe silencieusement sur la valeur par defaut et toute ecriture echoue
// (status() == QSettings::AccessError), sans erreur bruyante. Ce trou etait
// masque jusqu'ici : les tests existants qui touchent a QSettings
// (ex: ScopedModelDisabled, tests/unit/test_ai_tools.cpp) s'appuient TOUJOURS
// aussi sur une variable d'environnement en parallele, qui suffisait a elle
// seule a faire passer le test. Meme org/app name que la vraie application
// (apps/desktop/main.cpp) : les tests qui touchent QSettings partagent donc
// le meme emplacement reel que KillEngine.exe, d'ou l'importance de toujours
// sauvegarder/restaurer la valeur precedente autour d'un test (voir
// ScopedModelDisabled/ScopedUiLanguage pour le pattern).
class QtCoreApplicationEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char argv0[] = "killengine_unit_tests";
            static char* argv[] = {argv0, nullptr};
            m_app = new QCoreApplication(argc, argv);
        }
        QCoreApplication::setOrganizationName("KillEngine");
        QCoreApplication::setApplicationName("KillEngine");
    }

    void TearDown() override {
        delete m_app;
        m_app = nullptr;
    }

private:
    QCoreApplication* m_app{nullptr};
};

// S'enregistre pendant l'initialisation statique (avant main(), fourni par
// gtest_main) -- RUN_ALL_TESTS() appelle Environment::SetUp() une fois avant
// tous les tests et TearDown() une fois a la fin, peu importe qu'on ne
// possede pas main() ici.
const ::testing::Environment* const kQtCoreApplicationEnv =
    ::testing::AddGlobalTestEnvironment(new QtCoreApplicationEnvironment());

} // namespace
