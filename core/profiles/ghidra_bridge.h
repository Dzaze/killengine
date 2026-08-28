#pragma once

#include "profile_store.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace killcore {

struct GhidraSymbolImportResult {
    int symbolsRead{0};
    int targetsUpdated{0};
    int patchesUpdated{0};
    int unmatched{0};
    QStringList messages;
};

/// Export lisible et versionné pour passer les artefacts runtime KillEngine vers Ghidra.
QJsonObject exportGhidraArtifacts(const Profile& profile);

/// Script Ghidra Python autonome : coller dans Script Manager, il pose labels/bookmarks/comments.
QString generateGhidraImportScript(const QJsonObject& artifactExport);

/// Importe un export Ghidra JSON ou CSV (module,offset,name,comment) et enrichit le profil.
bool importGhidraSymbols(Profile* profile, const QByteArray& data, GhidraSymbolImportResult* result, QString* error);

} // namespace killcore
