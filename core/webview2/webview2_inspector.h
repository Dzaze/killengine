#pragma once

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariantMap>
#include <memory>
#include <functional>

namespace killcore {

class CdpClient;

/**
 * @brief Inspecteur WebView2 haut niveau pour KillEngine.
 *
 * Fournit une API simplifiée pour inspecter et interagir avec le contenu
 * WebView2 d'une application cible (comme SC2) via le protocole CDP.
 */
class WebView2Inspector : public QObject {
    Q_OBJECT

public:
    explicit WebView2Inspector(QObject* parent = nullptr);
    ~WebView2Inspector();

    /**
     * @brief Connecte à l'endpoint CDP d'un WebView2.
     * @param httpUrl URL HTTP de l'endpoint /json (ex: http://127.0.0.1:9333/json)
     * @param pageTitle Titre de la page à chercher (optionnel)
     * @param pageUrl URL de la page à chercher (optionnel)
     * @return true si connecté avec succès
     */
    bool connectToWebView(const QString& httpUrl, const QString& pageTitle = QString(), const QString& pageUrl = QString());

    /**
     * @brief Connecte directement à une URL WebSocket CDP connue.
     * @param wsUrl URL WebSocket CDP
     * @return true si connecté avec succès
     */
    bool connectToWebSocket(const QString& wsUrl);

    /**
     * @brief Vérifie si l'inspecteur est connecté.
     */
    bool isConnected() const;

    /**
     * @brief Déconnecte de l'endpoint CDP.
     */
    void disconnect();

    // =======================================================================
    // Inspection du DOM
    // =======================================================================

    /**
     * @brief Récupère le document root du DOM.
     * @return Objet document ou objet vide en cas d'erreur
     */
    QJsonObject getDocument();

    /**
     * @brief Recherche un élément par sélecteur CSS.
     * @param selector Sélecteur CSS (ex: "#minerals", ".resource-value")
     * @return NodeId de l'élément ou 0 si non trouvé
     */
    int querySelector(const QString& selector);

    /**
     * @brief Recherche tous les éléments correspondant à un sélecteur CSS.
     * @param selector Sélecteur CSS
     * @return Liste des NodeIds
     */
    QList<int> querySelectorAll(const QString& selector);

    /**
     * @brief Récupère le texte d'un élément.
     * @param nodeId NodeId de l'élément
     * @return Texte de l'élément ou QString() en cas d'erreur
     */
    QString getElementText(int nodeId);

    /**
     * @brief Récupère la valeur d'un attribut.
     * @param nodeId NodeId de l'élément
     * @param name Nom de l'attribut
     * @return Valeur de l'attribut ou QString() si non trouvé
     */
    QString getElementAttribute(int nodeId, const QString& name);

    /**
     * @brief Récupère les propriétés CSS calculées d'un élément.
     * @param nodeId NodeId de l'élément
     * @return Map des propriétés CSS
     */
    QVariantMap getComputedStyles(int nodeId);

    // =======================================================================
    // Exécution JavaScript
    // =======================================================================

    /**
     * @brief Évalue une expression JavaScript dans le contexte de la page.
     * @param expression Code JavaScript
     * @param returnByValue Si true, retourne la valeur sérialisée
     * @return Résultat de l'évaluation
     */
    QJsonObject evaluateJavaScript(const QString& expression, bool returnByValue = true);

    /**
     * @brief Exécute une fonction et retourne le résultat.
     * @param functionCode Code de la fonction (ex: "() => document.title")
     * @return Valeur retournée par la fonction
     */
    QVariant callFunction(const QString& functionCode);

    // =======================================================================
    // Inspection des ressources de jeu
    // =======================================================================

    /**
     * @brief Scanne la page à la recherche de valeurs numériques affichées.
     * @param value Valeur à chercher
     * @return Liste des éléments trouvés avec leur texte et position
     */
    QJsonArray findDisplayedValues(int value);

    /**
     * @brief Scanne la page à la recherche de texte.
     * @param text Texte à chercher
     * @return Liste des éléments trouvés
     */
    QJsonArray findDisplayedText(const QString& text);

    /**
     * @brief Récupère tous les éléments avec une classe CSS spécifique.
     * @param className Nom de la classe CSS
     * @return Liste des éléments
     */
    QJsonArray getElementsByClassName(const QString& className);

    /**
     * @brief Récupère tous les éléments avec un tag spécifique.
     * @param tagName Nom du tag (ex: "div", "span")
     * @return Liste des éléments
     */
    QJsonArray getElementsByTagName(const QString& tagName);

    // =======================================================================
    // Surveillance des changements
    // =======================================================================

    /**
     * @brief Active la surveillance des mutations DOM.
     * @return true si activé avec succès
     */
    bool enableDomMonitoring();

    /**
     * @brief Désactive la surveillance des mutations DOM.
     */
    void disableDomMonitoring();

    /**
     * @brief Active la surveillance de la console.
     * @return true si activé avec succès
     */
    bool enableConsoleMonitoring();

    /**
     * @brief Désactive la surveillance de la console.
     */
    void disableConsoleMonitoring();

    // =======================================================================
    // Informations sur la page
    // =======================================================================

    /**
     * @brief Récupère le titre de la page.
     * @return Titre de la page
     */
    QString getPageTitle();

    /**
     * @brief Récupère l'URL de la page.
     * @return URL de la page
     */
    QString getPageUrl();

    /**
     * @brief Récupère les métriques de performance de la page.
     * @return Métriques de performance
     */
    QJsonObject getPerformanceMetrics();

    // =======================================================================
    // Utilitaires
    // =======================================================================

    /**
     * @brief Attend que le DOM soit prêt.
     * @param timeoutMs Timeout en millisecondes
     * @return true si le DOM est prêt
     */
    bool waitForDomReady(int timeoutMs = 5000);

    /**
     * @brief Attend qu'un élément soit présent dans le DOM.
     * @param selector Sélecteur CSS de l'élément
     * @param timeoutMs Timeout en millisecondes
     * @return NodeId de l'élément ou 0 si timeout
     */
    int waitForElement(const QString& selector, int timeoutMs = 5000);

    /**
     * @brief Liste les pages CDP disponibles.
     * @param httpUrl URL HTTP de l'endpoint /json
     * @return Liste des pages avec leurs informations
     */
    static QJsonArray listAvailablePages(const QString& httpUrl);

signals:
    void connected();
    void disconnected();
    void error(const QString& message);

    /**
     * @brief Émis quand le DOM change (si monitoring activé).
     * @param mutations Liste des mutations
     */
    void domMutated(const QJsonArray& mutations);

    /**
     * @brief Émis quand un message console est reçu (si monitoring activé).
     * @param level Niveau du message (log, warn, error, etc.)
     * @param message Texte du message
     * @param source Source du message
     */
    void consoleMessage(const QString& level, const QString& message, const QString& source);

private slots:
    void onCdpEvent(const QString& method, const QJsonObject& params);

private:
    std::unique_ptr<CdpClient> m_client;
    bool m_domMonitoringEnabled = false;
    bool m_consoleMonitoringEnabled = false;

    // Cache des nodeIds pour éviter les requêtes répétées
    QHash<QString, int> m_selectorCache;

    void clearCache();
    int getNodeIdFromRemoteObject(const QJsonObject& remoteObject);
};

} // namespace killcore
