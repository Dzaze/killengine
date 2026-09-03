#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace killengine {

/// PROPOSITIONS-1 #4 — Live Lua REPL.
///
/// Contrairement à executeLuaScript/executeLuaScriptAsync (un process
/// lua.exe relancé à chaque appel, aucun état partagé entre deux scripts),
/// ce manager garde UN SEUL process lua.exe vivant sur toute la durée de la
/// session REPL (scripts/killengine_repl_driver.lua) : les variables et
/// `require("killengine")` persistent d'une ligne à l'autre.
///
/// Le process est possédé entièrement par un thread dédié (workerLoop) —
/// jamais touché depuis le thread appelant — pour deux raisons : (1) éviter
/// le deadlock documenté pour executeLuaScript (une ligne qui appelle
/// `ke.call(...)` doit pouvoir joindre le pipe d'automatisation, donc
/// sendLine() ne doit JAMAIS bloquer le thread qui sert le pipe — voir
/// commentaire sur ke.call dans scripts/killengine.lua) ; (2) permettre un
/// vrai flux interactif (une ligne peut mettre du temps sans geler
/// l'UI/le pipe). sendLine() est donc asynchrone comme executeLuaScriptAsync :
/// elle retourne {started:true, requestId} immédiatement, le résultat réel
/// s'obtient via lineResult(requestId) (poll, sûr sur le pipe d'automatisation
/// qui ne relaie pas les signaux Qt) ou le signal lineFinished (frontend Qt).
///
/// ATTENTION COLLISION (02/09/2026) : ce fichier a été écrasé une fois par
/// une autre implémentation (VM Lua embarquée lua_State*, agent Kimi K2.5)
/// avant d'être restauré sur décision explicite du propriétaire — voir
/// docs/SALON.md et docs/PHASE_TRACKER.md. Ne pas re-remplacer sans accord
/// explicite du propriétaire.
class LuaReplManager : public QObject {
    Q_OBJECT
public:
    explicit LuaReplManager(QObject* parent = nullptr);
    ~LuaReplManager() override;

    /// Démarre le process REPL. Options : luaPath (override), timeoutMs
    /// (par ligne, défaut 15000, clamp 1000-120000), pipeName.
    QVariantMap start(const QVariantMap& options);

    /// Envoie une ligne au REPL. Ne bloque jamais — voir commentaire de
    /// classe. Échoue si le REPL n'est pas démarré.
    QVariantMap sendLine(const QString& line);

    /// Résultat d'une ligne envoyée via sendLine (poll par requestId).
    QVariantMap lineResult(int requestId) const;

    /// Historique complet (ou les `maxEntries` dernières entrées) de la
    /// session courante, dans l'ordre d'envoi.
    QVariantMap history(int maxEntries) const;

    /// Complétion des fonctions `ke.*` connues (voir
    /// scripting/lua_repl_protocol.h), filtrées par `prefix`.
    QVariantMap completions(const QString& prefix) const;

    /// Arrête le process REPL proprement (signal de sortie au driver, puis
    /// kill si nécessaire) et joint le thread worker.
    QVariantMap stop();

    QVariantMap status() const;

signals:
    /// Émis (sur le thread de ce QObject, via invokeMethod queued) quand une
    /// ligne envoyée via sendLine termine — utile pour le frontend Qt réel
    /// (QWebChannel relaie les signaux, contrairement au pipe d'automatisation).
    /// `result` contient "requestId" (même convention que
    /// luaScriptExecutionFinished, pas un paramètre de signal séparé — le
    /// wrapper TypeScript QWebChannelSignal<T> ne modélise qu'un seul payload).
    void lineFinished(const QVariantMap& result);

private:
    struct HistoryEntry {
        int requestId{0};
        QString line;
        QString output;
        QString error;
        bool finished{false};
        qint64 startedAtMs{0};
        qint64 finishedAtMs{0};
    };

    void workerLoop(QString luaPath, QString driverPath, QString helperDir, QString pipeName, int lineTimeoutMs);
    QVariantMap historyEntryToVariant(const HistoryEntry& entry) const;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::pair<int, QString>> m_pendingLines;
    QHash<int, HistoryEntry> m_results;
    QList<int> m_historyOrder;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::thread m_worker;
    int m_nextRequestId{1};

    QString m_luaPath;
    QString m_helperPath;
    QString m_driverPath;
};

} // namespace killengine
