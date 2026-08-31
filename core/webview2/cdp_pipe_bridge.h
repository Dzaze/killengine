#pragma once

#include <QObject>
#include <QLocalSocket>
#include <QJsonObject>
#include <QJsonArray>
#include <functional>
#include <memory>

namespace killcore {

/**
 * @brief Bridge CDP via pipe nommé Windows.
 *
 * Contourne le blocage AppContainer/WFP en utilisant un canal de communication
 * local (named pipe) au lieu de TCP. Un helper injecté dans le processus cible
 * relaie les messages CDP entre le pipe et le WebView2.
 */
class CdpPipeBridge : public QObject {
    Q_OBJECT

public:
    using CommandCallback = std::function<void(const QJsonObject& result, const QString& error)>;

    explicit CdpPipeBridge(QObject* parent = nullptr);
    ~CdpPipeBridge();

    /**
     * @brief Crée et écoute sur un pipe nommé.
     * @param pipeName Nom du pipe (ex: "KillEngineCdpBridge")
     * @return true si le pipe est créé avec succès
     */
    bool createServer(const QString& pipeName);

    /**
     * @brief Se connecte à un pipe existant (mode client).
     * @param pipeName Nom du pipe
     * @return true si connecté
     */
    bool connectToServer(const QString& pipeName);

    /**
     * @brief Vérifie si le bridge est connecté.
     */
    bool isConnected() const;

    /**
     * @brief Envoie une commande CDP via le pipe.
     * @param method Méthode CDP
     * @param params Paramètres
     * @param callback Callback avec le résultat
     * @return ID de la commande
     */
    int sendCommand(const QString& method, const QJsonObject& params, CommandCallback callback);

    /**
     * @brief Envoie une commande CDP (synchrone).
     * @param method Méthode CDP
     * @param params Paramètres
     * @param timeoutMs Timeout
     * @return Résultat ou objet avec erreur
     */
    QJsonObject sendCommandSync(const QString& method, const QJsonObject& params, int timeoutMs = 5000);

    /**
     * @brief Active un domaine CDP pour recevoir ses événements.
     */
    bool enableDomain(const QString& domain);

    /**
     * @brief Désactive un domaine.
     */
    bool disableDomain(const QString& domain);

    /**
     * @brief Évalue une expression JavaScript.
     */
    QJsonObject evaluateJavaScript(const QString& expression, bool returnByValue = true);

    /**
     * @brief Ferme la connexion.
     */
    void disconnect();

    /**
     * @brief Génère le nom complet du pipe pour un processus donné.
     * @param processId PID du processus cible
     * @return Nom complet du pipe (ex: "\\\\.\\pipe\\KillEngineCdpBridge_1234")
     */
    static QString makePipeNameForProcess(quint32 processId);

    /**
     * @brief Vérifie si un pipe existe et est accessible.
     * @param pipeName Nom du pipe
     * @return true si le pipe existe
     */
    static bool pipeExists(const QString& pipeName);

signals:
    void connected();
    void disconnected();
    void error(const QString& message);

    /**
     * @brief Événement CDP reçu du pipe.
     */
    void eventReceived(const QString& method, const QJsonObject& params);

    /**
     * @brief Un client s'est connecté au pipe serveur.
     */
    void clientConnected();

private slots:
    void onConnected();
    void onDisconnected();
    void onError(QLocalSocket::LocalSocketError error);
    void onReadyRead();

private:
    std::unique_ptr<QLocalSocket> m_socket;
    int m_nextCommandId{1};
    QHash<int, CommandCallback> m_pendingCommands;
    QString m_pipeName;
    bool m_connected{false};
    bool m_isServer{false};

    // Buffer pour les messages incomplets
    QByteArray m_readBuffer;

    void processMessage(const QByteArray& data);
    void sendMessage(const QJsonObject& message);
};

/**
 * @brief Lance un helper injecté qui crée le bridge CDP vers pipe.
 *
 * Cette fonction injecte un helper DLL dans le processus cible qui :
 * 1. Se connecte au WebView2 CDP via localhost:9333
 * 2. Crée un pipe nommé local
 * 3. Relaie les messages entre les deux
 *
 * @param processId PID du processus cible (Solitaire/SC2)
 * @param pipeName Nom du pipe à créer
 * @return true si l'injection a réussi
 */
bool launchCdpPipeHelper(quint32 processId, const QString& pipeName);

/**
 * @brief Vérifie si le helper pipe est actif pour un processus.
 * @param processId PID du processus
 * @return true si le pipe existe et répond
 */
bool isCdpPipeHelperActive(quint32 processId);

} // namespace killcore
