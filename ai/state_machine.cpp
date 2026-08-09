#include "state_machine.h"

namespace killai {

QString aiStateToString(AIState state) {
    switch (state) {
        case AIState::Idle: return "Idle";
        case AIState::ProcessSelected: return "ProcessSelected";
        case AIState::FirstScanRunning: return "FirstScanRunning";
        case AIState::CandidatesFound: return "CandidatesFound";
        case AIState::WaitingForUserChange: return "WaitingForUserChange";
        case AIState::Refining: return "Refining";
        case AIState::Watching: return "Watching";
        case AIState::TargetProbable: return "TargetProbable";
        case AIState::TargetConfirmed: return "TargetConfirmed";
        case AIState::ValueEdited: return "ValueEdited";
        case AIState::ProfileSaved: return "ProfileSaved";
    }
    return "Unknown";
}

StateMachine::StateMachine(QObject* parent)
    : QObject(parent) {
}

AIState StateMachine::currentState() const {
    return m_state;
}

QString StateMachine::currentStateName() const {
    return aiStateToString(m_state);
}

void StateMachine::setState(AIState state) {
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged(currentStateName());
}

void StateMachine::reset() {
    setState(AIState::Idle);
}

} // namespace killai
