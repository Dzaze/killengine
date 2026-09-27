// =============================================================================
// KillEngineDemoTarget — cible de démonstration distribuable (UX-PRODUIT-15)
//
// Reprend la FORME de tests/memory_targets/test_target_main.cpp (QMainWindow
// Qt Widgets) mais volontairement minimal : pas de modes stress/hook, pas de
// Player heap, pas de champ interpolé. Une seule cible principale (santé) +
// deux leurres qui ne changent jamais (éliminés par un next-scan) + une
// valeur de profil jamais modifiée (adresse stable module+offset).
//
// Canal privé stdin/stdout JSON-ligne pour que l'instance KillEngine parente
// (mode tutoriel, voir apps/desktop/tutorial_session_manager.*) connaisse la
// vérité terrain sans jamais l'afficher à l'utilisateur. Cet exécutable ne
// démarre jamais le pipe Automation public et est indépendant d'
// ApplicationController.
//
// Lecture bloquante sur un thread std::thread dédié (std::getline n'a pas de
// borne native -> lecture bornée via istream::getline sur un buffer fixe),
// marshalée vers le thread Qt (qui est aussi le thread main()/event loop)
// via QMetaObject::invokeMethod(..., Qt::QueuedConnection) : toutes les
// lectures/écritures des globales de jeu restent sur un seul et même thread,
// qu'elles viennent d'un clic bouton ou d'une requête IPC -- pas de mutex
// nécessaire sur les globales.
// =============================================================================

#include <QApplication>
#include <QCoreApplication>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLabel>
#include <QMainWindow>
#include <QMetaObject>
#include <QPushButton>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Variables mémoire connues (variables globales pour adresses stables)
// ---------------------------------------------------------------------------

// Cible principale du tutoriel, clampée [kHealthMin, kHealthMax].
static int32_t g_health = 100;
constexpr int32_t kHealthMin = 0;
constexpr int32_t kHealthMax = 200;
constexpr int32_t kHealthStep = 10;
constexpr int32_t kHealthInitial = 100;

// Leurres : même valeur initiale que g_health, ne changent jamais -- un
// premier scan exact/unknown les retrouve avec g_health, un next-scan après
// modification de la santé doit les éliminer.
static int32_t g_decoyAlpha = 100;
static int32_t g_decoyBeta = 100;

// Jamais modifiée : adresse module+offset stable, exercice de profil/relance
// (préparation 15C, pas exploitée par cette passe).
static int32_t g_profileTarget = 777;

// Ligne d'entrée du canal privé bornée à 64 Kio.
constexpr std::size_t kMaxIpcLineBytes = 64 * 1024;

namespace {

QString addressHex(const void* ptr) {
    return QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(ptr), 0, 16);
}

// Lit une ligne bornée sur std::cin. Retourne false uniquement à EOF sans
// données restantes. Si la ligne dépasse kMaxIpcLineBytes avant le prochain
// '\n', outTooLong est mis à true et le flux est resynchronisé (le reste de
// la ligne trop longue est consommé jusqu'au prochain saut de ligne) plutôt
// que de planter ou de rester désynchronisé pour les requêtes suivantes.
bool readBoundedLine(std::string& outLine, bool& outTooLong) {
    static std::vector<char> buffer(kMaxIpcLineBytes + 1);
    outTooLong = false;

    if (!std::cin.getline(buffer.data(), static_cast<std::streamsize>(buffer.size()))) {
        if (std::cin.eof()) {
            if (std::cin.gcount() > 0) {
                outLine.assign(buffer.data());
                return true;
            }
            return false;
        }
        if (std::cin.fail()) {
            outTooLong = true;
            std::cin.clear();
            std::cin.ignore((std::numeric_limits<std::streamsize>::max)(), '\n');
            outLine.clear();
            return true;
        }
        return false;
    }

    outLine.assign(buffer.data());
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Fenêtre principale
// ---------------------------------------------------------------------------
class DemoTargetWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit DemoTargetWindow(QWidget* parent = nullptr) : QMainWindow(parent) {
        setWindowTitle(QStringLiteral("KillEngine — Tutoriel"));
        setMinimumSize(420, 260);

        auto* central = new QWidget(this);
        auto* rootLayout = new QVBoxLayout(central);

        auto* header = new QLabel(
            QStringLiteral("<b>Environnement de démonstration</b><br>"
                            "Cible jetable pour s'entraîner sans risque sur une vraie fenêtre."),
            central);
        header->setWordWrap(true);
        header->setStyleSheet(QStringLiteral("padding: 8px;"));
        rootLayout->addWidget(header);

        auto* healthGroup = new QGroupBox(QStringLiteral("Santé"), central);
        auto* healthLayout = new QGridLayout(healthGroup);
        healthLayout->addWidget(new QLabel(QStringLiteral("Valeur affichée :"), healthGroup), 0, 0);
        m_healthLabel = new QLabel(QString::number(g_health), healthGroup);
        healthLayout->addWidget(m_healthLabel, 0, 1);
        rootLayout->addWidget(healthGroup);

        auto* actionsGroup = new QGroupBox(QStringLiteral("Actions"), central);
        auto* actionsLayout = new QHBoxLayout(actionsGroup);

        auto* damageBtn = new QPushButton(QStringLiteral("Dégâts (-10)"), actionsGroup);
        connect(damageBtn, &QPushButton::clicked, this, [this]() {
            g_health = std::max(kHealthMin, g_health - kHealthStep);
            updateLabels();
        });
        actionsLayout->addWidget(damageBtn);

        auto* healBtn = new QPushButton(QStringLiteral("Soin (+10)"), actionsGroup);
        connect(healBtn, &QPushButton::clicked, this, [this]() {
            g_health = std::min(kHealthMax, g_health + kHealthStep);
            updateLabels();
        });
        actionsLayout->addWidget(healBtn);

        auto* resetBtn = new QPushButton(QStringLiteral("Réinitialiser"), actionsGroup);
        connect(resetBtn, &QPushButton::clicked, this, [this]() {
            g_health = kHealthInitial;
            updateLabels();
        });
        actionsLayout->addWidget(resetBtn);

        rootLayout->addWidget(actionsGroup);
        rootLayout->addStretch(1);

        setCentralWidget(central);
        updateLabels();
    }

    // Point d'entrée unique du canal IPC côté thread Qt -- appelé exclusivement
    // via QMetaObject::invokeMethod(..., Qt::QueuedConnection) depuis le thread
    // lecteur stdin dédié, jamais directement.
    Q_INVOKABLE void handleIpcLine(const QString& line, bool tooLong) {
        if (tooLong) {
            writeErrorResponse(QJsonValue(),
                QStringLiteral("Ligne trop longue (max %1 octets).").arg(kMaxIpcLineBytes));
            return;
        }

        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            writeErrorResponse(QJsonValue(), QStringLiteral("Requête JSON invalide."));
            return;
        }

        const QJsonObject request = doc.object();
        const QJsonValue id = request.value(QStringLiteral("id"));
        const QString method = request.value(QStringLiteral("method")).toString();

        if (method == QStringLiteral("getState")) {
            QJsonObject result;
            result[QStringLiteral("health")] = g_health;
            result[QStringLiteral("healthAddress")] = addressHex(&g_health);
            result[QStringLiteral("decoyAlpha")] = g_decoyAlpha;
            result[QStringLiteral("decoyAlphaAddress")] = addressHex(&g_decoyAlpha);
            result[QStringLiteral("decoyBeta")] = g_decoyBeta;
            result[QStringLiteral("decoyBetaAddress")] = addressHex(&g_decoyBeta);
            result[QStringLiteral("profileTarget")] = g_profileTarget;
            result[QStringLiteral("profileTargetAddress")] = addressHex(&g_profileTarget);
            writeResultResponse(id, result);
        } else if (method == QStringLiteral("close")) {
            writeResultResponse(id, QJsonValue(true));
            QApplication::quit();
        } else {
            writeErrorResponse(id, QStringLiteral("Méthode inconnue : %1").arg(method));
        }
    }

private:
    void updateLabels() {
        m_healthLabel->setText(QString::number(g_health));
    }

    static void writeResultResponse(const QJsonValue& id, const QJsonValue& result) {
        QJsonObject response;
        response[QStringLiteral("id")] = id;
        response[QStringLiteral("result")] = result;
        writeLine(response);
    }

    static void writeErrorResponse(const QJsonValue& id, const QString& error) {
        QJsonObject response;
        response[QStringLiteral("id")] = id;
        response[QStringLiteral("error")] = error;
        writeLine(response);
    }

    static void writeLine(const QJsonObject& response) {
        const QByteArray bytes = QJsonDocument(response).toJson(QJsonDocument::Compact);
        std::cout << bytes.constData() << "\n";
        std::cout.flush();
    }

    QLabel* m_healthLabel{nullptr};
};

#include "demo_target_main.moc"

// ---------------------------------------------------------------------------
// Thread lecteur stdin -- bloquant par nature (I/O console), détaché : il ne
// peut pas être proprement annulé, il meurt avec le process au retour de
// main(). Ne touche jamais aux globales de jeu directement, uniquement au
// marshaling vers le thread Qt.
// ---------------------------------------------------------------------------
static void runIpcReaderThread(DemoTargetWindow* window) {
    std::string line;
    bool tooLong = false;
    while (readBoundedLine(line, tooLong)) {
        QMetaObject::invokeMethod(
            window, "handleIpcLine", Qt::QueuedConnection,
            Q_ARG(QString, tooLong ? QString() : QString::fromStdString(line)),
            Q_ARG(bool, tooLong));
    }
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("KillEngineDemoTarget"));

    // Poignée de main initiale : le parent (mode tutoriel) attend ce message
    // exact avant de considérer la cible démo prête et d'exposer les boutons
    // du guide interactif (15C, futur chantier).
    {
        QJsonObject readyMsg;
        readyMsg[QStringLiteral("type")] = QStringLiteral("ready");
        readyMsg[QStringLiteral("nonce")] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        readyMsg[QStringLiteral("protocolVersion")] = 1;
        readyMsg[QStringLiteral("pid")] = static_cast<qint64>(QCoreApplication::applicationPid());
        const QByteArray bytes = QJsonDocument(readyMsg).toJson(QJsonDocument::Compact);
        std::cout << bytes.constData() << "\n";
        std::cout.flush();
    }

    DemoTargetWindow window;
    window.show();

    std::thread ipcThread(runIpcReaderThread, &window);
    ipcThread.detach();

    return QApplication::exec();
}
