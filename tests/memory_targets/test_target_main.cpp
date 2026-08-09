// =============================================================================
// KillEngineTestTarget — Application de test pour validation des scans
//
// Expose des variables mémoire connues avec des boutons pour les modifier,
// permettant de valider tous les types de scans KillEngine de manière
// reproductible.
//
// Voir KILLENGINE_PROJECT_SPEC.md sections 69-72.
// =============================================================================

#include <QApplication>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QSpinBox>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>

// ---------------------------------------------------------------------------
// Variables mémoire connues (variables globales pour adresses stables)
// ---------------------------------------------------------------------------

// Phase 4 — Exact Scan
static int32_t   g_health  = 100;
static int32_t   g_money   = 41250;
static float     g_stamina = 75.0f;
static double    g_position = 123.456;

// Phase 7 — Unknown Initial Value (valeur invisible dans l'UI)
static int32_t   g_hidden_score = 5000;

// Variables supplémentaires pour tests multi-type
static int64_t   g_big_counter = 1000000LL;
static uint32_t  g_uint_value  = 999;

// Phase 71 — Adresses dynamiques (heap)
struct Player {
    int32_t  health{100};
    int32_t  mana{50};
    int32_t  money{41250};
    float    speed{1.0f};
};

static Player* g_player = nullptr;

// ---------------------------------------------------------------------------
// Générateur aléatoire pour variations réalistes
// ---------------------------------------------------------------------------
static std::mt19937 g_rng(42);

static int32_t randomInt(int min, int max) {
    std::uniform_int_distribution<int32_t> dist(min, max);
    return dist(g_rng);
}

// ---------------------------------------------------------------------------
// Fenêtre principale
// ---------------------------------------------------------------------------
class TestTargetWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit TestTargetWindow(QWidget* parent = nullptr) : QMainWindow(parent) {
        setWindowTitle("KillEngine Test Target");
        setMinimumSize(500, 600);

        auto* central = new QWidget(this);
        setCentralWidget(central);

        auto* mainLayout = new QVBoxLayout(central);

        // --- En-tête ---
        auto* header = new QLabel(
            "KillEngine Test Target\n"
            "Variables mémoire exposées pour validation des scans.");
        header->setStyleSheet("font-size: 14px; font-weight: bold; padding: 8px;");
        header->setAlignment(Qt::AlignCenter);
        mainLayout->addWidget(header);

        // --- Section valeurs visibles ---
        auto* valuesGroup = new QGroupBox("Valeurs visibles (Phase 4 — Exact Scan)");
        auto* valuesLayout = new QGridLayout(valuesGroup);

        m_healthLabel  = new QLabel("100");
        m_moneyLabel   = new QLabel("41250");
        m_staminaLabel = new QLabel("75.0");
        m_positionLabel = new QLabel("123.456");

        valuesLayout->addWidget(new QLabel("Health (Int32):"), 0, 0);
        valuesLayout->addWidget(m_healthLabel, 0, 1);
        valuesLayout->addWidget(new QLabel("Money (Int32):"), 1, 0);
        valuesLayout->addWidget(m_moneyLabel, 1, 1);
        valuesLayout->addWidget(new QLabel("Stamina (Float32):"), 2, 0);
        valuesLayout->addWidget(m_staminaLabel, 2, 1);
        valuesLayout->addWidget(new QLabel("Position (Float64):"), 3, 0);
        valuesLayout->addWidget(m_positionLabel, 3, 1);

        mainLayout->addWidget(valuesGroup);

        // --- Section variables heap ---
        auto* heapGroup = new QGroupBox("Player (Heap — Phase 71)");
        auto* heapLayout = new QGridLayout(heapGroup);

        m_playerHealthLabel = new QLabel("100");
        m_playerMoneyLabel  = new QLabel("41250");

        heapLayout->addWidget(new QLabel("Player Health:"), 0, 0);
        heapLayout->addWidget(m_playerHealthLabel, 0, 1);
        heapLayout->addWidget(new QLabel("Player Money:"), 1, 0);
        heapLayout->addWidget(m_playerMoneyLabel, 1, 1);

        auto* reallocBtn = new QPushButton("Reallocate Player");
        heapLayout->addWidget(reallocBtn, 2, 0, 1, 2);
        connect(reallocBtn, &QPushButton::clicked, this, &TestTargetWindow::reallocatePlayer);

        mainLayout->addWidget(heapGroup);

        // --- Section actions ---
        auto* actionsGroup = new QGroupBox("Actions");
        auto* actionsLayout = new QGridLayout(actionsGroup);

        // Money
        auto* spendBtn = new QPushButton("Spend 500");
        auto* gainBtn  = new QPushButton("Gain 1000");
        auto* setMoneyBtn = new QPushButton("Set Money:");
        m_moneySpin = new QSpinBox();
        m_moneySpin->setRange(0, 2000000000);
        m_moneySpin->setValue(41250);

        actionsLayout->addWidget(spendBtn, 0, 0);
        actionsLayout->addWidget(gainBtn, 0, 1);
        actionsLayout->addWidget(setMoneyBtn, 0, 2);
        actionsLayout->addWidget(m_moneySpin, 0, 3);

        connect(spendBtn, &QPushButton::clicked, this, [this]() {
            g_money = std::max(0, g_money - 500);
            if (g_player) g_player->money = std::max(0, g_player->money - 500);
            updateLabels();
        });

        connect(gainBtn, &QPushButton::clicked, this, [this]() {
            g_money += 1000;
            if (g_player) g_player->money += 1000;
            updateLabels();
        });

        connect(setMoneyBtn, &QPushButton::clicked, this, [this]() {
            g_money = m_moneySpin->value();
            if (g_player) g_player->money = m_moneySpin->value();
            updateLabels();
        });

        // Health
        auto* damageBtn = new QPushButton("Damage (-10)");
        auto* healBtn   = new QPushButton("Heal (+10)");

        actionsLayout->addWidget(damageBtn, 1, 0);
        actionsLayout->addWidget(healBtn, 1, 1);

        connect(damageBtn, &QPushButton::clicked, this, [this]() {
            g_health = std::max(0, g_health - 10);
            if (g_player) g_player->health = std::max(0, g_player->health - 10);
            updateLabels();
        });

        connect(healBtn, &QPushButton::clicked, this, [this]() {
            g_health = std::min(1000, g_health + 10);
            if (g_player) g_player->health = std::min(1000, g_player->health + 10);
            updateLabels();
        });

        // Stamina (float)
        auto* staminaBtn = new QPushButton("Drain Stamina (-5.5)");
        actionsLayout->addWidget(staminaBtn, 2, 0, 1, 2);
        connect(staminaBtn, &QPushButton::clicked, this, [this]() {
            g_stamina = std::max(0.0f, g_stamina - 5.5f);
            updateLabels();
        });

        // Position (double)
        auto* relocateBtn = new QPushButton("Relocate (Position += 7.89)");
        actionsLayout->addWidget(relocateBtn, 3, 0, 1, 4);
        connect(relocateBtn, &QPushButton::clicked, this, [this]() {
            g_position += 7.89;
            updateLabels();
        });

        mainLayout->addWidget(actionsGroup);

        // --- Section Unknown ---
        auto* unknownGroup = new QGroupBox("Unknown Value (Phase 7)");
        auto* unknownLayout = new QHBoxLayout(unknownGroup);

        auto* unknownIncBtn = new QPushButton("Increase Hidden");
        auto* unknownDecBtn = new QPushButton("Decrease Hidden");
        auto* unknownRandBtn = new QPushButton("Randomize Hidden");

        unknownLayout->addWidget(unknownIncBtn);
        unknownLayout->addWidget(unknownDecBtn);
        unknownLayout->addWidget(unknownRandBtn);

        connect(unknownIncBtn, &QPushButton::clicked, this, [this]() {
            g_hidden_score += randomInt(1, 50);
        });
        connect(unknownDecBtn, &QPushButton::clicked, this, [this]() {
            g_hidden_score -= randomInt(1, 50);
        });
        connect(unknownRandBtn, &QPushButton::clicked, this, [this]() {
            g_hidden_score = randomInt(0, 100000);
        });

        mainLayout->addWidget(unknownGroup);

        // --- Log ---
        m_log = new QTextEdit();
        m_log->setReadOnly(true);
        m_log->setMaximumHeight(120);
        m_log->append("TestTarget démarré.");
        m_log->append(QString("PID: %1").arg(QApplication::applicationPid()));
        mainLayout->addWidget(new QLabel("Log:"));
        mainLayout->addWidget(m_log);

        // --- Timer pour bruit mémoire ---
        // Modifie g_big_counter et g_uint_value périodiquement pour
        // simuler de l'activité et créer des faux positifs.
        m_noiseTimer = new QTimer(this);
        m_noiseTimer->setInterval(500);
        connect(m_noiseTimer, &QTimer::timeout, this, [this]() {
            g_big_counter += randomInt(1, 100);
            g_uint_value = static_cast<uint32_t>(randomInt(0, 999999));
        });
        m_noiseTimer->start();

        // Alloue le player initial
        g_player = new Player();
        m_log->append(QString("Player alloué à: 0x%1").arg(reinterpret_cast<quintptr>(g_player), 0, 16));
    }

    ~TestTargetWindow() override {
        delete g_player;
        g_player = nullptr;
    }

private slots:
    void reallocatePlayer() {
        delete g_player;
        g_player = new Player();
        m_log->append(QString("Player réalloué à: 0x%1").arg(reinterpret_cast<quintptr>(g_player), 0, 16));
        updateLabels();
    }

private:
    void updateLabels() {
        m_healthLabel->setText(QString::number(g_health));
        m_moneyLabel->setText(QString::number(g_money));
        m_staminaLabel->setText(QString::number(g_stamina, 'f', 1));
        m_positionLabel->setText(QString::number(g_position, 'f', 3));

        if (g_player) {
            m_playerHealthLabel->setText(QString::number(g_player->health));
            m_playerMoneyLabel->setText(QString::number(g_player->money));
        }
    }

    QLabel*     m_healthLabel{nullptr};
    QLabel*     m_moneyLabel{nullptr};
    QLabel*     m_staminaLabel{nullptr};
    QLabel*     m_positionLabel{nullptr};
    QLabel*     m_playerHealthLabel{nullptr};
    QLabel*     m_playerMoneyLabel{nullptr};
    QSpinBox*   m_moneySpin{nullptr};
    QTextEdit*  m_log{nullptr};
    QTimer*     m_noiseTimer{nullptr};
};

#include "test_target_main.moc"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("KillEngineTestTarget");

    TestTargetWindow window;
    window.show();

    return app.exec();
}