#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QVariantMap>

#include <memory>

namespace killcore {
class JobObject;
}

namespace killengine {

/// UX-PRODUIT-15B — possède, côté instance NORMALE, le cycle de vie d'une
/// session tutoriel : un second KillEngine.exe lancé avec
/// --tutorial-session-root=<data/tutorial-sessions/<uuid>>, qui lui-même
/// possède sa propre cible KillEngineDemoTarget.exe (voir
/// ApplicationController::enterTutorialMode, rôle enfant). Un JobObject par
/// niveau de parenté -- celui-ci couvre normal->enfant KillEngine, pas
/// enfant->cible démo (qui est géré par l'enfant lui-même).
///
/// Une seule session à la fois par instance normale : start() sur une
/// session déjà active retourne l'état de la session existante sans en
/// relancer une seconde (pas de fenêtre orpheline supplémentaire).
class TutorialSessionManager : public QObject {
    Q_OBJECT
public:
    explicit TutorialSessionManager(QObject* parent = nullptr);
    ~TutorialSessionManager() override;

    /// Démarre une session tutoriel (ou retourne l'état de la session déjà
    /// active s'il y en a une). "success" à false + "error" si le process
    /// enfant n'a pas pu être lancé ou assigné au Job Object.
    QVariantMap start();

    /// État courant : "active" (bool), et si active "sessionId", "sessionRoot",
    /// "pid".
    QVariantMap status() const;

    /// Termine la session : arrêt gracieux (terminate) puis forcé (kill) si
    /// nécessaire, puis suppression du dossier de session -- uniquement après
    /// vérification que son chemin absolu est bien sous data/tutorial-sessions/.
    QVariantMap close();

private:
    bool isActive() const;

    QString m_sessionId;
    QString m_sessionRoot;
    std::unique_ptr<QProcess> m_childProcess;
    std::unique_ptr<killcore::JobObject> m_jobObject;
};

} // namespace killengine
