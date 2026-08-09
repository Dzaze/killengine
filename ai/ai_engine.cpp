#include "ai_engine.h"
#include "logging/logger.h"

#include <QRegularExpression>

namespace killai {

AIEngine::AIEngine(QObject* parent) : QObject(parent) {}
AIEngine::~AIEngine() {}

bool AIEngine::init() {
    const bool llamaReady = m_llama.init();
    if (llamaReady) {
        const auto info = m_llama.info();
        KE_LOG_INFO() << "AIEngine::init() - llama.cpp runtime ready model="
                      << info.modelPath.toStdString();
    } else {
        KE_LOG_INFO() << "AIEngine::init() - deterministic fallback ready; llama unavailable: "
                      << m_llama.info().errorMessage.toStdString();
    }
    m_ready = true;
    return true;
}

bool AIEngine::isReady() const { return m_ready; }

QVariantMap AIEngine::processQuery(const QString& query) {
    const QString q = query.toLower();

    if (!m_ready) {
        QVariantMap result;
        result["status"] = "not_ready";
        result["message"] = "AIEngine is not initialized.";
        return result;
    }

    if (m_llama.isAvailable()) {
        const auto generated = m_llama.planToolCall(query, m_registry);
        if (generated.success) {
            QString error;
            const QVariantMap call = LlamaRuntime::extractToolCallJson(generated.output, &error);
            if (!call.isEmpty()) {
                QVariantMap result;
                result["status"] = m_validator.validate(call, &error) ? "tool_call" : "invalid_tool_call";
                result["tool"] = call.value("tool").toString();
                result["args"] = call.value("args").toMap();
                result["rationale"] = "Plan généré par le modèle local llama.cpp/Qwen.";
                result["state"] = m_stateMachine.currentStateName();
                result["aiBackend"] = "llama.cpp";
                result["error"] = error;
                if (result.value("status").toString() == "tool_call") {
                    return result;
                }
                KE_LOG_INFO() << "AIEngine model tool call rejected: " << error.toStdString();
            } else {
                KE_LOG_INFO() << "AIEngine model output rejected: " << error.toStdString();
            }
        } else {
            KE_LOG_INFO() << "AIEngine model generation failed: " << generated.errorMessage.toStdString();
        }
    }

    auto fallback = deterministicPlan(query);
    fallback["aiBackend"] = "deterministic";
    if (m_llama.info().available == false && !m_llama.info().errorMessage.isEmpty()) {
        fallback["aiBackendNote"] = m_llama.info().errorMessage;
    }
    return fallback;
}

QVariantMap AIEngine::deterministicPlan(const QString& query) {
    const QString q = query.toLower();

    if (q.contains("unknown") || q.contains("inconnue")) {
        if (q.contains("capture") || q.contains("initial")) {
            m_stateMachine.setState(AIState::WaitingForUserChange);
            return makeToolCall("unknown_capture", {}, "Capture initiale pour valeur inconnue.");
        }

        QString mode = "changed";
        if (q.contains("augment") || q.contains("increased")) mode = "increased";
        if (q.contains("diminu") || q.contains("decreased")) mode = "decreased";
        if (q.contains("pareil") || q.contains("unchanged")) mode = "unchanged";
        return makeToolCall("unknown_compare", {{"mode", mode}, {"valueType", inferValueType(query)}}, "Comparaison unknown initial value.");
    }

    if (q.contains("next") || q.contains("changed") || q.contains("change") ||
        q.contains("increased") || q.contains("augment") ||
        q.contains("decreased") || q.contains("diminu") ||
        q.contains("inchang")) {
        QString mode = "changed";
        if (q.contains("exact")) mode = "exact";
        if (q.contains("increased") || q.contains("augment")) mode = "increased";
        if (q.contains("decreased") || q.contains("diminu")) mode = "decreased";
        if (q.contains("unchanged") || q.contains("inchang")) mode = "unchanged";
        m_stateMachine.setState(AIState::Refining);
        return makeToolCall("next_scan", {{"mode", mode}, {"value", firstNumber(query)}}, "Réduction des candidats.");
    }

    if (q.contains("freeze") || q.contains("geler")) {
        return makeToolCall("freeze_value", {
            {"address", firstHexAddress(query)},
            {"valueType", inferValueType(query)},
            {"value", firstNumber(query)},
            {"enabled", true},
        }, "Freeze demandé par l'utilisateur.");
    }

    if (q.contains("write") || q.contains("écri") || q.contains("mettre")) {
        return makeToolCall("write_value", {
            {"address", firstHexAddress(query)},
            {"valueType", inferValueType(query)},
            {"value", firstNumber(query)},
        }, "Écriture mémoire demandée.");
    }

    const QString value = firstNumber(query);
    if (!value.isEmpty()) {
        m_stateMachine.setState(AIState::FirstScanRunning);
        return makeToolCall("exact_scan", {{"value", value}, {"valueType", inferValueType(query)}}, "Premier scan exact depuis une valeur détectée.");
    }

    QVariantMap result;
    result["status"] = "needs_clarification";
    result["message"] = "Je n'ai pas trouvé de valeur ou d'action claire.";
    result["state"] = m_stateMachine.currentStateName();
    result["availableTools"] = m_registry.availableTools();
    return result;
}

QVariantMap AIEngine::makeToolCall(const QString& tool, const QVariantMap& args, const QString& rationale) {
    QVariantMap call;
    call["tool"] = tool;
    call["args"] = args;

    QString error;
    QVariantMap result;
    result["status"] = m_validator.validate(call, &error) ? "tool_call" : "invalid_tool_call";
    result["tool"] = tool;
    result["args"] = args;
    result["rationale"] = rationale;
    result["state"] = m_stateMachine.currentStateName();
    result["error"] = error;
    return result;
}

QString AIEngine::inferValueType(const QString& query) {
    const QString q = query.toLower();
    if (q.contains("double") || q.contains("float64")) return "Float64";
    if (q.contains("float") || q.contains("float32")) return "Float32";
    if (q.contains("int64") || q.contains("long")) return "Int64";
    return "Int32";
}

QString AIEngine::firstNumber(const QString& query) {
    const QRegularExpression re(R"([-+]?\d+(?:[\.,]\d+)?)");
    const auto match = re.match(query);
    if (!match.hasMatch()) return {};
    return match.captured(0).replace(',', '.');
}

QString AIEngine::firstHexAddress(const QString& query) {
    const QRegularExpression re(R"(0x[0-9a-fA-F]+)");
    const auto match = re.match(query);
    return match.hasMatch() ? match.captured(0) : QString();
}

} // namespace killai
