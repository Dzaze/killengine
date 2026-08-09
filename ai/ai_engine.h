#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

#include "state_machine.h"
#include "llama_runtime.h"
#include "tool_registry.h"
#include "tool_validator.h"

namespace killai {

/**
 * @brief Moteur IA de KillEngine.
 *
 * Phase 9: socle tool-calling local, prêt pour branchement llama.cpp.
 */
class AIEngine : public QObject {
    Q_OBJECT
public:
    explicit AIEngine(QObject* parent = nullptr);
    ~AIEngine() override;

    /// Initialise le runtime IA (chargement différé du modèle).
    bool init();

    /// Le moteur IA est-il prêt ?
    bool isReady() const;

    /// Traite une requête utilisateur et retourne une décision JSON.
    QVariantMap processQuery(const QString& query);

private:
    QVariantMap makeToolCall(const QString& tool, const QVariantMap& args, const QString& rationale);
    QVariantMap deterministicPlan(const QString& query);
    static QString inferValueType(const QString& query);
    static QString firstNumber(const QString& query);
    static QString firstHexAddress(const QString& query);

    bool           m_ready{false};
    ToolRegistry   m_registry;
    ToolValidator  m_validator;
    StateMachine   m_stateMachine;
    LlamaRuntime   m_llama;
};

} // namespace killai
