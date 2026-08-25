# Validate the bundled Lua examples without requiring a live target process.
#
# Usage:
#   .\scripts\test-lua-examples.ps1
#   .\scripts\test-lua-examples.ps1 -RequirePipe
#   .\scripts\test-lua-examples.ps1 -LuaPath .\runtime\lua\lua.exe

param(
    [string]$LuaPath,
    [string]$PipeName = "KillEngineAutomationPipe",
    [int]$PipeTimeoutMs = 800,
    [switch]$RequirePipe
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$examplesRoot = Join-Path $repoRoot "scripts\lua_examples"

function Resolve-LuaPath {
    param([string]$RequestedPath)

    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        $resolved = Resolve-Path -LiteralPath $RequestedPath -ErrorAction SilentlyContinue
        if ($resolved) { return $resolved.Path }
        throw "LuaPath not found: $RequestedPath"
    }

    $candidates = @(
        (Join-Path $repoRoot "runtime\lua\lua.exe"),
        (Join-Path $repoRoot "runtime\lua\lua54.exe"),
        (Join-Path $repoRoot "runtime\lua\lua5.4.exe"),
        (Join-Path $repoRoot "runtime\lua\luajit.exe"),
        "lua.exe",
        "lua54.exe",
        "lua5.4.exe",
        "luajit.exe"
    )

    foreach ($candidate in $candidates) {
        if ($candidate -like "*\*") {
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                return (Resolve-Path -LiteralPath $candidate).Path
            }
            continue
        }

        $command = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($command) { return $command.Source }
    }

    throw "No Lua runtime found. Run .\scripts\setup-lua-runtime.ps1 or pass -LuaPath."
}

function Invoke-Step {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$ScriptBlock
    )

    Write-Host "[RUN] $Name" -ForegroundColor Cyan
    & $ScriptBlock
    Write-Host "[OK]  $Name" -ForegroundColor Green
}

function Invoke-Lua {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [hashtable]$Environment = @{}
    )

    $previous = @{}
    foreach ($key in $Environment.Keys) {
        $previous[$key] = [Environment]::GetEnvironmentVariable($key, "Process")
        [Environment]::SetEnvironmentVariable($key, [string]$Environment[$key], "Process")
    }

    try {
        & $script:ResolvedLua @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "lua exited with code $LASTEXITCODE"
        }
    } finally {
        foreach ($key in $Environment.Keys) {
            [Environment]::SetEnvironmentVariable($key, $previous[$key], "Process")
        }
    }
}

$script:ResolvedLua = Resolve-LuaPath -RequestedPath $LuaPath

$requiredExamples = @(
    "01_ping_and_status.lua",
    "02_exact_scan_snapshot.lua",
    "03_cancellable_wait.lua"
)

Invoke-Step "Lua runtime present" {
    Write-Host "Lua: $script:ResolvedLua"
    & $script:ResolvedLua -v
    if ($LASTEXITCODE -ne 0) {
        throw "lua -v failed with code $LASTEXITCODE"
    }
}

Invoke-Step "Lua examples are present" {
    foreach ($example in $requiredExamples) {
        $path = Join-Path $examplesRoot $example
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Missing Lua example: $path"
        }
    }
}

Invoke-Step "Lua example syntax" {
    $checks = @()
    foreach ($example in $requiredExamples) {
        $path = (Join-Path $examplesRoot $example) -replace "\\", "\\"
        $checks += "assert(loadfile('$path'))"
    }
    Invoke-Lua -Arguments @("-e", ($checks -join "; "))
}

Invoke-Step "Cancellable wait example short run" {
    Invoke-Lua `
        -Arguments @((Join-Path $examplesRoot "03_cancellable_wait.lua")) `
        -Environment @{ KILLENGINE_LUA_WAIT_SECONDS = "1" }
}

$pipeAvailable = $false
try {
    $ping = & (Join-Path $repoRoot "scripts\automation-pipe-call.ps1") `
        -Method ping `
        -ParamsJson '["lua examples test"]' `
        -PipeName $PipeName `
        -TimeoutMs $PipeTimeoutMs 2>$null
    if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($ping)) {
        $pipeAvailable = $true
    }
} catch {
    $pipeAvailable = $false
}

if ($pipeAvailable) {
    Invoke-Step "Pipe-backed example smoke" {
        Invoke-Lua `
            -Arguments @((Join-Path $examplesRoot "01_ping_and_status.lua")) `
            -Environment @{
                KILLENGINE_ROOT = $repoRoot
                KILLENGINE_AUTOMATION_PIPE_NAME = $PipeName
            }
    }
} elseif ($RequirePipe) {
    throw "KillEngine automation pipe '$PipeName' is unavailable. Start KillEngine with KILLENGINE_AUTOMATION_PIPE=1 or omit -RequirePipe."
} else {
    Write-Warning "KillEngine automation pipe '$PipeName' unavailable; skipped pipe-backed smoke. Use -RequirePipe to make this mandatory."
}

Write-Host "Lua examples validation complete." -ForegroundColor Green
