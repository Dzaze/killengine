#include "ai_engine.h"
#include "intent_contract.h"
#include "logging/logger.h"

#include <QRegularExpression>
#include <QVariantList>

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

QVariantMap AIEngine::processIntent(const QString& query) {
    if (!m_ready) {
        QVariantMap result;
        result["status"] = "not_ready";
        result["message"] = "AIEngine is not initialized.";
        return result;
    }

    const QString q = query.toLower();
    const bool looksLikeWriteWithoutValue =
        (q.contains("passe") || q.contains("passer") || q.contains("mets") || q.contains("met ")
         || q.contains("write") || q.contains("écri") || q.contains("ecri"))
        && firstNumber(query).isEmpty();
    if (looksLikeWriteWithoutValue) {
        QVariantMap result;
        result["status"] = "needs_clarification";
        result["intent"] = "Unknown";
        result["value"] = "";
        result["targetValue"] = "";
        result["addresses"] = QVariantList{};
        result["confidence"] = 0.9;
        result["missing"] = "Tu veux le passer à quelle valeur ?";
        result["message"] = result["missing"];
        result["aiBackend"] = "deterministic_guard";
        return result;
    }

    if (m_llama.isAvailable()) {
        const auto generated = m_llama.planIntent(query);
        if (generated.success) {
            QString error;
            QVariantMap intent = LlamaRuntime::extractIntentJson(generated.output, &error);
            if (!intent.isEmpty()) {
                const bool valid = IntentContract::validate(intent, &error);
                intent["status"] = valid ? "intent" : "needs_clarification";
                intent["aiBackend"] = "llama.cpp";
                intent["error"] = error;
                if (!valid && intent.value("message").toString().isEmpty()) {
                    intent["message"] = intent.value("missing").toString().isEmpty()
                        ? QString("Je dois préciser l'intention avant d'agir.")
                        : intent.value("missing").toString();
                }
                return intent;
            }
            KE_LOG_INFO() << "AIEngine model intent rejected: " << error.toStdString();
        } else {
            KE_LOG_INFO() << "AIEngine model intent generation failed: " << generated.errorMessage.toStdString();
        }
    }

    QVariantMap fallback = deterministicIntent(query);
    fallback["aiBackend"] = "deterministic";
    if (m_llama.info().available == false && !m_llama.info().errorMessage.isEmpty()) {
        fallback["aiBackendNote"] = m_llama.info().errorMessage;
    }
    return fallback;
}

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

QVariantMap AIEngine::deterministicIntent(const QString& query) {
    const QString q = query.toLower();
    QVariantMap result;
    result["status"] = "intent";
    result["intent"] = "Unknown";
    result["value"] = "";
    result["targetValue"] = "";
    result["addresses"] = QVariantList{};
    result["confidence"] = 0.5;
    result["missing"] = "";

    QVariantList addresses;
    const QRegularExpression addressRe(R"(0x[0-9a-fA-F]{5,16})");
    auto addressIt = addressRe.globalMatch(query);
    while (addressIt.hasNext()) {
        addresses.append(addressIt.next().captured(0));
    }

    QStringList numbers;
    const QRegularExpression numberRe(R"([-+]?\d+(?:[\.,]\d+)?)");
    auto numberIt = numberRe.globalMatch(query);
    while (numberIt.hasNext()) {
        numbers.append(numberIt.next().captured(0).replace(',', '.'));
    }

    if (q.contains("autre") || q.contains("nouveau") || q.contains("reset") || q.contains("recommence")) {
        result["intent"] = numbers.isEmpty() ? "ResetContext" : "ExactScan";
        if (!numbers.isEmpty()) result["value"] = numbers.first();
        result["confidence"] = 0.75;
    } else if (!addresses.isEmpty()) {
        result["intent"] = numbers.isEmpty() ? "ActivateMemoryTargets" : "WriteMemoryTargets";
        result["addresses"] = addresses;
        if (!numbers.isEmpty()) result["value"] = numbers.first();
        result["confidence"] = 0.9;
    } else if (numbers.size() >= 2) {
        result["intent"] = "GuidedScan";
        result["value"] = numbers.at(0);
        result["targetValue"] = numbers.at(1);
        result["confidence"] = 0.85;
    } else if (numbers.size() == 1) {
        if (q.contains("passe") || q.contains("passer") || q.contains("mets") || q.contains("met ")) {
            result["intent"] = "RewriteLastTargets";
        } else {
            result["intent"] = "ExactScan";
        }
        result["value"] = numbers.first();
        result["confidence"] = 0.7;
    } else if (q.contains("passe") || q.contains("passer") || q.contains("mets") || q.contains("met ")) {
        result["status"] = "needs_clarification";
        result["missing"] = "Tu veux le passer à quelle valeur ?";
        result["message"] = result["missing"];
    } else {
        result["status"] = "needs_clarification";
        result["missing"] = "Quelle valeur veux-tu chercher ?";
        result["message"] = result["missing"];
    }

    QString error;
    if (result.value("status").toString() == "intent" && !IntentContract::validate(result, &error)) {
        result["status"] = "needs_clarification";
        result["error"] = error;
        result["message"] = result.value("missing").toString().isEmpty()
            ? QString("Il manque une information pour continuer.")
            : result.value("missing").toString();
    }
    return result;
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
