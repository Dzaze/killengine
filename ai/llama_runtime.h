#pragma once

#include "model_locator.h"
#include "tool_registry.h"

#include <QString>
#include <QVariantMap>

namespace killai {

struct LlamaRuntimeInfo {
    bool available{false};
    QString executablePath;
    QString modelPath;
    QString errorMessage;
};

struct LlamaGenerationResult {
    bool success{false};
    QString output;
    QString errorMessage;
};

class LlamaRuntime {
public:
    bool init();
    bool isAvailable() const;
    LlamaRuntimeInfo info() const;
    LlamaGenerationResult planToolCall(const QString& query, const ToolRegistry& registry) const;
    LlamaGenerationResult planIntent(const QString& query) const;

    static QVariantMap extractToolCallJson(const QString& text, QString* error = nullptr);
    static QVariantMap extractIntentJson(const QString& text, QString* error = nullptr);

private:
    static QString findExecutable();
    static QString buildPrompt(const QString& query, const ToolRegistry& registry);
    static QString buildIntentPrompt(const QString& query);

    LlamaRuntimeInfo m_info;
};

} // namespace killai
