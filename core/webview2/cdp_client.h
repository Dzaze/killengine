#pragma once

#include <QObject>
#include <QWebSocket>
#include <QJsonObject>
#include <QJsonArray>
#include <functional>
#include <memory>

namespace killcore {

/**
 * @brief Client Chrome DevTools Protocol (CDP) pour WebView2.
 *
 * Se connecte au endpoint WebSocket d'un WebView2 et permet d'exécuter
 * des commandes CDP (Runtime.evaluate, DOM.querySelector, etc.).
 */
class CdpClient : public QObject {
    Q_OBJECT

public:
    using CommandCallback = std::function<void(const QJsonObject& result, const QString& error)>;

    explicit CdpClient(QObject* parent = nullptr);
    ~CdpClient();

    /**
     * @brief Se connecte au WebSocket CDP.
     * @param wsUrl URL WebSocket (ex: ws://127.0.0.1:9333/devtools/browser/...)
     * @return true si la connexion est initiée (async)
     */
    bool connectTo(const QString& wsUrl);

    /**
     * @brief Vérifie si le client est connecté.
     */
    bool isConnected() const;

    /**
     * @brief Envoie une commande CDP.
     * @param method Méthode CDP (ex: "Runtime.evaluate")
     * @param params Paramètres de la méthode
     * @param callback Callback appelé avec le résultat ou une erreur
     * @return ID de la commande (pour corrélation), ou 0 si échec
     */
    int sendCommand(const QString& method, const QJsonObject& params, CommandCallback callback);

    /**
     * @brief Envoie une commande CDP (synchrone, bloquant).
     * @param method Méthode CDP
     * @param params Paramètres
     * @param timeoutMs Timeout en millisecondes
     * @return Résultat ou objet avec champ "error"
     */
    QJsonObject sendCommandSync(const QString& method,
                                const QJsonObject& params = QJsonObject(),
                                int timeoutMs = 5000);

    /**
     * @brief Active un domaine CDP pour recevoir ses événements.
     * @param domain Nom du domaine (ex: "Runtime", "DOM", "Network")
     */
    bool enableDomain(const QString& domain);

    /**
     * @brief Désactive un domaine.
     */
    bool disableDomain(const QString& domain);

    /**
     * @brief Évalue une expression JavaScript dans le contexte de la page.
     * @param expression Code JS à exécuter
     * @param returnByValue Si true, retourne la valeur sérialisée
     * @return Résultat de l'évaluation
     */
    QJsonObject evaluateJavaScript(const QString& expression, bool returnByValue = true);

    /**
     * @brief Récupère les propriétés d'un objet distant.
     * @param objectId ID de l'objet (RemoteObject.objectId)
     * @param ownProperties Si true (défaut), ne remonte que les propriétés propres de
     *        l'objet, sans remonter la chaîne de prototypes (utile pour éviter des
     *        centaines de méthodes héritées lors du sondage d'un objet global type window).
     * @return Propriétés de l'objet
     */
    QJsonObject getObjectProperties(const QString& objectId, bool ownProperties = true);

    /**
     * @brief Déconnecte le client.
     */
    void disconnect();

signals:
    void connected();
    void disconnected();
    void error(const QString& message);

    /**
     * @brief Événement CDP reçu (pour les domaines activés).
     * @param method Méthode de l'événement (ex: "Runtime.consoleAPICalled")
     * @param params Paramètres de l'événement
     */
    void eventReceived(const QString& method, const QJsonObject& params);

private slots:
    void onConnected();
    void onDisconnected();
    void onError(QAbstractSocket::SocketError error);
    void onTextMessageReceived(const QString& message);

private:
    std::unique_ptr<QWebSocket> m_socket;
    int m_nextCommandId{1};
    QHash<int, CommandCallback> m_pendingCommands;
    QString m_wsUrl;
    bool m_connected{false};
};

/**
 * @brief Découvre les pages CDP disponibles via l'endpoint HTTP /json.
 * @param httpUrl URL HTTP (ex: http://127.0.0.1:9333/json)
 * @return Liste des pages avec leurs URLs WebSocket, ou vide si échec
 */
QJsonArray discoverCdpPages(const QString& httpUrl, bool pageTargetsOnly = true);

/**
 * @brief Découvre les pages CDP avec fallback WebView2 UWP via Windows Device Portal.
 * @param directHttpUrl Endpoint direct (ex: http://127.0.0.1:9333/json)
 * @param statusMessage Message lisible sur le chemin retenu ou l'étape manquante
 * @param pageTargetsOnly Si true, ne remonte que les targets CDP de type "page"
 * @return Liste aplatie des pages/targets CDP
 */
QJsonArray discoverCdpPagesWithFallback(const QString& directHttpUrl,
                                        QString* statusMessage = nullptr,
                                        bool pageTargetsOnly = true);

/**
 * @brief Trouve l'URL WebSocket d'une page par son titre ou URL.
 * @param httpUrl URL HTTP de l'endpoint /json
 * @param pageTitle Titre de la page à chercher (partiel match)
 * @param pageUrl URL de la page à chercher (partiel match)
 * @return URL WebSocket ou QString() si non trouvée
 */
QString findCdpWebSocketUrl(const QString& httpUrl,
                            const QString& pageTitle = QString(),
                            const QString& pageUrl = QString(),
                            bool pageTargetsOnly = true);

} // namespace killcore
