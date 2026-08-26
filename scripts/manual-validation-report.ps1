# KillEngine manual validation report helper
# Usage:
#   .\scripts\manual-validation-report.ps1
#   .\scripts\manual-validation-report.ps1 -Tester rage -Target "KillEngineTestTarget"

param(
    [string]$Tester = $env:USERNAME,
    [string]$Target = "KillEngineTestTarget",
    [string]$OutputDir
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $OutputDir) {
    $OutputDir = Join-Path $repoRoot "docs\manual-validation-results"
}
if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}

$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$safeTarget = ($Target -replace '[^a-zA-Z0-9_.-]+', '_').Trim('_')
if (-not $safeTarget) {
    $safeTarget = "target"
}

$reportPath = Join-Path $OutputDir "manual_validation_${safeTarget}_${timestamp}.md"
$killEngineExe = Join-Path $repoRoot "build\bin\KillEngine.exe"
$testTargetExe = Join-Path $repoRoot "build\bin\KillEngineTestTarget.exe"
$unitTests = Join-Path $repoRoot "build\bin\killengine_unit_tests.exe"
$integrationTests = Join-Path $repoRoot "build\bin\killengine_integration_tests.exe"

$content = @"
# KillEngine Manual Validation Report

- Date: $(Get-Date -Format "yyyy-MM-dd HH:mm:ss")
- Tester: $Tester
- Target: $Target
- Windows: $([Environment]::OSVersion.VersionString)
- Machine: $env:COMPUTERNAME
- KillEngine: ``$killEngineExe``
- Test target: ``$testTargetExe``
- Unit tests: ``$unitTests``
- Integration tests: ``$integrationTests``

## Automated Gate Snapshot

- [ ] ``.\scripts\release-check.ps1 -SkipConfigure`` passed before manual pass
- [ ] ``.\scripts\release-check.ps1 -OnlyRecentTargetedTests`` passed
- [ ] ``.\scripts\release-check.ps1 -OnlyAutomationPipeSafeMethods`` passed
- [ ] UI opened without blank screen
- [ ] Backend ping OK
- [ ] Attach/detach does not crash

## KillEngineTestTarget Regression

- [ ] Start ``build\bin\KillEngineTestTarget.exe``
- [ ] Start ``build\bin\KillEngine.exe``
- [ ] Attach to ``KillEngineTestTarget.exe``
- [ ] Exact scan finds visible Int32 value
- [ ] Next scan after value change shrinks candidates
- [ ] Write changes visible value
- [ ] Rollback restores previous value
- [ ] Unknown capture succeeds
- [ ] Unknown next scan after change returns candidates
- [ ] Profile save / reload / activate / write works
- [ ] Polling freeze starts and stops cleanly
- [ ] Breakpoint freeze works on writable test variable or fails with clear permission/debugger error
- [ ] Trainer feature can be created from a confirmed candidate
- [ ] Investigation export produces readable JSON/Markdown

Notes:

~~~

~~~

## Trainer Dependencies (dependsOn)

- [ ] 2-3 chained Trainer features created, one depending on the other(s)
- [ ] Toggling the dependent feature ON activates prerequisites first, in order
- [ ] Apply all / Restore all respect the same dependency order
- [ ] ``dependsOn`` survives profile/workspace save and reload
- [ ] Deleting a prerequisite cleans dangling references on dependents
- [ ] A deliberate dependency cycle is refused cleanly (no crash/hang)

Notes:

~~~

~~~

## Lua Scripting v2

- [ ] Script using ``ke.call``/``ke.call_table`` runs against the attached target
- [ ] JSON response decodes into a usable Lua table
- [ ] ``Stop`` cancels a deliberately slow script quickly
- [ ] Script saves to and reloads from a profile
- [ ] Deleted saved script does not reappear after reload

Notes:

~~~

~~~

## CLR Inspector

- [ ] Discovery/connect flow reaches a ready state on a managed test target
- [ ] A managed field value reads back correctly
- [ ] Attaching to a non-CLR process fails with a clear message (no hang/crash)

Notes:

~~~

~~~

## Kernel Driver / Probe-Only Bridge

- [ ] Driver service status/restart action works from Settings
- [ ] Probe-only IOCTL round-trip succeeds with expected structure
- [ ] Missing/absent driver fails cleanly with a clear message
- [ ] Marked explicitly skipped below if this machine is not set up for driver testing

Notes:

~~~

~~~

## Generic Game / App Smoke Pass

- [ ] Target is a local/offline app or authorized test target
- [ ] Attach succeeds
- [ ] Exact or Unknown workflow reaches a small candidate set
- [ ] No broad write is performed
- [ ] Single-address write/freeze is understandable and reversible
- [ ] Find What Writes is only launched after explicit confirmation
- [ ] AOB/patch suggestions block ambiguous multi-match results

Notes:

~~~

~~~

## UWP / LocalSettings / File Watch / Patch Bytes

- [ ] Save files discovered on an authorized UWP target
- [ ] Save file preview reads back correctly
- [ ] File watch detects an external change without blocking the UI
- [ ] Byte-sequence patch on a disposable test file confirms occurrencesFound and on-disk content
- [ ] LocalSettings values are readable, not just raw hex
- [ ] Assistant fast-path resolves off-memory requests without falling through to the local model first

Notes:

~~~

~~~

## Assistant Trainer Fast-Path

- [ ] ``startSmartSearch("liste le trainer")`` returns ``trainer_list_features`` without loading the local model
- [ ] Creating a write feature from natural language requires an attached process and explicit address/value
- [ ] The created Trainer feature is present but not auto-applied
- [ ] Deleting by natural-language id removes only the requested feature
- [ ] Apply/restore requests return ``requiresConfirmation`` and do not call ``applyTrainerFeature``/``restoreTrainerFeature`` directly

Notes:

~~~

~~~

## Displayed vs Source Classifier

- [ ] ``DisplaySourceClassifier.*`` unit tests pass
- [ ] ``DisplayVsSourceTargetTest.*`` integration tests pass on ``KillEngineTestTarget.exe``
- [ ] Displayed field is reported as likely derived/display-only
- [ ] Source field with no writes is reported honestly as no writes observed
- [ ] Any future Assistant/UI entry remains read-only unless the user explicitly confirms a write elsewhere

Notes:

~~~

~~~

## Debugger Prolonged Validation

- [ ] Capture can be cancelled
- [ ] Timeout returns a clear status
- [ ] A successful hit includes RIP/module/disassembly context
- [ ] UI remains responsive during capture
- [ ] Detach/reattach works after capture
- [ ] Failure path explains permissions/debugger conflict

Notes:

~~~

~~~

## Decision

- [ ] Manual regression accepted
- [ ] Needs fixes before tracker can be closed

Remaining fixes:

~~~

~~~
"@

$content | Out-File -FilePath $reportPath -Encoding utf8NoBOM
Write-Host "Manual validation report created:" -ForegroundColor Green
Write-Host $reportPath
