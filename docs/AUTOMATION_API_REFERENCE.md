# Référence exhaustive des réponses `Q_INVOKABLE`

> Généré par extraction directe du code source le 30/08/2026 (PHASE 238) — à revalider si le code change, ne pas copier-coller sans revérifier après un futur refactor. Voir `docs/AUTOMATION_API.md` pour le protocole général et les méthodes les plus courantes.

Ce document inventorie, pour **chaque** méthode `Q_INVOKABLE` de `ApplicationController` (`apps/desktop/application_controller.h`), la forme réelle de sa réponse (clés du `QVariantMap`, ou type scalaire), extraite en lisant l'implémentation réelle (le `.cpp` du manager délégué quand il y en a un, sinon `application_controller.cpp` directement). Aucune clé listée ici n'a été devinée : chaque entrée correspond à une ligne `result["clé"] = ...` (ou équivalent) effectivement lue dans le code au 30/08/2026.

Convention : `success`/`error` apparaissent dans la quasi-totalité des méthodes (booléen de statut + message d'erreur), ils ne sont donc rementionnés explicitement que lorsqu'il y a une nuance (absence, nom différent, etc.). Les méthodes qui délèguent à un objet non suffixé `*Manager` (`ClrInspectorBridge`, `UiStringInvestigator` = classe réelle de `display_string_investigator.h/.cpp`, `SaveFileInvestigator`) sont indiquées explicitement.

Certaines réponses sont **fortement dépendantes du chemin emprunté** (notamment `startSmartSearch` et `startAutoResolve`, des dizaines de branches) : pour celles-ci, la liste de clés couvre les cas les plus fréquents observés dans le code, pas une énumération à 100% exhaustive de toutes les branches — c'est noté explicitement à chaque fois.

---

## Gestion des profils (déjà documenté dans `docs/AUTOMATION_API.md`)

`saveProfileTarget`, `loadProfile`, `resolveProfileTarget`, `comparePointerMapAcrossRestart`, `deleteProfile`, `listProfiles` sont déjà documentées avec leur forme de réponse exacte dans `docs/AUTOMATION_API.md`, section "Gestion des profils" — non dupliquées ici. Le reste du cluster Profils (patches, scripts, dépendances, Ghidra, pointer maps) est documenté plus bas, section "Profils (suite)".

---

## Scan (mémoire, valeurs, UI strings)

Fichier source principal : `apps/desktop/scanning_core_manager.cpp` (délégué depuis `application_controller.cpp`). Les méthodes Trace UI string / fenêtre mémoire / diff de pages sont dans `apps/desktop/display_string_investigator.cpp` (classe `UiStringInvestigator`).

### `startExactScan(value, valueType)`
Fichier : `scanning_core_manager.cpp`
Réponse : `{ success, partial, cancelled, regionsScanned, bytesScanned, matchesFound, matchesReturned, error, matches: [{address, type}] (50 max), candidateStoreSize, elapsedMs, bytesPerSecond, matchesPerSecond }`

### `startExactScanMultiType(value, valueType)`
Réponse : identique à `startExactScan` + `variantCount`, et chaque entrée de `matches` porte aussi `confidence`/`variantLabel`.

### `startExactScanExpert(value, valueType, expertOptions)`
Réponse : identique à `startExactScan` (mêmes clés).

### `scanEncryptedValue(value, valueType, options)`
Réponse : `{ success, partial, matches: [{address, type, confidence, variantLabel, regionBase, protection, memoryType, writable}], matchesFound, matchesReturned, maxResults, regionsScanned, bytesScanned, elapsedMs, mode, key (hex), keySearchBits, error }`

### `startExactScanAsync(value, valueType, expertOptions)`
Réponse immédiate : `{ success, started, requestId, error }`. Résultat final livré via le signal Qt `scanFinished` avec les mêmes clés que `startExactScan` plus `requestId`, `candidateStoreFileBacked`, `candidateStoreBytes`, `candidateStoreMemoryBytes`.

### `cancelActiveScan()`
Réponse : `{ success, error }` (pas d'autre clé).

### `nextScan(mode, value)`
Réponse : `{ success, checked, unreadable, remaining, error, debugBeforeCount, debugMode, debugValue, diagnostic? (si remaining==0), stableGroupCycles?/stableGroupAddresses?/stableGroupHint? (si petit groupe stable détecté) }`

### `nextScanAsync(mode, value)`
Réponse immédiate : `{ success, started, requestId, error }`. Résultat final via signal `scanFinished`, mêmes clés que `nextScan` + `requestId`, `kind: "next_scan"`, `cancelled`, `streamInput`, `streamOutput`, `fileBacked`, `candidateStorePath`, `elapsedMs`, `candidatesPerSecond`, `candidateStoreBytes`, `candidateStoreMemoryBytes`.

### `undoCandidateScan()`
Réponse : `{ success, restored, count, fileBacked, candidateStorePath, candidateStoreBytes, candidateStoreMemoryBytes, error }`

### `getCandidates(pageIndex, pageSize, addressFilter)`
Réponse : `{ pageIndex, pageSize, totalCount, displaySuppressed, displayLimit, fileBacked, candidateStorePath, candidateStoreBytes, candidateStoreMemoryBytes, candidates: [{address, type, lastValueHex, lastValueNumber, confidence, variantLabel?, writeVerified}] }` — pas de `success` racine.

### `captureUnknownSnapshot()` / `captureUnknownSnapshotWithOptions(expertOptions)`
Réponse : `{ success, partial, cancelled, regionsCaptured, regionsSkipped, bytesCaptured, captureLimitBytes, captureLimitReached, compressedBytes, mappedStorage, writableOnly, executableOnly, copyOnWriteOnly, suggestedDepthMb, relevantBytes, autoDepthApplied, error }`

### `captureUnknownSnapshotAsync()` / `captureUnknownSnapshotAsyncWithOptions(expertOptions)`
Réponse immédiate : `{ success, started, requestId, error }`. Résultat final via `scanFinished`, mêmes clés que `captureUnknownSnapshotWithOptions` + `requestId`, `kind: "unknown_capture"`.

### `unknownNextScan(mode, valueType)`
Réponse : soit un raffinement direct sur les candidats existants (mêmes clés que `nextScan` + `kind: "unknown_refine"`, `refinedFromCandidates: true`, `checkedBytes`, `matchesFound`, `stored`), soit `{ success, partial, cancelled, checkedBytes, matchesFound, stored, valueType, typePasses: [{type, success, partial, checkedBytes, matchesFound, stored, error}], error, diagnostic? }`

### `unknownNextScanAsync(mode, valueType)`
Réponse immédiate ou directe selon le nombre de candidats (voir code) ; version async finale via `scanFinished` avec `kind: "unknown_next"` et les mêmes clés que `unknownNextScan`.

### `scanGroupScan(entries, options)`
Réponse : `{ success, partial, matches: [{address, type, confidence, variantLabel, regionBase, protection, memoryType, writable}], matchesFound, maxResults, regionsScanned, bytesScanned, elapsedMs, entriesCount, error }`

### `getMemoryMap()`
Réponse : `{ attached, processName?, pid?, regions: [{baseAddress, allocationBase, size, protection, state, type, readable, writable, executable, guarded}], stats: {regionCount, committedCount, readableCount, writableCount, executableCount, totalBytes, committedBytes, readableBytes, writableBytes, executableBytes} }`

### `readMemoryPreview(addressHex, size)` / `readMemoryBlock(addressHex, size)`
Réponse : `{ success, partial, cancelled, bytesRead, requestedBytes, error, hex }`

### `analyzeStructureMemory(addressHex, size)`
Réponse : `{ success, partial, cancelled, baseAddress, bytesRead, requestedBytes, fieldCount, fields: [{offset, address, type, value, valueText, rawHex, changed, label?}], error }`

### `inferStructureInstanceDelta(baseAddressAHex, fieldAddressAHex, baseAddressBHex, fieldAddressBHex, options)`
Fichier : `application_controller.cpp` (pas délégué). Ajoutée en PHASE 239 (Codex, 30/08/2026) — capturée puis re-vérifiée après commit (`75183a3`) : la forme initialement observée pendant le développement (non commité) manquait `candidateCount`, corrigé ici contre le code final. Preuve concrète que la mise en garde de ce document sur les instantanés figés n'est pas théorique.
Réponse : `{ success, error, warning, compatibleLayout, baseAddressA, baseAddressB, fieldAddressA, fieldAddressB, fieldOffsetA, fieldOffsetB, fieldOffsetDelta, instanceDelta, fieldAddressDelta, candidates: [{relativeIndex, baseAddress, fieldAddress, inputInstance}], candidateCount }`

### `analyzeFieldStability(addressHex, options)`
Réponse : `{ success, verdict ("no_writes_observed"|"insufficient_data"|"likely_derived_display"|"likely_event_driven"), writeCount, distinctInstructionCount, dominantInstructionPointer, dominantInstructionShare, meanIntervalMs, intervalCoefficientOfVariation, rationale, warning, error }`

### `readAttachedWindowText(options)`
Réponse : `{ success, windows: [{hwnd, pid, pidMatchesAttached, visible, text, className, x, y, width, height, children: [{hwnd, text, className, x, y, width, height}], childCount}], windowCount, includeAllVisible, titleContains, error }`

### `readUiAutomationTree(options)`
Réponse : `{ success, elements: [{name, value, controlType, x, y, width, height}], elementCount, scannedCount, hwnd, rootName, rootControlType, findAllHr, error }` (Windows uniquement)

### `scanUiStrings(value, options)`
Fichier : `display_string_investigator.cpp` (`UiStringInvestigator`)
Réponse : `{ success, partial, matches: [{address, encoding, text, byteLength, bytesHex, regionBase, regionSize, protection, memoryType, writable}], matchesFound, matchesReturned, maxResults, regionsScanned, bytesScanned, elapsedMs, writableOnly, error }`

### `trackUiStringCandidates(candidates, value)`
Réponse : `{ success, checked, unreadable, moved, remaining, survivors: [même forme que matches + movedFrom?/movedDistanceBytes?], error }`

### `analyzeUiStringSources(stringCandidate, value, options)`
Réponse : `{ success, partial, candidates: [{address, type, confidence, variantLabel, lastValueHex, lastValueNumber, distanceBytes, offsetFromString, regionBase, protection, memoryType}], matchesFound, matchesReturned, bytesScanned, windowStart, windowEnd, radiusBytes, error }`

### `scanMemoryWindow(addressHex, value, options)`
Réponse : `{ success, candidates: [{address, type, variantLabel, lastValueHex, lastValueNumber, distanceBytes, offsetFromAnchor, regionBase, protection, memoryType}], matchesFound, matchesReturned, bytesScanned, windowStart, windowEnd, radiusBytes, error }`

### `trackUiStringSources(sourceCandidates, value)`
Réponse : `{ success, checked, unreadable, incompatible, remaining, survivors: [... + previousValueNumber, lastValueNumber, lastValueHex, expectedHex, trackHits, confidence], error }`

### `inspectUiStringOrigins(stringCandidates, options)`
Réponse : `{ success, targetCount, targets: [{address, offsetFromCluster, regionBase?, protection?, memoryType?}], clusterStart, clusterEnd, clusterSpanBytes, commonStrideBytes, pointerRefs: [{address, pointsTo, nearestString, distanceToString, regionBase, protection, memoryType, writable}], pointerRefsFound, bytesScanned, regionsScanned, partial, error }`

### `startUiStringInvestigation(stringCandidates, sourceCandidates, options)`
Réponse : `{ success, windows, bytesCaptured, probeBlocks, probeBytesCaptured, probeRegions, probeUnreadable, unreadable, radiusBytes, error }`

### `finishUiStringInvestigation(options)`
Réponse : `{ success, windowsChecked, capturedWindows, unreadable, changedBytes, changesFound, changes: [{address, offset, length, label, reason, beforeHex, afterHex, beforeInt32?, afterInt32?, beforeFloat32?, afterFloat32?}], globalValueHits: [{address, type, variantLabel, confidence, lastValueHex, lastValueNumber, regionBase, protection, memoryType, origin}], globalValueHitsFound, probeBlocksCaptured, probeBlocksChecked, probeBlocksChanged, probeBytesChecked, probeChangedBytes, probeUnreadable, partial, elapsedMs, error }`

### `startChangedPagesDiff(options)`
Réponse : `{ success, blocksCaptured, bytesCaptured, regionsScanned, unreadable, partial, maxBytes, blockSize, privateOnly, writableOnly, error }`

### `finishChangedPagesDiff(previousValue, currentValue, options)`
Réponse : `{ success, previousValue, currentValue, hits: [{address, type, variantLabel, previousValue, currentValue, lastValueHex, lastValueNumber, previousAtSameOffset, distanceToChangedBytes, confidence, origin, regionBase, protection, memoryType}], hitsFound, capturedBlocks, blocksChecked, blocksChanged, bytesChecked, changedBytes, unreadable, partial, error }`

---

## Write / Freeze

Fichier source : `apps/desktop/write_freeze_core_manager.cpp` (écritures/rollback), `apps/desktop/freeze_hotkey_overlay_manager.cpp` (freeze polling/hotkeys/overlay), `apps/desktop/debug_feature_manager.cpp` (freeze par breakpoint matériel/in-process).

### `writeMemoryValue(addressHex, valueType, value)`
Réponse : `{ success, verified, protectionChanged, bytesWritten, error }`

### `writeMemoryValuesWithVariants(targets, value)`
Réponse : `{ success, verified, protectionChanged, protectionChangedCount, bytesWritten (toujours 0, champ legacy), written, total, results: [{success, verified, protectionChanged, bytesWritten, error, address, type, variantLabel, displayValue, encodedHex}], error }`

### `writeMemoryValuesAtomic(targets, options)`
Réponse : `{ success, results: [{address, type, success, verified, protectionChanged, bytesWritten, error}], written, total, suspendedThreadCount, parseErrors? }`

### `writeMemoryValueConfirmed(addressHex, valueType, value, persistHistory)` — pas Q_INVOKABLE (interne), mais réutilisé partout : `{ success, verified, confirmationMode: true, temporaryVerified, restoredBeforeFinal, finalVerified, temporaryProtectionChanged, bytesWritten, restoreProtectionChanged, protectionChanged, error }`

### `rollbackLastWriteBatch()`
Réponse : `{ success, rolledBack, total, restoredWrites: [{address, type, from, to, success, verified, protectionChanged}], error }`

### `rollbackLastWrite()`
Réponse : `{ success, verified, protectionChanged, bytesWritten, error }`

### `setFreezeValue(addressHex, valueType, value, enabled)`
Fichier : `freeze_hotkey_overlay_manager.cpp`
Réponse : `{ success, enabled, error, warning? (adresse jamais write-verified) }`

### `freezeWithBreakpoint(addressHex, valueType, value, options)`
Fichier : `debug_feature_manager.cpp`
Réponse : `{ success, enabled, address, type, bytesWritten (0), verified (false), breakpointSize, freezeMode, mode: "breakpoint", error }`

### `escalatePollingFreezeToBreakpoint(addressHex)`
Réponse : identique à `freezeWithBreakpoint` (même fonction interne `activateBreakpointFreezeFor`).

### `stopBreakpointFreeze()`
Réponse : `{ success, enabled: false, mode: "breakpoint", hits?, rewrites?, blocks?, errors? }` (les stats n'apparaissent que si un freeze BP était actif).

### `getBreakpointFreezeStats()`
Réponse : `{ active, mode: "breakpoint", hits, rewrites, blocks, errors, healthy }`

### `setFreezeInterval(intervalMs)`
Réponse : `{ success, intervalMs, wasActive }`

### `registerGlobalHotkey(combo, action)`
Réponse : `{ success, combo, error, id?, type?, targetId? }`

### `unregisterGlobalHotkey(id)`
Réponse : `{ success, id, error? }`

### `getGlobalHotkeys()`
Réponse : `{ success, hotkeys: [{index, combo, targetId, label, payload}] }`

### `clearGlobalHotkeys()`
Réponse : `{ success }` (pas d'`error`)

### `setTrainerOverlayVisible(visible, options)`
Réponse : `{ success, visible }`

### `updateTrainerOverlay(state)`
Réponse : `{ success, lineCount?, error? }`

### `writeMemoryHex(addressHex, hexString)`
Fichier : `application_controller.cpp` (pas délégué)
Réponse : `{ success, verified, address, bytesWritten, requestedBytes, protectionChanged, previousHex, newHex, error }`

### `dumpMemoryRegion(addressHex, size, fileName)`
Réponse : `{ success, address, size, partial, filePath, fileName, error }`

### `forceWriteInstructionValue(ripHex, instructionLength, memBaseRegister, memDisplacement, valueType, value)`
Réponse : celle d'`executeAutoAssemblerScript` (voir section Lua/AutoAsm) + `generatedScript`, `targetAddress`.

### `startInProcessBreakpointFreeze(addressHex, valueType, value, options)`
Fichier : `debug_feature_manager.cpp`
Réponse : `{ success, enabled, mode: "inprocess", armedThreadCount, warning, error }`

### `stopInProcessBreakpointFreeze()`
Réponse : `{ success: true, enabled: false, mode: "inprocess", hits?, rewrites? }`

### `getInProcessBreakpointFreezeStats()`
Réponse : `{ mode: "inprocess", active, hits, rewrites, armedThreadCount, healthy? }`

---

## Trainer

Il n'existe **pas** de méthode `Q_INVOKABLE` C++ dédiée à la CRUD des features Trainer (création/suppression/liste/application) : cette logique vit côté frontend (`ui/src/stores/trainer.ts`, Pinia) et n'est exposée à l'automatisation que via la liste blanche `callVueStoreAction` (voir section Chat / Smart Search). Les seules méthodes C++ propres au Trainer sont l'overlay (`setTrainerOverlayVisible`/`updateTrainerOverlay`, voir Write/Freeze ci-dessus) et les hotkeys (`registerGlobalHotkey` etc., idem), plus le cluster "patch trainer" documenté sous Profils (suite) : `saveProfileCodePatch`/`applyProfileCodePatch`/`restoreProfileCodePatch`/`applyAllProfileCodePatches`/`restoreAllProfileCodePatches`/`inspectProfileCodePatches`.

---

## Profils (suite — non couvert par `docs/AUTOMATION_API.md`)

Fichier source : `apps/desktop/profile_manager.cpp`.

### `saveClrFieldProfileTarget(profileName, targetName, typeSubstring, identityField, identityValue, targetField, valueType, description)`
Réponse : `{ success, profileName, targetName, locator, locatorKind: "clr_field", address, typeSubstring, identityField, identityValue, fieldName, type, targetCount, error }`

### `exportPointerMap(profileName)`
Réponse : `{ success, profileName, targetCount, json (string JSON indenté), error }`

### `importPointerMap(profileName, pointerMapJson, options)`
Réponse : `{ success, profileName, isNewProfile, imported, replaced, skipped, messages: [string], targetCount, error }`

### `setProfileTargetDependencies(profileName, targetName, dependencyNames)`
Réponse : `{ success, profileName, targetName, dependsOn: [string], dependencyCount, error }`

### `exportGhidraArtifacts(profileName)`
Réponse : `{ success, profileName, artifactCount, json, pythonScript, error }`

### `importGhidraSymbols(profileName, symbolsText)`
Réponse : `{ success, profileName, symbolsRead, targetsUpdated, patchesUpdated, unmatched, messages: [string], error }`

### `activateProfileTarget(profileName, targetName)`
Réponse (locator normal) : `{ success, profileName, targetName, groupName, address, type, activeTargetCount, message, error }`
Réponse (locator CLR) : mêmes clés + `locatorKind: "clr_field"`, `clrTypeSubstring`, `clrIdentityField`, `clrIdentityValue`, `clrFieldName` (pas `address` sous forme numérique brute, `address` reste une string hex).

### `saveProfileCodePatch(profileName, patchName, addressHex, aobPattern, patchBytes, metadata)`
Réponse : `{ success, profileName, patchName, module, moduleOffset, patchCount, replaced, signatureMatches, signatureWarning, signatureQuality: {score, level, warning, patternBytes, fixedBytes, wildcardBytes, uniqueFixedBytes, fixedRatio, trainerSafe}, signatureRisk, error }`

### `applyProfileCodePatch(profileName, patchName)`
Réponse : celle d'`applyCodePatch` (voir Patch/AOB) + `profileName`, `patchName`, `matchedAddress`, `matchCount`, `aobPattern`, `signatureQuality`, `signatureRisk`, `signatureMatches`, `executableVersionMismatch`, `executableVersionWarning?`.

### `restoreProfileCodePatch(profileName, patchName)`
Réponse : celle de `restoreCodePatch` + `profileName`, `patchName`, `matchedAddress`.

### `applyAllProfileCodePatches(profileName)` / `restoreAllProfileCodePatches(profileName)`
Réponse : `{ success, profileName, applied/restored, alreadyActive/alreadyInactive, total, results: [résultat par patch], error }`

### `inspectProfileCodePatches(profileName)`
Réponse : `{ success, profileName, states: [{profileName, patchName, module, moduleOffset, signatureScore, signatureLevel, signatureWarning, signatureFixedBytes, signatureWildcardBytes, signatureUniqueFixedBytes, signatureFixedRatio, trainerSafe, savedSignatureMatches, success, active, status ("active"|"original"|"missing"|autre), originalMatches, patchedMatches, matchedAddress, error?, warning?}], total, original, active, ambiguous, missing, invalid, error }`

### `saveProfileAutoAsmScript(profileName, scriptName, scriptText, metadata)`
Réponse : `{ success, profileName, scriptName, scriptCount, replaced, error, errorLine? (si parse échoué) }`

### `applyProfileAutoAsmScript(profileName, scriptName)`
Réponse : celle d'`executeAutoAssemblerScript` (délègue directement), ou `{ success: false, error }` si script introuvable.

### `deleteProfileAutoAsmScript(profileName, scriptName)`
Réponse : `{ success, profileName, scriptName, scriptCount, error }`

### `saveProfileLuaScript(profileName, scriptName, scriptText, metadata)`
Réponse : `{ success, profileName, scriptName, scriptCount, replaced, error }`

### `deleteProfileLuaScript(profileName, scriptName)`
Réponse : `{ success, profileName, scriptName, scriptCount, error }`

---

## Workspace / Pointer chains

Fichier source : `apps/desktop/profile_manager.cpp`.

### `scanPointerChains(addressHex, scanOptions)`
Réponse : `{ success, partial, cancelled, pointersScanned, bytesScanned, elapsedMs, chainCount, chains: [{module, baseOffset, offsets: [hex], depth, label}], error }`

### `resolvePointerChain(chain)`
Réponse : `{ success, finalAddress, steps: [hex adresses intermédiaires], error }`

### `suggestStableLocatorForAddress(addressHex, options)`
Réponse : `{ success, chainCount, elapsedMs, error?, message, bestChain? ({module, baseOffset, offsets, depth, label}) }`

### `savePointerChainProfileTarget(profileName, targetName, chain, valueType, description)`
Réponse : `{ success, profileName, targetName, resolvedAddress, isNewProfile, chainLabel, message, error }`

---

## CLR Inspector

Fichier source : `apps/desktop/clr_inspector_bridge.cpp` (classe `ClrInspectorBridge`). La plupart des méthodes appellent un helper .NET externe (`KillEngineClrInspector.exe`) via un pipe RPC JSON et renvoient `{ success, method, error, result: <payload .NET> }` — le contenu de `result` dépend entièrement du helper .NET (hors périmètre de ce document C++), sauf pour les quelques méthodes qui font du travail natif supplémentaire (listées ci-dessous avec leurs clés ajoutées).

### `getClrInspectorStatus()`
Réponse (100% native, pas de RPC) : `{ success: true, available, helperPath, pipeName, running, attachedProcess, pid, processName }`

### `attachClrInspector()` / `detachClrInspector()` / `shutdownClrInspector()` / `flushClrInspectorCache()`
Réponse RPC brute : `{ success, method, error, result }` (`result` = payload .NET, structure non documentée ici).

### `findClrObjectsByType(typeSubstring)` / `findClrObjectsByFieldValue(typeSubstring, fieldName, expectedValue, maxResults)` / `readClrObject(addressHex)`
Réponse RPC brute (`{ success, result, error }`) ; `findClrObjectsByFieldValue` : `result.matches` est une liste d'objets `{address, ...}` réutilisée par tous les locators CLR C++ (`writeClrPrimitivePathByLocator`, `activateProfileTarget`, etc.).

### `writeClrPrimitiveField(objectAddressHex, fieldName, value)` / `writeClrPrimitivePath(objectAddressHex, path, value)` / `writeClrPrimitivePathBatch(objectAddressHex, operations)`
Réponse RPC brute ; `result.verified`/`result.success`/`result.rolledBack` alimentent la télémétrie mais restent dans `result`, pas promus au niveau racine.

### `writeClrPrimitivePathByLocator(typeSubstring, identityField, identityValue, path, value)`
Réponse : celle de `writeClrPrimitivePath` (relocalisée) + `relocated: true`, `resolvedAddress`, `typeSubstring`, `identityField`, `identityValue`. Erreur si 0 ou >1 objet correspondant (`"Locator ambigu"`).

### `writeClrPrimitivePathBatchByLocator(typeSubstring, identityField, identityValue, operations)`
Réponse : celle de `writeClrPrimitivePathBatch` (relocalisée) + mêmes clés `relocated`/`resolvedAddress`/`typeSubstring`/`identityField`/`identityValue`.

### `writeClrPrimitivePathBatchAtomic(objectAddressHex, operations)`
Réponse : celle de `writeClrPrimitivePathBatch` + `suspendedThreadCount`, `suspendedDuringTransaction: true`.

### `enumerateClrRoots(typeSubstring)`
Réponse RPC brute.

### `callClrInstanceMethod(objectAddressHex, methodName, valueText, valueType)`
Réponse (100% native, injection de shellcode) : `{ success, verified, objectAddress, methodName, error, nativeCodeAddress, parameterType, parameterIsReferenceType, parameterIsStruct, parameterStructPassedByRef, threadCompleted }`

### `findClrGcRootPath(targetObjectAddressHex, maxDepth, maxRootsScanned)` / `generateClrObjectReport(objectAddressHex, maxDepth, maxNodes, includeGcRootChain)` / `enumerateClrRoots`
Réponse RPC brute (`{ success, result, error }`), payload dans `result` défini côté helper .NET.

### `disassembleClrMethod(objectAddressHex, methodName, instructionCount)`
Réponse (native, lecture directe + désassembleur x64 interne) : `{ success, objectAddress, methodName, error, nativeCodeAddress, instructions: [{address, length, disassembly, mnemonicHint, rawBytesText, decoder}], requestedInstructionCount, returnedInstructionCount, truncated, bufferBytesRead }`

---

## Patch / AOB

Fichier source : `apps/desktop/code_patch_manager.cpp`.

### `scanAobPattern(patternText, options)`
Réponse : `{ success, pattern, partial, error, bytesScanned, regionsScanned, matchesFound, matches: [{address, regionBase, regionSize, protection, memoryType, module?, moduleOffset?}], patternBytes, executableOnly, imageOnly, signatureQuality: {score, level, warning, patternBytes, fixedBytes, wildcardBytes, uniqueFixedBytes, fixedRatio, trainerSafe}, signatureRisk, signatureWarning }`

### `generateAobSignature(addressHex, options)`
Réponse : `{ success, address, partial, startAddress, instructionAddress, bytesRead, requestedBytes, hex, pattern, patternBytes, signatureQuality, signatureRisk, module, moduleOffset, error, warning, codeReadProtected? }`

### `applyCodePatch(addressHex, bytesText, options)`
Réponse : `{ success, address, patchBytes, verified, protectionChanged, bytesWritten, originalBytes, writtenBytes, error, active? }`

### `suggestCodePatches(addressHex, options)`
Réponse : `{ success, address, instructionSuccess, instructionLength, mnemonicHint, disassembly, decoder, category, stableAobPattern, signatureQuality, signatureRisk, bytesRead, bytes, memBaseRegister, memDisplacement, error, suggestions: [{label, bytesText, description, category, riskLevel, risky, needsValueInput, valueOffset, valueSize}], warning }`

### `restoreCodePatch(addressHex)`
Réponse : `{ success, address, verified, protectionChanged, bytesWritten, restoredBytes, error, active }`

### `disassembleBackward(addressHex, options)`
Fichier : `debug_feature_manager.cpp`
Réponse : `{ success, address, error, codeReadProtected?, instructions: [{address, bytes, disassembly, mnemonicHint, category, memBaseRegister, memDisplacement, isCandidateField}], candidateFields: [même forme, filtrée], warning }`

### `testCandidateFieldsAsync(writeInstructionAddressHex, knownWriteTargetAddressHex, options)`
Fichier : `application_controller.cpp` (pas délégué)
Réponse immédiate : `{ success, started, requestId, candidateCount, warning, error }`. Résultat final via le signal `candidateFieldTestFinished` : `{ requestId, kind: "test_candidate_fields", success, results: [{address, memBaseRegister, memDisplacement, valueType, verdict ("holds"|"reverts"|"error"), ticksSurvived?, restored?, error?}], cancelled }`

### `cancelCandidateFieldTest()`
Réponse : `{ success, error }`

---

## Debug / Breakpoints

Fichier source : `apps/desktop/debug_feature_manager.cpp`.

### `findWhatWrites(addressHex, options)`
Réponse : `{ success, address, hits: [{address, instructionPointer, threadId, valueBefore, valueAfter, module, moduleOffset}], hitCount, size, timeoutMs, maxHits, warning, error }`

### `findWhatWritesAsync(addressHex, options)`
Réponse immédiate : `{ success, started, requestId, size, timeoutMs, maxHits, error }`. Résultat final via signal `findWhatWritesFinished` : mêmes clés que `findWhatWrites` + `requestId`, `kind: "find_what_writes"`, `cancelled`.

### `cancelFindWhatWrites()`
Réponse : `{ success, error }`

### `findWhatAccesses(addressHex, options)`
Réponse : identique à `findWhatWrites` (même forme de `hits`).

### `findWhatAccessesAsync(addressHex, options)`
Réponse immédiate : `{ success, started, requestId, size, timeoutMs, maxHits, error }`. Résultat final via `findWhatAccessesFinished` : mêmes clés + `requestId`, `kind: "find_what_accesses"`, `cancelled`.

### `findWhatExecutes(instructionAddressHex, options)`
Réponse : `{ success, address, hits: [{address, instructionPointer, threadId, valueBefore, valueAfter, module, moduleOffset, rax..r15, xmm0Hex}] (forme complète breakpointHitToVariant), hitCount, timeoutMs, maxHits, warning, error }`

### `startPageGuardWatchAsync(addressHex, options)`
Réponse immédiate : `{ success, started, requestId, size, timeoutMs, maxHits, error }`. Résultat final via `pageGuardWatchFinished` : `{ requestId, kind: "page_guard_watch", success, address, hits: [{address, instructionPointer, threadId, isWrite, module, moduleOffset}], hitCount, size, timeoutMs, maxHits, timedOut, warning, error }`

### `cancelPageGuardWatch()`
Réponse : `{ success, error }`

### `startInProcessBreakpointWatchAsync(addressHex, options)`
Réponse immédiate : `{ success, started, requestId, size, timeoutMs, maxHits, error }`. Résultat final via `inProcessBreakpointWatchFinished` : `{ requestId, kind: "inprocess_breakpoint_watch", success, address, hits: [forme inProcessBreakpointHitToVariant : instructionPointer, threadId, module, moduleOffset, rax, rcx, rdx, rbp, rsp, r8, r9, xmm0Hex], hitCount, size, captureExecute, timeoutMs, maxHits, timedOut, existingThreadsArmed, warning, error }`

### `cancelInProcessBreakpointWatch()`
Réponse : `{ success, error }`

### `startInProcessExecuteWatchAsync(instructionAddressHex, options)`
Réponse : délègue directement à `startInProcessBreakpointWatchAsync` (mêmes clés, `captureExecute` forcé à `true`).

### `startInProcessExecuteWatch(instructionAddressHex, options)`
Version bloquante (pas de requestId/signal) : `{ success, address, hits, hitCount, timedOut, timeoutMs, maxHits, existingThreadsArmed, error }`

---

## Settings / Diagnostics

Fichier source : `apps/desktop/settings_diagnostics_manager.cpp`, sauf indication contraire.

### `getSettings()`
Réponse : `{ language, defaultValueType, scanMaxResults, scanChunkSizeMb, performanceMode, scanMaxWorkerThreads, scanMaxInFlightMb, candidateFileBackedThreshold, unknownSnapshotMaxMb, fastScan, smartSearchDebugEnabled, smartSearchDebugMaxEvents, modelPath, modelEnabled, modelThreads }` (pas de `success`).

### `saveSettings(settings)`
Réponse : mêmes clés que `getSettings()` + `success: true`.

### `getAiModelStatus()`
Réponse : `{ success, enabled, backend, ready, available, configuredModelPath, envModelPath, envExecutablePath, modelFound, modelPath, modelSource, modelError, executableFound, executablePath, modelCandidates: [{path, exists, isGguf, source, absolutePath?, sizeBytes?}], executableCandidates: [{path, exists, source, absolutePath?, sizeBytes?}], embeddedAgents: [{id, displayName, role, provider, manifestPath, valid, modelFound, error?, modelPath?, modelSizeBytes?, required?}], embeddedAgentCount, embeddedModelFolders: [{id, path, hasGguf, ggufCount, primaryModelPath?, primaryModelSizeBytes?}], threads, message }`

### `browseForModelFile()`
Réponse : `{ success, path }` ou `{ success: false, cancelled: true }` (dialogue annulé — nécessite une interaction UI, peu adapté à l'automation pipe).

### `getLogFilePath()` / `getSmartSearchDebugFilePath()` / `getScanTelemetryFilePath()`
Retournent `QString` brute (chemin de fichier).

### `getSmartSearchDebugEvents(maxEvents)`
Réponse : `{ success, path, events: [objets JSON déjà présents dans le fichier, ou {event: "parse_error", raw, error} en cas de ligne corrompue], error }`

### `clearSmartSearchDebugEvents()`
Réponse : `{ success, path, error }`

### `getLogTail(maxLines)`
Réponse : `{ success, path, lines: [string], error }`

### `exportDiagnostics()`
Réponse : `{ success, path, bytesWritten, error, folderOpened, openFolderError }`

### `getTemporaryStorageStatus()`
Réponse : `{ success, tempPath, activeBytes, activeFileCount, candidateBytes, candidateFileBacked, undoBytes, undoFileBacked, snapshotBytes, snapshotFileBacked, orphanBytes, orphanFileCount, orphanFiles: [{name, path, bytes, lastModified}], totalBytes }`

### `clearTemporaryStorage()`
Réponse : `{ success, tempPath, beforeBytes, closedActiveBytes, removedBytes, removedFileCount, removedFiles: [{path, bytes}], failedFiles: [path], clearedCandidates, hadUndoReduction, hadUnknownSnapshot, message }` (pas d'`error` racine — `success` reflète `failedFiles.isEmpty()`) ; peut aussi renvoyer `{ success: false, error: "Un scan est actif..." }`.

### `hasSeenOnboarding()`
Retourne `bool` brut (lit `QSettings`).

### `setOnboardingSeen(seen)`
`void` (pas de retour).

### `openUserGuide()`
Retourne `bool` brut (succès de l'ouverture via `QDesktopServices`).

### `requestWindowsDefenderExclusion()`
Fichier : `application_controller.cpp`
Réponse : `{ success, cancelled, error? }` (Windows uniquement ; ouvre une invite UAC visible, pas adapté à l'automation non supervisée).

---

## Automation pipe lifecycle

Fichier source : `apps/desktop/automation_pipe_manager.cpp`.

### `enableAutomationMode()`
Réponse : celle de `getAutomationPipeStatus()` + `success` (reflète si le serveur a bien démarré), `error?`.

### `disableAutomationMode()`
Réponse : celle de `getAutomationPipeStatus()` + `success: true`.

### `getAutomationPipeStatus()`
Réponse : `{ enabled, running, pipeName, callCount, lastMethod, lastCallAt }` (les 4 dernières clés viennent de `AutomationPipeServer::status()` si le serveur tourne, sinon valeurs par défaut `running: false`/`callCount: 0`/etc.). Pas de `success` racine.

---

## Lua / AutoAsm scripting

Fichier source : `apps/desktop/application_controller.cpp` (Lua externe, AutoAsm), `apps/desktop/code_patch_manager.cpp` (compilation/exécution AutoAsm bas niveau).

### `getLuaScriptingStatus()`
Réponse : `{ success: true, available, luaPath, helperAvailable, helperPath, helperDirectory, pipeName, automationPipeOptIn, message }`

### `executeLuaScript(scriptText, options)`
Réponse : `{ success, started, timedOut, cancelled, exitCode, luaPath, helperPath, stdout, stderr, error }`

### `executeLuaScriptAsync(scriptText, options)`
Réponse immédiate : `{ success, started, requestId, error }`. Résultat final via signal `luaScriptExecutionFinished` : mêmes clés que `executeLuaScript` + `requestId`, `kind: "lua_script_execute"`.

### `cancelLuaScriptExecution()`
Réponse : `{ success, error }`

### `parseAutoAssemblerScript(scriptText)`
Fichier : `code_patch_manager.cpp`
Réponse : `{ success, parseSuccess, parseError, parseErrorLine, instructionCount, instructions: [{line, target}], allocations: [{name, size}], labels: [string], compileSuccess?, compileError?, compileErrorLine?, compiledBytes? (résumé texte), compiledRegionCount? }`

### `executeAutoAssemblerScript(scriptText)`
Réponse : `{ success, error, errorLine, patchedRegions: [{address, size, wasAllocated}], patchAddress?, patchSize?, active? }`

### `restoreAutoAssemblerScript()`
Réponse : `{ success, restoredAddresses?: [hex], active, error }`

---

## Chat / Smart Search / Auto Resolve

Fichier source : `apps/desktop/smart_search_manager.cpp`. **Ce cluster est le plus fortement branché de tout `ApplicationController`** : `startSmartSearch` seul dépasse 20 branches distinctes (classification d'intention, outils IA, confirmations RiskGate). Les clés listées ci-dessous couvrent l'écrasante majorité des cas réels observés dans le code ; toute branche non listée reste néanmoins un sous-ensemble de ces mêmes clés (jamais une clé complètement nouvelle non documentée ici, sauf mention explicite "non résolu").

### `startSmartSearch(query)`
Clés quasi systématiques : `{ query, aiReady, status, actionStatus, workflowStatus, message, error?, debugFile }` (les deux dernières ajoutées après le calcul de `result` dans la majorité des branches).
Clés fréquentes selon la branche : `intent`, `intentRationale` (toujours ajoutées par `stampIntent`), `tool`, `args`, `rationale`, `state` (`"FirstScanRunning"`/`"Refining"`), `actionResult`, `targetValue`, `suggestedWrites`/`suggestedWrite`, `autoWriteResults`/`autoWriteResult`/`autoWriteCount`, `activeTargetCount`, `previousTargetValue`, `writeHistory`, `rollbackNote`, `recoveryActions: [{id, label, ...}]`, `requiresConfirmation`, `confirmationReason`, `savedProfileTargets`, `uiStringCandidates`/`uiSourceCandidates`, `needsLocalStoreAction` (délégation Trainer côté frontend, voir plus bas), `pendingFeature`/`pendingAddress`/`pendingValueType`/`pendingValue`/`locatorSummary`, `diagnostic`, `observedValue`.
Non résolu à 100% : certaines branches internes à `m_ai.processQuery()` (moteur IA local, hors périmètre de ce fichier — voir `ai/ai_engine.cpp`) peuvent ajouter des clés supplémentaires non énumérées ici.

### `confirmChatMemoryWrite(value)`
Délègue à `writeChatMemoryTargetsFromQuery(QString(), value)`. Réponse : `{ query, aiReady, status: "tool_call", tool: "chat_memory_write", actionStatus, workflowStatus, targetValue, success, actionResult: {success, remaining, error}, suggestedWrites, suggestedWrite, autoWriteResults, autoWriteResult, autoWriteCount, activeTargetCount, previousTargetValue, writeHistory, rollbackNote, message }`

### `confirmChatMemoryFreeze(value)`
Délègue à `freezeChatMemoryTargetsFromQuery(QString(), value)`. Réponse : `{ query, aiReady, status: "tool_call", tool: "chat_memory_freeze", actionStatus, workflowStatus, targetValue, success, suggestedWrites, freezeResults: [{source, address, value, type, ...résultat de setFreezeValue}], activeTargetCount, message, error?, workflowStatus: "freeze_partial_or_failed"? }`

### `confirmRewriteLastAutoWrite(value)`
Délègue à `ApplicationController::rewriteLastAutoWriteTargets(value, QString())` (fonction dans `application_controller.cpp`, pas dans le manager). Réponse : `{ query, aiReady, status: "tool_call", tool: "rewrite_last_auto_write", actionStatus, workflowStatus: "auto_write_done", targetValue, actionResult: {success, remaining, error}, suggestedWrites, suggestedWrite, autoWriteResults, autoWriteResult, autoWriteCount, activeTargetCount, previousTargetValue, writeHistory, rollbackNote, message }`

### `startAutoResolve(query, options)`
Réponse : `{ success, query, status: "auto_resolve", workflowStatus, aiReady, error?, message, plan: [{index, type, description, params, completed, success, result}], planStepCount, targetValue, initialValue, valueType, contextReport (= sortie de getAutoResolveReport), actionStatus, firstAction? / safeAction?, candidateCount?, requiresConfirmation?, confirmationReason?, suggestedWrites?, suggestedWrite?, nextActions: [{id, label, safe, requiresConfirmation?}], executedSafeSteps: [{tool, status, detail, safe, payload?}], fallbackAction?, encryptedMatches?, fallbackTraceUiAction?, uiStringMatches?, unknownCaptureAction? }`

### `getAutoResolveReport(maxEvents)`
Réponse : `{ success: true, attached, processName, candidateCount, workflow, initialValue, targetValue, valueType, activeChatTargetCount, activeProfileTargetCount, learnedProfile: {gameKey, starts, reductions, noCandidateCount, lowCandidateCheckpoints, lastWorkflow, lastCandidateCount, lastUpdated, lastSuccessfulAuditEvent, lastSuccessfulAuditAt, lastSuccessfulAddress, lastSuccessfulValueType, lastSuccessfulAobPattern, strategyWins}, strategyScores: [{id, label, score, reason}], preferredStrategy, nextBestAction: {id, label, tool, confidence, safe, requiresConfirmation, risk, reason, proactive, learnedFrom?, displayValueAware?}, eventCounts, recentSignals: [{event, timestamp, candidateStoreSize, matchesFound, remaining, globalValueHits, error}], telemetryInsights: [{id, label, reason, nextAction, safe, requiresConfirmation}], displayValueReport: {enabled, pattern, traceUiSourceCount, globalValueHits, exactZeroCount, recommendation, warnings}, aob: {multiMatchCount, weakQualityCount, trainerBlockedCount, matchesFound?, qualityReady}, recommendations: [{id, label, safe, reason}], guardrails: [{id, label, risk}], summary }`

### `clearAutoResolveMemory(allProcesses)`
Réponse : `{ success: true, allProcesses, message, gameKey? (si allProcesses==false) }`

### `logAiAudit(event, payload)`
Réponse : `{ success: true, event }` (l'essentiel de l'effet est un écrit `QSettings`, pas dans la réponse).

### `getRememberedPatterns()`
Réponse : `{ success: true, gameKey, patterns: [{module, moduleOffset, valueType, aobPattern, auditEvent, queryLabel, confirmedAt, confirmCount, resolved, liveAddress?}], patternCount }`

### `getWriteHistorySequence()`
Réponse : `{ success: true, gameKey, sequence: [{module, moduleOffset, valueType, value, writtenAt, resolved, liveAddress?}], sequenceCount }`

### `replayWriteHistorySequence()`
Réponse : `{ success, error?, replayedCount, skippedCount, failedCount, details: [{module, moduleOffset, valueType, value, success, error?, liveAddress?}] }`

### `clearWriteHistorySequence()`
Réponse : `{ success: true, gameKey }`

### `getActiveChatMemoryTargets()`
Réponse : `{ success: true, count, targets: [{address, type}] }`

### `clearActiveChatMemoryTargets()`
Réponse : `{ success: true, cleared, targets: [] }`

### `clearScanContext()`
Réponse : `{ success: true, clearedCandidates, hadUndoReduction, hadUnknownSnapshot, wasSmartSearchActive, message }`

### `acknowledgePendingSmartSearchRecovery()`
`void` (pas de retour) — nettoie un état interne côté serveur.

## Carnet d'hypothèses (PHASE 120-E/F)

Fichier source : `apps/desktop/investigation_notebook_manager.cpp` (classe `InvestigationNotebookManager`, délègue à `killai::InvestigationNotebook`, `ai/investigation_notebook.cpp`). Moteur de pondération déterministe, indépendant de tout état process/mémoire — aucune génération d'hypothèse ni de "prochaine expérience" ici, ça reste le rôle du modèle local (PHASE 120-G, pas encore livré). Chaque hypothèse : `{ id, description, confidenceScore, status, evidenceLog }` — `status` vaut `"active"`/`"confirmed"`/`"refuted"` ; score borné `[0,100]`, `+20` sur confirmation, `-30` sur contradiction, verrouillé une fois `confirmed`/`refuted`.

### `addInvestigationHypothesis(description, baselineScore=50)`
Réponse : `{ success: true, hypothesis: {id, description, confidenceScore, status: "active", evidenceLog: []} }` ou `{ success: false, error: "Description vide." }` si `description` est vide/blanc.

### `recordInvestigationTestResult(hypothesisId, confirmed, evidenceNote)`
Réponse : `{ success: true, hypothesis: {...} }` (score/status mis à jour, `evidenceNote` ajouté à `evidenceLog`) ou `{ success: false, error }` — `error` vaut `"Hypothese introuvable."` (id inconnu) ou `"Hypothese deja dans un etat terminal (confirmee ou refutee)."` (hypothèse verrouillée, aucune mise à jour rétroactive).

### `getInvestigationNotebookSynthesis()`
Réponse : `{ success: true, confirmed: [...], active: [...], refuted: [...] }` — chaque liste contient des hypothèses complètes (`{id, description, confidenceScore, status, evidenceLog}`), triées par `confidenceScore` décroissant.

### `resetInvestigationNotebook()`
Réponse : `{ success: true }` — vide le carnet et réinitialise le compteur d'id (`H1` repart de zéro).

### `getSmartSearchContext()`
Réponse : `{ success: true, active, workflow, initialValue, targetValue, valueType, candidateCount, hasUndoReduction, chatTargets: [{address, type}], profileTargets: [{profile, target, group, address, type, locatorKind, clrTypeSubstring?, clrIdentityField?, clrIdentityValue?, clrFieldName?}], lastAutoWriteCount, writeHistory }`

### `callVueStoreAction(action, args)`
Fichier : `application_controller.cpp`. Passerelle vers le store Pinia frontend via `QWebEnginePage::runJavaScript`, restreinte à une liste blanche C++ fixe (`keepCandidate`, `ignoreCandidate`, `addAddressToWatch`, `writeSelectedValue`, `writeSelectedAddresses`, `writeSelectedTargets`, `writeSelectedAtomic`, `rollbackLastWrite`, `rollbackLastWriteBatch`, `freezeCandidateCurrent`, `toggleFreeze`, `startBreakpointFreeze`, `createTrainerFeature`, `deleteTrainerFeature`, `applyTrainerFeature`, `restoreTrainerFeature`, `applyAllTrainerFeatures`, `restoreAllTrainerFeatures`, `getTrainerFeaturesSnapshot`, `generateTrainerFeaturePointerChain`) — c'est la seule voie C++ pour lister/créer/supprimer une feature Trainer.
Réponse : `{ success, action, error?, result? (valeur JS retournée par l'action, forme dépendant de l'action appelée — non documentée ici, propre à `ui/src/stores/*.ts`) }`. Timeout fixe de 5s côté C++ si la page ne répond pas.

---

## Injection / Hooking

Fichier source : `apps/desktop/code_patch_manager.cpp` (DLL/hooks), `apps/desktop/debug_feature_manager.cpp` (API hook), `application_controller.cpp` (injectDllIntoProcess).

### `injectDllIntoProcess(dllPath)`
Fichier : `application_controller.cpp`
Réponse : `{ success, dllPath, error, moduleBase (hex) }`

### `installFunctionHook(targetAddressHex, hookAddressHex)`
Réponse : `{ success, targetAddress, hookAddress, error, trampolineAddress, originalBytes, active? }`

### `removeFunctionHook(targetAddressHex)`
Réponse : `{ success, targetAddress, error, restoredBytes, active? }`

### `startApiHook(moduleName, functionName, mode, forcedReturnValue)`
Fichier : `debug_feature_manager.cpp`
Réponse : `{ success, error, active, callCount }`

### `stopApiHook()`
Réponse : `{ success: true, active: false, finalCallCount? }`

### `getApiHookStatus()`
Réponse : `{ success: true, active, installError?, resolveError?, callCount?, pid? }` (les 4 dernières clés absentes si `active` est `false`).

---

## Save-file / UWP

Fichier source : `apps/desktop/save_file_investigator.cpp` (classe `SaveFileInvestigator`).

### `discoverProcessSaveFiles(maxResults)`
Réponse : `{ success, familyName, files: [{path, sizeBytes, lastWriteTime}], count, error }`

### `inspectProcessLocalSettings(maxValues)`
Réponse : `{ success, familyName, settingsPath, values: [{keyPath, name, type, preview, dataSizeBytes}], count, error }`

### `readProcessSaveFileText(path, maxBytes)`
Réponse : `{ success, path, text, truncated, error }`

### `watchSaveFileForChanges(path, options)`
Réponse (bloquant, attend jusqu'à `timeoutMs`) : `{ success, path, changed, changeType, cancelled, error }`

### `startSaveFileWatchAsync(path, options)`
Réponse immédiate : `{ success, started, path, requestId, error }`. Résultat final via signal `saveFileWatchFinished` : `{ requestId, kind: "save_file_watch", success, path, changed, changeType, cancelled, error }`

### `cancelSaveFileWatch()`
Réponse : `{ success, error }`

### `patchProcessSaveFileBytes(path, findHex, replaceHex)`
Réponse : `{ success, path, occurrencesFound, error }`

---

## Kernel driver

Fichier source : `apps/desktop/kernel_driver_manager.cpp`, sauf `writeMemoryValueKernel` (dans `application_controller.cpp`).

### `probeKernelDriver()`
Réponse : `{ success, status ("Connected"/autre, via KernelDriverBridge::statusToString), devicePath, message, capabilities: {protocolVersion, healthProbe, processMemoryAccess, privilegedInstrumentation} }`

### `startKernelDriver()`
Réponse : `{ success, serviceName: "KillEngineKernel", started, alreadyRunning, status, devicePath, message, error?, serviceState, capabilities (mêmes clés que probeKernelDriver, fusionnées depuis un appel interne à probeKernelDriver) }` (Windows uniquement ; `{ status: "unavailable", ... }` sur les autres OS)

### `readMemoryKernel(addressHex, size)`
Réponse : `{ success, error, bytesRead, hex }` (Windows uniquement, sinon `{ error: "Fonctionnalité Windows uniquement." }`)

### `writeMemoryKernel(addressHex, hexBytes)`
Réponse : `{ success, error, bytesWritten? }`

### `writeMemoryValueKernel(addressHex, valueType, value)`
Fichier : `application_controller.cpp`
Réponse : `{ success, error, bytesWritten? }`

---

## Réseau / Speedhack

Fichier source : `apps/desktop/application_controller.cpp` (réseau, invite UAC via `New-NetFirewallRule`), `apps/desktop/debug_feature_manager.cpp` (speedhack, injection de composant).

### `blockProcessNetwork()`
Réponse : `{ success, cancelled, error?, ruleOutbound?, ruleInbound?, exePath? }` (les 3 dernières clés seulement si `success == true`) — Windows uniquement, invite UAC visible.

### `unblockProcessNetwork()`
Réponse : `{ success, cancelled, error? }`

### `getProcessNetworkBlockStatus()`
Réponse : `{ success: true, blocked, ruleName?, exePath?, error? }`

### `startSpeedhack(factor)`
Réponse : `{ success, error, active, factor, hooksInstalledMask }`

### `setSpeedhackFactor(factor)`
Réponse : `{ success, error, active, installError, factor, hooksInstalledMask, pid }`

### `stopSpeedhack()`
Réponse : `{ success: true, active: false }`

### `getSpeedhackStatus()`
Réponse (inactif) : `{ success: true, active: false, factor: 1.0 }` ; (actif) : `{ success: true, active, installError, factor, hooksInstalledMask, pid }`

---

## Méthodes scalaires simples (pas de forme d'objet à détailler)

| Méthode | Fichier | Retour |
| --- | --- | --- |
| `getVersion()` | `application_controller.cpp` | `QString` (ex: `"1.2.3"`) |
| `ping(message)` | `application_controller.cpp` | `QString` (`"pong: <message> @ HH:mm:ss.zzz"`) |
| `attachProcess(pid)` | `application_controller.cpp` | `bool` |
| `detachProcess()` | `application_controller.cpp` | `void` |
| `hasSeenOnboarding()` | `settings_diagnostics_manager.cpp` | `bool` |
| `setOnboardingSeen(seen)` | `settings_diagnostics_manager.cpp` | `void` |
| `openUserGuide()` | `application_controller.cpp` | `bool` |
| `getLogFilePath()` | `settings_diagnostics_manager.cpp` | `QString` (chemin) |
| `getSmartSearchDebugFilePath()` | `settings_diagnostics_manager.cpp` | `QString` (chemin) |
| `getScanTelemetryFilePath()` | `settings_diagnostics_manager.cpp` | `QString` (chemin) |
| `deleteProfile(profileName)` | `profile_manager.cpp` | `bool` (déjà documenté dans `docs/AUTOMATION_API.md`) |
| `listProfiles()` | `profile_manager.cpp` | `QVariantList` brute, pas un objet englobant (déjà documenté dans `docs/AUTOMATION_API.md`) |
| `acknowledgePendingSmartSearchRecovery()` | `smart_search_manager.cpp` | `void` |

## Méthodes de lecture simple (processus / modules)

Fichier source : `apps/desktop/application_controller.cpp`.

### `getProcesses()`
Réponse : `QVariantList` de `{pid, name, path, arch, hasWindow, moduleCount}` (pas d'objet englobant).

### `getProcessModules(pid)`
Réponse : `QVariantList` de `{name, path, baseAddress, size}` (pas d'objet englobant).

### `resolveSymbolAddress(moduleName, functionName)`
Réponse : `{ success, module, function, error, address }`

### `listModuleExports(moduleName, filterSubstring, maxNames)`
Réponse : `{ success, module, names: [string], count, error }`

---

## Résumé du périmètre couvert

Toutes les méthodes `Q_INVOKABLE` listées dans `apps/desktop/application_controller.h` au 30/08/2026 sont couvertes ci-dessus, sauf les cas explicitement marqués "non résolu" (contenu de `result` opaque provenant du helper .NET CLR externe, ou branches internes du moteur IA local `ai/ai_engine.cpp`). Aucune clé de réponse listée dans ce document n'a été inventée : chacune correspond à une affectation `result["clé"] = ...` (ou équivalent QVariantMap) lue directement dans le `.cpp` cité en regard.
