#pragma once

#include "activity/activity_registry.h"

#include <QObject>
#include <QString>
#include <QVariantMap>

namespace killengine {

class ApplicationController;

/**
 * @brief UX-PRODUIT-12 — Centre d'activité permanent.
 *
 * Glue Qt fine autour de killcore::ActivityRegistry (pure logique, testée en
 * GTest) : porte le signal activityUpdated et convertit les entrées en
 * QVariantMap pour le pont QWebChannel. Ne connaît RIEN des scans/Timeline/
 * Lua/installations eux-mêmes -- chaque manager producteur appelle
 * registry() directement puis notifyUpdated() ; le routage d'annulation par
 * kind vit dans ApplicationController::cancelActivity (qui a, lui, accès aux
 * autres managers), pas ici, pour ne dépendre d'aucun autre manager.
 *
 * IMPORTANT thread-safety : voir le commentaire en tête de
 * core/activity/activity_registry.h. Toutes les méthodes ci-dessous doivent
 * être appelées depuis le thread Qt (appel Q_INVOKABLE direct, ou
 * continuation déjà marshalée via QMetaObject::invokeMethod(...,
 * Qt::QueuedConnection) -- jamais depuis un worker thread brut).
 */
class ActivityManager : public QObject {
    Q_OBJECT
public:
    explicit ActivityManager(ApplicationController& controller, QObject* parent = nullptr);

    killcore::ActivityRegistry& registry();
    const killcore::ActivityRegistry& registry() const;

    /// Snapshot complet, autorité serveur : {globalRevision, entries:[...]}.
    QVariantMap getActivitySnapshot() const;

    /// À appeler par le producteur juste après une mutation du registre
    /// (beginActivity/updateProgress/finish/markCancelRequested) : émet
    /// activityUpdated avec CETTE entrée + la révision globale courante. Un
    /// operationId inconnu (évincé entre-temps) est silencieusement ignoré.
    void notifyUpdated(const QString& operationId);

signals:
    void activityUpdated(const QVariantMap& payload);

private:
    QVariantMap entryToVariantMap(const killcore::ActivityEntry& entry) const;

    ApplicationController& m_controller;
    killcore::ActivityRegistry m_registry;
};

} // namespace killengine
