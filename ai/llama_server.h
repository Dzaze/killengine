#pragma once

#include <QByteArray>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <functional>

namespace killai {

// PHASE (08/09/2026, goulot d'etranglement) : partage entre llama_server.cpp
// (-c passe a llama-server) et llama_runtime.cpp (-c passe au fallback
// llama-cli, qui jusqu'ici n'en passait AUCUN et retombait donc sur le
// contexte natif max du modele Qwen3.5 -- 262144 tokens. Mesure live sur
// cette machine (i5-8250U) : ce defaut alloue a froid un buffer KV de 3072
// Mio (allocation+premier touche = 585ms) plus la reservation du sous-systeme
// prompt-cache/slot associee (~1s de plus), pour un prompt qui tient dans
// quelques milliers de tokens -- explique l'essentiel du "user time quasi nul
// mais plusieurs secondes d'attente" observe sur llama-cli. 8192 couvre le
// prompt actuel (methodologie + 58 outils, ~14 000 caracteres statiques,
// ~3600-4100 tokens estimes) + generation (jusqu'a 512 tokens) avec de la
// marge, pour un cout memoire trivial (~96 Mio de KV cache).
constexpr int kLlamaContextSize = 8192;

/// Resultat d'une completion servie par le llama-server persistant.
struct LlamaServerCompletion {
    bool success{false};
    QString content;
    QString errorMessage;
    bool fromServer{false};
};

/**
 * @brief Serveur llama.cpp persistant (llama-server.exe).
 *
 * Charge le modele UNE seule fois puis sert les requetes suivantes via HTTP
 * local, au lieu de relancer un processus llama-cli (et de recharger le modele
 * GGUF) a chaque message utilisateur.
 *
 * Points cles:
 * - Demarrage paresseux au premier usage IA, redemarrage auto si le process meurt.
 * - `cache_prompt` active cote requete: le prefixe statique des prompts
 *   KillEngine (systeme + regles + outils) n'est re-encode qu'une fois.
 * - Timeout bornes partout (completion, health, startup) pour ne jamais
 *   bloquer l'UI indefiniment.
 * - Desactive via KILLENGINE_DISABLE_LLAMA_SERVER=1 ou ai/useServer=false:
 *   LlamaRuntime retombe alors sur le chemin llama-cli historique.
 */
class LlamaServer {
public:
    /// Instance partagee (le serveur survit aux requetes et aux AIEngine).
    static LlamaServer& instance();

    /// Localise llama-server.exe (env KILLENGINE_LLAMA_SERVER puis chemins connus).
    static QString locateExecutable();

    /// Construit le corps JSON de la requete POST /completion (pur, testable).
    static QByteArray buildCompletionRequest(const QString& prompt, int nPredict, const QStringList& stop);

    /// Extrait le champ "content" d'une reponse /completion (pur, testable).
    static QString parseCompletionContent(const QByteArray& body);

    LlamaServer();
    ~LlamaServer();

    LlamaServer(const LlamaServer&) = delete;
    LlamaServer& operator=(const LlamaServer&) = delete;

    /// Configure le modele et l'executable a utiliser (appels repetes sans effet de bord).
    void setModel(const QString& modelPath, const QString& executablePath);

    /// Demarre (ou reutilise) le serveur. Bloquant au premier demarrage
    /// (chargement du modele), quasi instantane ensuite.
    bool ensureRunning(QString* error = nullptr);

    /// Le processus serveur est-il vivant ?
    bool isRunning() const;

    /// Completion bloquantee bornee. Timeout: KILLENGINE_LLAMA_SERVER_TIMEOUT_MS (defaut 15 s).
    /// onStage (optionnel) : notifie "loadingModel" avant ensureRunning() (le
    /// cout de demarrage a froid, jusqu'a ~90s) puis "warmingPrompt" juste
    /// avant la requete HTTP -- utilise par la fenetre de prechauffage au
    /// demarrage (AIEngine::warmupLocalModel) pour une progression honnete.
    LlamaServerCompletion complete(const QString& prompt, int nPredict, const QStringList& stop,
                                    const std::function<void(const QString&)>& onStage = {});

    /// Tue le serveur (appele aussi sur aboutToQuit de l'application).
    void shutdown();

    int port() const { return m_port; }

private:
    bool healthCheck(int timeoutMs);
    bool startAndWait(QString* error);
    void connectQuitHook();

    QString m_modelPath;
    QString m_executablePath;
    QProcess m_process;
    QByteArray m_lastServerError;
    int m_port{0};
    bool m_quitHookConnected{false};
};

} // namespace killai