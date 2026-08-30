#pragma once

#include "process/process_handle.h"
#include "scanner/scan_types.h"

#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace killengine {

class ApplicationController;

class ProfileManager {
public:
    explicit ProfileManager(ApplicationController& controller);

    QVariantMap saveProfileTarget(const QString& profileName, const QString& targetName, const QString& addressHex, const QString& valueType, const QString& description);
    QVariantMap saveClrFieldProfileTarget(const QString& profileName, const QString& targetName, const QString& typeSubstring, const QString& identityField, const QString& identityValue, const QString& targetField, const QString& valueType, const QString& description);
    QVariantList listProfiles();
    QVariantMap loadProfile(const QString& profileName);
    bool deleteProfile(const QString& profileName);
    QVariantMap resolveProfileTarget(const QString& profileName, const QString& targetName);
    QVariantMap comparePointerMapAcrossRestart(const QString& profileName);
    QVariantMap exportPointerMap(const QString& profileName);
    QVariantMap importPointerMap(const QString& profileName, const QString& pointerMapJson, const QVariantMap& options);
    QVariantMap setProfileTargetDependencies(const QString& profileName, const QString& targetName, const QVariantList& dependencyNames);
    QVariantMap exportGhidraArtifacts(const QString& profileName);
    QVariantMap importGhidraSymbols(const QString& profileName, const QString& symbolsText);
    QVariantMap activateProfileTarget(const QString& profileName, const QString& targetName);
    QVariantMap saveProfileCodePatch(const QString& profileName, const QString& patchName, const QString& addressHex, const QString& aobPattern, const QString& patchBytes, const QVariantMap& metadata);
    QVariantMap applyProfileCodePatch(const QString& profileName, const QString& patchName);
    QVariantMap restoreProfileCodePatch(const QString& profileName, const QString& patchName);
    QVariantMap applyAllProfileCodePatches(const QString& profileName);
    QVariantMap restoreAllProfileCodePatches(const QString& profileName);
    QVariantMap inspectProfileCodePatches(const QString& profileName);
    QVariantMap saveProfileAutoAsmScript(const QString& profileName, const QString& scriptName, const QString& scriptText, const QVariantMap& metadata);
    QVariantMap applyProfileAutoAsmScript(const QString& profileName, const QString& scriptName);
    QVariantMap deleteProfileAutoAsmScript(const QString& profileName, const QString& scriptName);
    QVariantMap saveProfileLuaScript(const QString& profileName, const QString& scriptName, const QString& scriptText, const QVariantMap& metadata);
    QVariantMap deleteProfileLuaScript(const QString& profileName, const QString& scriptName);
    QVariantMap scanPointerChains(const QString& addressHex, const QVariantMap& scanOptions);
    QVariantMap resolvePointerChain(const QVariantMap& chain);
    QVariantMap suggestStableLocatorForAddress(const QString& addressHex, const QVariantMap& options);
    QVariantMap savePointerChainProfileTarget(const QString& profileName, const QString& targetName, const QVariantMap& chain, const QString& valueType, const QString& description);

private:
    ApplicationController& m_controller;
};

} // namespace killengine
