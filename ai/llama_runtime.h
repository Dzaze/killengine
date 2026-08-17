#pragma once

#include "model_locator.h"
#include "tool_registry.h"

#include <QString>
#include <QStringList>
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
    /// "llama-server" si servi par le serveur persistant, "llama-cli" sinon.
    QString backend;
};

class LlamaRuntime {
public:
    bool init();
    bool isAvailable() const;
    LlamaRuntimeInfo info() const;
    /// Contexte d'etat transmis au prompt pour choisir le bon outil
    /// (processus attache, scan actif, candidats restants, valeurs initiale/cible...).
    /// Champs optionnels supplementaires:
    /// - "history": QVariantList de {query, tool, outcome} (conversation recente)
    /// - "unknownSnapshotActive": bool (snapshot unknown capture)
    /// - "freezeCount": int (adresses gelees)
    /// - "valueType": dernier type de valeur utilise
    LlamaGenerationResult planToolCall(
        const QString& query,
        const ToolRegistry& registry,
        const QVariantMap& context = {}) const;
    LlamaGenerationResult planIntent(const QString& query) const;

    static QVariantMap extractToolCallJson(const QString& text, QString* error = nullptr);
    static QVariantMap extractIntentJson(const QString& text, QString* error = nullptr);

private:
    /// Completion serveur persistant si dispo, sinon llama-cli one-shot.
    LlamaGenerationResult generate(const QString& prompt, int nPredict) const;
    static QString findExecutable();
    static QString buildPrompt(const QString& query, const ToolRegistry& registry, const QVariantMap& context);
    static QString buildContextBlock(const QVariantMap& context);
    static QString buildHistoryBlock(const QVariantMap& context);
    static QString buildDynamicHints(const QVariantMap& context);
    static QString buildIntentPrompt(const QString& query);

    LlamaRuntimeInfo m_info;
    QString m_serverExecutablePath;
    /// true tant que le serveur persistant reste utilisable (retombe sur llama-cli sinon).
    mutable bool m_serverUsable{true};
};

} // namespace killai