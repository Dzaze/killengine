# KillEngine V1 Regression Checklist

Use this checklist before tagging a V1 release candidate.

## Automated Gate

Run from the repository root:

```powershell
.\scripts\release-check.ps1
```

Optional portable package gate:

```powershell
.\scripts\release-check.ps1 -Package
```

Expected result:

- UI type-check passes.
- UI build passes.
- CMake configure passes.
- C++ build passes.
- Unit tests pass.
- Integration tests pass.
- Optional portable zip is produced under `dist/`.

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

## Assistant Manual Pass

1. Ask the Assistant to find a value visible in `KillEngineTestTarget.exe`.
2. Follow the guided flow: initial value, changed game value, new value.
3. When 1-4 candidates remain, confirm auto-write behavior and visible rollback batch.
4. Ask a natural rewrite such as `mets-les a 3000`.
5. Say that the address did not work and confirm recovery actions are offered.
6. Clear active addresses and start a new unrelated search.

## Expert Mode Manual Pass

1. Use memory map to open a region in Expert mode.
2. Confirm start/stop address and filters are prefilled.
3. Inspect memory preview: hex, ASCII, decoded values, copy, scan around.
4. Keep, ignore, watch, and use a candidate in the Assistant.
5. Try writing to an unknown/non-writable region and confirm explicit acknowledgement is required.
6. Start and stop freeze on a known safe writable target.

## Solitaire Smoke Pass

1. Attach to Solitaire only after the KillEngineTestTarget pass succeeds.
2. Run a normal exact scan on a visible value.
3. Refine after changing the value.
4. Avoid broad writes; write only to a final small candidate set.
5. Confirm rollback and freeze are understandable from the UI.

## Release Notes

Record:

- Windows version.
- Visual Studio / Build Tools kit.
- Qt path.
- Model path if AI runtime is enabled.
- Test results and any skipped item.
