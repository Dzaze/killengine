# KillEngine launch diagnostic bundle
# Usage:
#   .\scripts\diagnose-launch.ps1
#   .\scripts\diagnose-launch.ps1 -ExePath .\dist\KillEngine-portable\KillEngine.exe

param(
    [string]$ExePath,
    [int]$Seconds = 8,
    [string]$OutputDir
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $ExePath) {
    $ExePath = Join-Path $repoRoot "build\bin\KillEngine.exe"
}

$resolvedExe = Resolve-Path $ExePath
$exeName = Split-Path $resolvedExe -Leaf

if (-not $OutputDir) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutputDir = Join-Path $repoRoot "diagnostics\launch-$stamp"
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$reportPath = Join-Path $OutputDir "launch-report.txt"
$eventPath = Join-Path $OutputDir "windows-events.txt"
$processPath = Join-Path $OutputDir "processes-before.txt"
$logsOut = Join-Path $OutputDir "app-logs"

function Write-ReportLine {
    param([string]$Text)
    Add-Content -Path $reportPath -Value $Text -Encoding UTF8
}

Get-CimInstance Win32_Process |
    Where-Object { $_.Name -eq $exeName } |
    Select-Object ProcessId, Name, ExecutablePath, CommandLine |
    Format-List |
    Out-String |
    Set-Content -Path $processPath -Encoding UTF8

Write-ReportLine "KillEngine launch diagnostic"
Write-ReportLine "Generated: $(Get-Date -Format o)"
Write-ReportLine "Executable: $resolvedExe"
Write-ReportLine "ObservationSeconds: $Seconds"
Write-ReportLine ""

$process = Start-Process -FilePath $resolvedExe -PassThru
Write-ReportLine "Started PID: $($process.Id)"

Start-Sleep -Seconds $Seconds

$alive = Get-Process -Id $process.Id -ErrorAction SilentlyContinue
if ($alive) {
    Write-ReportLine "Status: RUNNING after $Seconds second(s)"

    $cim = Get-CimInstance Win32_Process -Filter "ProcessId=$($process.Id)" -ErrorAction SilentlyContinue
    if ($cim) {
        Invoke-CimMethod -InputObject $cim -MethodName Terminate | Out-Null
        Write-ReportLine "Cleanup: terminated launched process via WMI"
    } else {
        Stop-Process -Id $process.Id -Force
        Write-ReportLine "Cleanup: terminated launched process via Stop-Process"
    }
} else {
    $process.Refresh()
    Write-ReportLine "Status: EXITED before $Seconds second(s)"
    Write-ReportLine "ExitCode: $($process.ExitCode)"
}

$since = (Get-Date).AddMinutes(-10)
try {
    Get-WinEvent -FilterHashtable @{ LogName = "Application"; StartTime = $since } -ErrorAction Stop |
        Where-Object {
            $_.ProviderName -in @("Application Error", "Windows Error Reporting", ".NET Runtime") -and
            ($_.Message -like "*$exeName*" -or $_.Properties.Value -contains $exeName)
        } |
        Select-Object TimeCreated, ProviderName, Id, LevelDisplayName, Message |
        Format-List |
        Out-String |
        Set-Content -Path $eventPath -Encoding UTF8
} catch {
    "Unable to read Windows Application event log: $($_.Exception.Message)" |
        Set-Content -Path $eventPath -Encoding UTF8
}

$logRoot = Join-Path $env:LOCALAPPDATA "KillEngine\KillEngine\logs"
if (Test-Path $logRoot) {
    New-Item -ItemType Directory -Force -Path $logsOut | Out-Null
    Get-ChildItem -LiteralPath $logRoot -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 20 |
        ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $logsOut -Force
        }
    Write-ReportLine "AppLogs: copied from $logRoot"
} else {
    Write-ReportLine "AppLogs: no log directory found at $logRoot"
}

Write-ReportLine "WindowsEvents: $eventPath"
Write-ReportLine "ProcessesBefore: $processPath"

Write-Host "Launch diagnostic written to:" -ForegroundColor Green
Write-Host "  $OutputDir" -ForegroundColor Cyan
