> **ATTENTION - Priorité Des Ordres Projet**
> Un ordre prioritaire explicite du propriétaire du projet prime sur les consignes temporaires de session des agents IA.
# KillEngine V1 Regression Checklist

Use this checklist before tagging a V1 release candidate.

## Automated Gate

Run from the repository root:

```powershell
.\scripts\release-check.ps1
```

Create a dated manual report template:

```powershell
.\scripts\manual-validation-report.ps1 -Target "KillEngineTestTarget"
```

Optional portable package gate:

```powershell
.\scripts\release-check.ps1 -Package
```

Optional Lua examples check, best-effort (never fails the gate if `runtime\lua\` is absent on this machine):

```powershell
.\scripts\release-check.ps1 -IncludeLuaExamples
```

Optional targeted replay for the most recent automated checks:

```powershell
.\scripts\test-recent-targeted.ps1
.\scripts\release-check.ps1 -IncludeRecentTargetedTests
```

For a strict pipe-backed check of the Lua examples (needs `KillEngine.exe` already running with `KILLENGINE_AUTOMATION_PIPE=1`), run this separately — `-RequirePipe` is intentionally not wired into `release-check.ps1`:

```powershell
.\scripts\test-lua-examples.ps1 -RequirePipe
```

Expected result:

- UI type-check passes.
- UI build passes.
- CMake configure passes.
- C++ build passes.
- Embedded AI layout verifier passes for `build\bin`.
- `build\bin\KillEngine.exe` stays alive during the launch smoke test.
- Unit tests pass.
- Integration tests pass.
- Optional portable zip is produced under `dist/`.
- Optional portable package contains only runtime executables and required Qt/WebEngine assets.
- Optional portable package passes the embedded AI layout verifier.
- Optional Lua examples check runs the bundled examples when `runtime\lua\` is present; warns and continues (does not fail the gate) when it is absent.
- Optional recent targeted tests replay the Trainer fast-path, Field Stability fast-path, AI registry, DisplaySourceClassifier unit suite, and DisplayVsSourceTarget integration suite.
- `dist\KillEngine-portable\KillEngine.exe` stays alive during the portable launch smoke test.

## KillEngineTestTarget Manual Pass

1. Start `build\bin\KillEngineTestTarget.exe`.
2. Start `build\bin\KillEngine.exe`.
3. Attach to `KillEngineTestTarget.exe`.
4. Run an exact `Int32` scan on the visible score value.
5. Change the score in the target, run `next_scan exact`, and confirm candidates shrink.
6. Write a new score, confirm the target UI changes, then rollback.
7. Run an unknown initial value capture.
8. Change the unknown value in the target, compare with `changed`, and confirm candidates appear.
9. Save one target into a profile, reload/activate it, write through the profile, then rollback.
10. Confirm the action log records scan, write, rollback, profile activation, and errors clearly.

## Trainer Dependencies (`dependsOn`) Manual Pass

**Covered by automated test** (PHASE 115): `.\scripts\test-trainer-dependencies.ps1` runs the real `ui/src/stores/trainerDependencies.ts` logic (not a reimplementation) and asserts the Apply all order, the reversed Restore all order, a togglable action (`freeze_polling`, not just one-shot `write`), dead-reference cleanup on delete, and cycle/missing-reference rejection — steps 3, 5, 6 below. Run it before this manual pass to catch logic regressions cheaply; the manual pass still matters for what the script can't see: real UI wiring (clicks, confirmation dialogs) and profile/workspace persistence (step 4).

1. On `KillEngineTestTarget.exe`, create 2-3 Trainer features and set one to depend on the other(s) via the "Dépend de" selector.
2. Toggle ON the dependent feature; confirm prerequisites activate first, in order, in the Trainer action log.
3. Use Apply all / Restore all; confirm the same dependency order is respected and no feature is skipped or double-applied.
4. Save the chained features into a profile or workspace, reload it, and confirm `dependsOn` is still there (not dropped on reload).
5. Delete one prerequisite feature; confirm dependent features lose the dead reference instead of pointing at a missing id.
6. Deliberately create a dependency cycle (A depends on B, B depends on A); confirm it is refused with a clear action-log message, not a crash or a hang.

## Assistant Manual Pass

1. Ask the Assistant to find a value visible in `KillEngineTestTarget.exe`.
2. Follow the guided flow: initial value, changed game value, new value.
3. When 1-4 candidates remain, confirm auto-write behavior and visible rollback batch.
4. Ask a natural rewrite such as `mets-les a 3000`.
5. Say that the address did not work and confirm recovery actions are offered.
6. Clear active addresses and start a new unrelated search.
7. Ask `liste le trainer` and confirm the response uses the Trainer fast-path instead of loading the local model.
8. Ask to create a Trainer write feature with an explicit address/value; confirm it is created but not applied.
9. Ask to apply or restore Trainer features and confirm the Assistant asks for UI confirmation instead of applying directly.

## Displayed vs Source Classifier Manual Pass

Automated proxy, no human needed:

```powershell
.\build\bin\killengine_unit_tests.exe --gtest_filter=DisplaySourceClassifier.*
.\build\bin\killengine_integration_tests.exe --gtest_filter=DisplayVsSourceTargetTest.*
```

1. On `KillEngineTestTarget.exe`, locate the displayed counter field exposed by the Displayed vs Source fixture and confirm it is classified as likely derived/display-only.
2. Locate the source field and confirm the classifier reports no writes observed unless the target explicitly changes it.
3. If this gets surfaced in Assistant/UI later, confirm the flow remains read-only and only advises the user before any write-capable action.

## Expert Mode Manual Pass

1. Use memory map to open a region in Expert mode.
2. Confirm start/stop address and filters are prefilled.
3. Inspect memory preview: hex, ASCII, decoded values, copy, scan around.
4. Keep, ignore, watch, and use a candidate in the Assistant.
5. Try writing to an unknown/non-writable region and confirm explicit acknowledgement is required.
6. Start and stop freeze on a known safe writable target.

## Lua Scripting v2 Manual Pass

1. Open the Scripting view and run a simple script using `ke.call`/`ke.call_table` against the attached `KillEngineTestTarget.exe` (requires `KILLENGINE_AUTOMATION_PIPE=1`).
2. Confirm the JSON response decodes into a usable Lua table (`ke.decode_json`/`ke.call_table`).
3. Run a deliberately slow script (e.g. a loop or `os.execute` sleep), click `Stop`, and confirm cancellation returns quickly instead of waiting out the script.
4. Save the script to a profile, reload the profile, confirm the script text loads back into the editor.
5. Delete the saved script and confirm it is gone after reloading the profile.

## CLR Inspector Manual Pass

1. Attach to a managed/.NET test target (`KillEngineClrTestTarget` or another authorized CLR process).
2. Run the CLR inspector discovery/connect flow and confirm it reaches a ready state.
3. Read a managed field value through the inspector and confirm it matches the known test value.
4. Attach to a non-CLR process (no COR20 header) and confirm the inspector fails with a clear message instead of hanging or crashing.

## Kernel Driver / Probe-Only Bridge Manual Pass

1. On a machine set up for kernel driver testing (test signing enabled), confirm the driver service status/restart action works from Settings.
2. Confirm a probe-only IOCTL round-trip succeeds and returns the expected structure.
3. Confirm the app fails cleanly with a clear message (no crash) when the driver is absent or not installed.
4. If this machine is not set up for driver testing, mark this section skipped explicitly in the report rather than leaving it blank.

## Authorized Third-Party Smoke Pass

1. Attach to an authorized third-party application only after the KillEngineTestTarget pass succeeds.
2. Run a normal exact scan on a visible value.
3. Refine after changing the value.
4. Avoid broad writes; write only to a final small candidate set.
5. Confirm rollback and freeze are understandable from the UI.

## UWP / LocalSettings / File Watch / Patch Bytes Manual Pass

1. Attach to an authorized UWP application (e.g. a Store app you own, or `Notepad.exe` which is itself a UWP package).
2. Discover save files (Expert "Fichiers de sauvegarde" panel or `discover_save_files`) and confirm results.
3. Read a save file preview and confirm the content is readable.
4. Start a file watch on the selected file, trigger a change externally, and confirm the change notification arrives without blocking the UI.
5. On a disposable test file (never a real user save), patch a byte sequence via find/replace hex and confirm `occurrencesFound` plus the new content on disk; cancel/undo path stays clear.
6. Inspect LocalSettings for the attached process and confirm readable values (not just a raw hex dump for the common types).
7. Ask the Assistant naturally ("inspecte LocalSettings", "trouve le fichier de sauvegarde", "surveille ce fichier") and confirm it resolves in well under the old >60s regression (PHASE 96/100), not by falling through to the local model first.

## Reliability Pass — Persistence & Freeze Under Real Conditions

Purpose: the automated suite proves the mechanisms work on `KillEngineTestTarget.exe` under controlled, synthetic conditions. It cannot prove they hold on a real third-party application, because that needs a human varying values in real time and judging whether the result still looks right. Run this pass before claiming a locator or freeze mode is reliable beyond the test target — scope stays local, offline, single-player software you own or control, per `docs/ULTIMATE_PRODUCT_GUIDELINE.md`.

Automated proxy already covered by CI/build (no human needed for this part):

```powershell
.\build\bin\killengine_integration_tests.exe --gtest_filter=PowerUpRuntimeTest.BreakpointFreeze*
```

`BreakpointFreezeHoldsUnderFastRewriteStress` measures the actual hold rate of hardware-breakpoint freeze against a target rewriting its own memory at ~1000 Hz (see `AGENTS.md` section "Freeze par hardware breakpoint" for what the numbers mean and why an external `WriteProcessMemory` writer cannot be used to simulate this).

Manual steps, on an authorized third-party application:

1. **Freeze BP under real load.** Find a value that visibly changes fast in the target (a counter, a timer, a fast-ticking resource). Freeze it with `Freeze BP` rather than polling. Watch for at least 30 seconds of active gameplay/use. Record whether the displayed value ever visibly reverts to the game's own value (a flicker means the hold rate is lower on this target than on the test bench — note the app and scenario).
2. **Pointer chain survives a restart.** Find and write a value with the normal guided flow. Use `Stabiliser cette adresse` (Expert → Write panel) to resolve a pointer chain, save it to a profile. Close the target application fully, relaunch it, reattach, activate the saved profile target, and confirm the resolved address is still correct without a new scan.
3. **AOB patch survives an app restart (not just a fresh scan).** Build an AOB-based patch (Expert → AOB signatures), save it to a profile as a Trainer feature, restart the target application, and confirm `applyProfileCodePatch` / the Trainer toggle still finds a unique match and applies cleanly. A patch that becomes `ambiguous` or fails to resolve after restart is expected on some binaries (ASLR-sensitive code, JIT) — record it, don't force-apply.
4. **Worst-case fallback chain.** Deliberately pick a value that the first exact scan won't find (something encoded, or only visible as on-screen text). Ask the Assistant/Auto to resolve it and confirm it actually falls back through encrypted scan → Trace UI string → Unknown capture, per `startAutoResolve` in `AGENTS.md`, rather than just giving up after the first empty scan.

Record results the same way as the rest of this checklist (app name/version, Windows version, what worked, what needed a manual retry, what failed outright). A step that fails on a specific third-party app is expected and useful data — it is not a release blocker by itself unless it also fails on `KillEngineTestTarget.exe`.

## Release Notes

Record:

- Windows version.
- Visual Studio / Build Tools kit.
- Qt path.
- AI layout verifier result for `build\bin` and, if packaged, `dist\KillEngine-portable`.
- Embedded model path under `model\qwen\` and agents under `model\assistant\` / `model\auto_resolver\`.
- Test results and any skipped item.
- Manual report path under `docs/manual-validation-results/`.
