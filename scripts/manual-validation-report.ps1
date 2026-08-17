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

$content | Out-File -FilePath $reportPath -Encoding UTF8
Write-Host "Manual validation report created:" -ForegroundColor Green
Write-Host $reportPath
