#pragma once

#include "scanner/scan_types.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <cstdint>

namespace killengine {

struct WriteRecord {
    uint64_t   address{0};
    QByteArray previousValue;
    QByteArray writtenValue;
    killcore::ValueType type{killcore::ValueType::Int32};
    QString valueText;
};

struct AutoWriteTarget {
    uint64_t address{0};
    killcore::ValueType type{killcore::ValueType::Int32};
    // Ecriture RiskGate (29/08/2026) : distingue une cible venant d'une
    // adresse tapee explicitement dans le chat (a gater derriere une
    // confirmation reelle) d'une cible issue du mode Auto/UI-string-trace/
    // profil (comportement de confiance deja etabli, ne jamais gater -- voir
    // commentaire de ApplicationController::rewriteLastAutoWriteTargets()).
    bool chatOrigin{false};
};

class AutoWriteStateAccess {
public:
    AutoWriteStateAccess(
        QList<WriteRecord>& writeHistory,
        QList<AutoWriteTarget>& lastTargets,
        QList<AutoWriteTarget>& chatTargets);

    int writeHistorySize() const;
    bool writeHistoryEmpty() const;
    const WriteRecord& writeHistoryAt(int index) const;
    void appendWriteRecord(const WriteRecord& record);
    void removeWriteHistoryAt(int index);

    bool hasLastTargets() const;
    int lastTargetCount() const;
    const QList<AutoWriteTarget>& lastTargets() const;
    QList<AutoWriteTarget>& lastTargets();
    void appendLastTarget(const AutoWriteTarget& target);
    void clearLastTargets();

    bool hasChatTargets() const;
    int chatTargetCount() const;
    const QList<AutoWriteTarget>& chatTargets() const;
    QList<AutoWriteTarget>& chatTargets();
    void appendChatTarget(const AutoWriteTarget& target);
    void clearChatTargets();
    void replaceChatTargetsWithLastTargets();

private:
    QList<WriteRecord>& m_writeHistory;
    QList<AutoWriteTarget>& m_lastTargets;
    QList<AutoWriteTarget>& m_chatTargets;
};

} // namespace killengine
