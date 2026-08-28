#pragma once

#include <QString>
#include <QStringList>

namespace killai {

QString firstDecimalOutsideHex(QString text);
QString firstHexAddressIn(const QString& text);

bool looksLikePureSocialQuery(const QString& query);
bool looksLikePureSocialQuery(
    const QString& query,
    const QStringList& knownNumbers,
    const QStringList& knownHexAddresses);

bool wantsTrainerQuery(const QString& query);
bool wantsFieldStabilityQuery(const QString& query);
bool wantsUiSourcesQuery(const QString& query);
bool wantsGenerateAobQuery(const QString& query);
bool wantsSuggestPatchQuery(const QString& query);
bool wantsDisassembleBackwardQuery(const QString& query);
bool wantsFindWhatWritesQuery(const QString& query);
bool wantsTestCandidateFieldsQuery(const QString& query);

bool wantsAobOrPatchWorkflowQuery(const QString& query);
bool wantsFindWhatWritesOrTestFieldsQuery(const QString& query);

QString investigationPlaybookTopic(const QString& query);
bool wantsInvestigationPlaybookQuery(const QString& query);

} // namespace killai
