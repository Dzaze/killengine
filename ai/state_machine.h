#pragma once

#include <QObject>
#include <QString>

namespace killai {

enum class AIState { Idle, ProcessSelected, FirstScanRunning, CandidatesFound, WaitingForUserChange, Refining, Watching, TargetProbable, TargetConfirmed, ValueEdited, ProfileSaved };

QString aiStateToString(AIState state);

class StateMachine : public QObject {
    Q_OBJECT
public:
    explicit StateMachine(QObject* parent = nullptr);

    AIState currentState() const;
    QString currentStateName() const;
    void setState(AIState state);
    void reset();

signals:
    void stateChanged(const QString& state);

private:
    AIState m_state{AIState::Idle};
};

} // namespace killai
