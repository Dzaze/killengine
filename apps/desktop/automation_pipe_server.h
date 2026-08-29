#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QLocalServer>
#include <QObject>
#include <QString>
#include <QVariantMap>

class QLocalSocket;

namespace killengine {

class ApplicationController;

/// Connecteur d'automatisation local (demandé explicitement le 19/08/2026 :
/// pilotage de KillEngine en temps réel par un agent IA pendant une session de
/// test manuelle sur un vrai jeu). Expose TOUTE la surface Q_INVOKABLE
/// d'ApplicationController via un named pipe local (QLocalServer -> Windows
/// named pipe), pour qu'un script/agent externe puisse piloter le moteur en
/// JSON-RPC minimal pendant qu'un humain observe/agit en parallèle dans l'UI
/// Qt — même processus, même état partagé (m_attached, m_candidates, etc.),
/// pas une deuxième implémentation du moteur.
///
/// Désactivé par défaut : n'écoute que si la variable d'environnement
/// KILLENGINE_AUTOMATION_PIPE=1 est présente au démarrage (voir main.cpp).
/// Aucune couche d'authentification : le named pipe est déjà local-machine-only
/// par construction Windows — accepté comme risque pour un outil dev/test qui
/// ne doit jamais être activé dans un build livré à l'utilisateur final.
///
/// Protocole : une ligne JSON par requête/réponse (newline-delimited) :
///   -> {"id":1,"method":"attachProcess","params":[12345]}
///   <- {"id":1,"result":true}
/// "method" doit correspondre exactement au nom d'une méthode Q_INVOKABLE
/// d'ApplicationController ; "params" est un tableau JSON positionnel converti
/// vers les types Qt attendus via QVariant::convert (réflexion QMetaMethod à
/// l'exécution, pas de wrapper écrit à la main par méthode — une nouvelle
/// méthode Q_INVOKABLE devient automatiquement appelable ici sans y toucher).
///
/// IMPORTANT — bypass volontaire du RiskGate : les confirmations (write/freeze/
/// debug/patch) vivent côté frontend (confirmRiskAction, ui/src/stores/app.ts),
/// pas dans ApplicationController. Un appel via ce pipe exécute donc l'action
/// IMMÉDIATEMENT, sans dialogue de confirmation. C'est le comportement voulu
/// pour un pilotage automatisé (l'agent est celui qui décide), mais ce n'est
/// pas "l'UI sans les boutons" : c'est un accès direct au moteur, à activer
/// uniquement pendant une session de test supervisée.
class AutomationPipeServer : public QObject {
    Q_OBJECT

public:
    explicit AutomationPipeServer(ApplicationController* controller, QObject* parent = nullptr);

    /// Nom du pipe local (\\.\pipe\<name> sous Windows via QLocalServer).
    static QString pipeName();

    /// Démarre l'écoute. Retourne false (avec message loggé) si le pipe est
    /// déjà pris par une autre instance de KillEngine.
    bool start();

    /// Arrête l'écoute (ferme le QLocalServer) sans détruire l'instance --
    /// permet une désactivation en direct depuis le mode Automation (Settings)
    /// sans redémarrer KillEngine.
    void stop();

    /// Statut exposé au mode Automation (Settings) : nom du pipe, écoute
    /// active, et compteur d'activité (dernier appel/méthode) pour que
    /// l'utilisateur voie que le pipe sert réellement, pas juste "activé".
    QVariantMap status() const;

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();

private:
    void handleLine(QLocalSocket* socket, const QByteArray& line);
    void writeResponse(QLocalSocket* socket, const QByteArray& json);

    ApplicationController* m_controller;
    QLocalServer m_server;
    QHash<QLocalSocket*, QByteArray> m_buffers;
    int m_callCount = 0;
    QString m_lastMethod;
    QDateTime m_lastCallAt;
};

} // namespace killengine
