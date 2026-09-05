# KillEngine Windows packaging script
# Usage:
#   .\scripts\package-windows.ps1
#   .\scripts\package-windows.ps1 -SkipBuild
#   .\scripts\package-windows.ps1 -ExcludeModel
#   .\scripts\package-windows.ps1 -SkipClrInspector
#   .\scripts\package-windows.ps1 -RequireSigning   (fail the build instead of shipping KillEngine.exe unsigned; see docs/CODE_SIGNING.md)

param(
    [switch]$SkipBuild,
    [switch]$ExcludeModel,
    [switch]$SkipClrInspector,
    [switch]$RequireSigning
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildBin = Join-Path $repoRoot "build\bin"
$distRoot = Join-Path $repoRoot "dist"
$packageName = "KillEngine-portable"
$packageRoot = Join-Path $distRoot $packageName
$zipPath = Join-Path $distRoot "$packageName.zip"

function Copy-ItemIfExists {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    if (Test-Path $Path) {
        Copy-Item -LiteralPath $Path -Destination $Destination -Recurse -Force
    }
}

function Find-FirstExistingFile {
    param([Parameter(Mandatory = $true)][string[]]$Paths)

    foreach ($path in $Paths) {
        if (-not [string]::IsNullOrWhiteSpace($path) -and (Test-Path -LiteralPath $path -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $path).Path
        }
    }
    return $null
}

if (-not $SkipBuild) {
    & (Join-Path $repoRoot "scripts\build.ps1")
}

$exePath = Join-Path $buildBin "KillEngine.exe"
if (-not (Test-Path $exePath)) {
    throw "KillEngine.exe was not found at $exePath. Run .\scripts\build.ps1 first."
}

if (Test-Path $packageRoot) {
    $resolvedPackageRoot = (Resolve-Path $packageRoot).Path
    $resolvedDistRoot = if (Test-Path $distRoot) { (Resolve-Path $distRoot).Path } else { $distRoot }
    if (-not $resolvedPackageRoot.StartsWith($resolvedDistRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean unexpected package path: $resolvedPackageRoot"
    }
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $packageRoot | Out-Null

$excludedNames = @(
    "killengine_unit_tests.exe",
    "killengine_unit_tests.pdb",
    "killengine_integration_tests.exe",
    "killengine_integration_tests.pdb",
    "KillEngineBenchmark.exe",
    "KillEngineBenchmark.pdb",
    "KillEngineTestTarget.exe",
    "KillEngineTestTarget.pdb",
    "lz4.pdb"
)

$excludedExtensions = @(".pdb", ".ilk", ".exp", ".lib")

Get-ChildItem -LiteralPath $buildBin -Force | ForEach-Object {
    if ($excludedNames -contains $_.Name) {
        return
    }
    if (-not $_.PSIsContainer -and $excludedExtensions -contains $_.Extension) {
        return
    }
    if (-not $_.PSIsContainer -and $_.Name -match "d\.dll$") {
        return
    }

    Copy-Item -LiteralPath $_.FullName -Destination $packageRoot -Recurse -Force
}

$debugPatterns = @(
    "*.pdb",
    "*.ilk",
    "*.exp",
    "*.lib",
    "*.debug.*",
    "*d.dll",
    "*d.exe",
    "msvcp*d*.dll",
    "vcruntime*d*.dll",
    "ucrt*d*.dll",
    "concrt*d*.dll"
)

foreach ($pattern in $debugPatterns) {
    Get-ChildItem -LiteralPath $packageRoot -Recurse -File -Filter $pattern -ErrorAction SilentlyContinue |
        Remove-Item -Force
}

$signResult = & (Join-Path $repoRoot "scripts\codesign.ps1") -Path (Join-Path $packageRoot "KillEngine.exe") -RequireSigning:$RequireSigning

Copy-ItemIfExists -Path (Join-Path $repoRoot "README.md") -Destination $packageRoot
Copy-ItemIfExists -Path (Join-Path $repoRoot "LICENSE") -Destination $packageRoot
Copy-ItemIfExists -Path (Join-Path $repoRoot "KILLENGINE_PROJECT_SPEC.md") -Destination $packageRoot
Copy-ItemIfExists -Path (Join-Path $repoRoot "docs\PHASE_TRACKER.md") -Destination $packageRoot
Copy-ItemIfExists -Path (Join-Path $repoRoot "docs\USER_GUIDE.md") -Destination $packageRoot
Copy-ItemIfExists -Path (Join-Path $repoRoot "docs\V1_REGRESSION_CHECKLIST.md") -Destination $packageRoot
Copy-ItemIfExists -Path (Join-Path $repoRoot "docs\AUTOMATION_API.md") -Destination $packageRoot

$llamaCli = Find-FirstExistingFile -Paths @(
    (Join-Path $repoRoot "third_party\llama.cpp\llama-cli.exe"),
    (Join-Path $repoRoot "third_party\llama.cpp\build\bin\Release\llama-cli.exe"),
    (Join-Path $repoRoot "third_party\llama.cpp\build\bin\llama-cli.exe"),
    (Join-Path $buildBin "llama-cli.exe")
)

if ($llamaCli) {
    Copy-Item -LiteralPath $llamaCli -Destination (Join-Path $packageRoot "llama-cli.exe") -Force
}

$scriptsOut = Join-Path $packageRoot "scripts"
New-Item -ItemType Directory -Force -Path $scriptsOut | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot "scripts\killengine.lua") -Destination $scriptsOut -Force
Copy-Item -LiteralPath (Join-Path $repoRoot "scripts\automation-pipe-call.ps1") -Destination $scriptsOut -Force

# EDR helper scripts (used by the Modules view for Defender exclusions/registry changes)
Copy-Item -LiteralPath (Join-Path $repoRoot "scripts\add_defender_exclusion.bat") -Destination $scriptsOut -Force
Copy-Item -LiteralPath (Join-Path $repoRoot "scripts\disable_defender_registry.bat") -Destination $scriptsOut -Force
Copy-Item -LiteralPath (Join-Path $repoRoot "scripts\enable_defender_registry.bat") -Destination $scriptsOut -Force

$luaExamples = Join-Path $repoRoot "scripts\lua_examples"
if (Test-Path $luaExamples) {
    Copy-Item -LiteralPath $luaExamples -Destination $scriptsOut -Recurse -Force
}

$luaRuntimeExe = Find-FirstExistingFile -Paths @(
    (Join-Path $repoRoot "runtime\lua\lua.exe"),
    (Join-Path $repoRoot "runtime\lua\lua54.exe"),
    (Join-Path $repoRoot "runtime\lua\lua5.4.exe"),
    (Join-Path $repoRoot "runtime\lua\luajit.exe"),
    (Join-Path $repoRoot "third_party\lua\lua.exe"),
    (Join-Path $repoRoot "third_party\lua\lua54.exe"),
    (Join-Path $repoRoot "third_party\lua\lua5.4.exe"),
    (Join-Path $repoRoot "third_party\lua\luajit.exe"),
    (Join-Path $repoRoot "third_party\lua\bin\lua.exe"),
    (Join-Path $repoRoot "third_party\lua\bin\lua54.exe"),
    (Join-Path $repoRoot "third_party\lua\bin\lua5.4.exe"),
    (Join-Path $repoRoot "third_party\lua\bin\luajit.exe"),
    (Join-Path $repoRoot "tools\lua\lua.exe"),
    (Join-Path $repoRoot "tools\lua\lua54.exe"),
    (Join-Path $repoRoot "tools\lua\lua5.4.exe"),
    (Join-Path $repoRoot "tools\lua\luajit.exe"),
    (Join-Path $buildBin "runtime\lua\lua.exe"),
    (Join-Path $buildBin "runtime\lua\lua54.exe"),
    (Join-Path $buildBin "runtime\lua\lua5.4.exe"),
    (Join-Path $buildBin "runtime\lua\luajit.exe")
)

if ($luaRuntimeExe) {
    $luaRuntimeSource = Split-Path -Parent $luaRuntimeExe
    $luaRuntimeOut = Join-Path $packageRoot "runtime\lua"
    New-Item -ItemType Directory -Force -Path $luaRuntimeOut | Out-Null
    Get-ChildItem -LiteralPath $luaRuntimeSource -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in @(".exe", ".dll", ".txt", ".md") -or $_.Name -match "license|copying" } |
        ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $luaRuntimeOut -Force
        }
} else {
    Write-Warning "Lua runtime not found. Put Lua in runtime\lua, third_party\lua, third_party\lua\bin or tools\lua to bundle scripting support."
}

if (-not $SkipClrInspector) {
    $dotnetCmd = Get-Command dotnet -ErrorAction SilentlyContinue
    if (-not $dotnetCmd) {
        throw "SDK .NET introuvable sur le PATH. Installe le SDK .NET 8+ ou relance avec -SkipClrInspector pour un package sans inspecteur CLR."
    }

    $clrProject = Join-Path $repoRoot "tools\clr_inspector\KillEngineClrInspector\KillEngineClrInspector.csproj"
    if (-not (Test-Path -LiteralPath $clrProject -PathType Leaf)) {
        throw "Projet KillEngineClrInspector introuvable: $clrProject"
    }

    $clrOut = Join-Path $packageRoot "tools\clr_inspector"
    New-Item -ItemType Directory -Force -Path $clrOut | Out-Null
    Write-Host "Publishing KillEngineClrInspector self-contained..." -ForegroundColor Cyan
    & dotnet publish $clrProject -c Release -r win-x64 --self-contained true -o $clrOut `
        -p:DebugType=None -p:DebugSymbols=false
    if ($LASTEXITCODE -ne 0) {
        throw "dotnet publish KillEngineClrInspector a échoué (code $LASTEXITCODE)."
    }

    if (-not (Test-Path -LiteralPath (Join-Path $clrOut "KillEngineClrInspector.exe") -PathType Leaf)) {
        throw "Publication ClrMD invalide: KillEngineClrInspector.exe absent de $clrOut"
    }

    Get-ChildItem -LiteralPath $clrOut -Recurse -File -Include "*.pdb", "*.xml" -ErrorAction SilentlyContinue |
        Remove-Item -Force
} else {
    Write-Warning "KillEngineClrInspector skipped. The CLR view will require a dev-built helper or will report it as unavailable."
}

$modelRoot = Join-Path $repoRoot "model"
$modelOut = Join-Path $packageRoot "model"
New-Item -ItemType Directory -Force -Path $modelOut | Out-Null
Copy-ItemIfExists -Path (Join-Path $modelRoot "README.md") -Destination $modelOut

Get-ChildItem -LiteralPath $modelRoot -Directory -ErrorAction SilentlyContinue |
    ForEach-Object {
        $destination = Join-Path $modelOut $_.Name
        New-Item -ItemType Directory -Force -Path $destination | Out-Null
        Copy-ItemIfExists -Path (Join-Path $_.FullName "README.md") -Destination $destination
        Copy-ItemIfExists -Path (Join-Path $_.FullName "MODEL_MANIFEST.md") -Destination $destination
        Copy-ItemIfExists -Path (Join-Path $_.FullName "MODEL_MANIFEST.json") -Destination $destination
    }

if (-not $ExcludeModel) {
    Get-ChildItem -LiteralPath $modelRoot -Recurse -File -Filter "*.gguf" -ErrorAction SilentlyContinue |
        ForEach-Object {
            $relative = [System.IO.Path]::GetRelativePath($modelRoot, $_.DirectoryName)
            $destination = Join-Path $modelOut $relative
            New-Item -ItemType Directory -Force -Path $destination | Out-Null
            Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
        }

    $packagedModels = @(Get-ChildItem -LiteralPath $modelOut -Recurse -File -Filter "*.gguf" -ErrorAction SilentlyContinue)
    if ($packagedModels.Count -eq 0) {
        throw "No embedded .gguf model was found in $modelOut. Product packages must include AI models. Use -ExcludeModel only for lightweight development packages."
    }
    if (-not (Test-Path -LiteralPath (Join-Path $packageRoot "llama-cli.exe") -PathType Leaf)) {
        throw "llama-cli.exe was not found. Product packages must include the embedded AI runtime. Put it in third_party\llama.cpp or build\bin, or use -ExcludeModel only for lightweight development packages."
    }
}

Get-ChildItem -LiteralPath $modelOut -Recurse -File -Filter "*.partial" -ErrorAction SilentlyContinue |
    Remove-Item -Force

$packageReadme = @"
KillEngine portable package
===========================

Run:
  KillEngine.exe

Notes:
  - This package is intended for local/offline testing.
  - llama-cli.exe is copied automatically when present in third_party/llama.cpp.
  - Lua scripting uses runtime\lua\lua.exe when bundled, then falls back to PATH.
  - Lua helper scripts live in scripts\killengine.lua, scripts\automation-pipe-call.ps1 and scripts\lua_examples.
  - CLR inspection uses tools\clr_inspector\KillEngineClrInspector.exe when bundled.
  - GGUF models are included by default.
  - Use -ExcludeModel only for lightweight development packages.
  - Use -SkipClrInspector only for lightweight development packages without the CLR helper.
  - The normal product layout is model\<ai-name>\ next to KillEngine.exe.
  - Agent folders use MODEL_MANIFEST.json and may point to shared GGUF weights.
  - A custom model path is only an advanced override.
  - Logs and profiles are stored under the Windows local app data folder.
  - Read USER_GUIDE.md for the V1 user workflow.
  - Read AUTOMATION_API.md to script KillEngine via the local JSON-RPC pipe (Lua scripting, or your own agent/tool).
  - If the app does not start from a development checkout, run scripts\diagnose-launch.ps1.
"@

Set-Content -Path (Join-Path $packageRoot "PACKAGE_README.txt") -Value $packageReadme -Encoding ASCII

$requiredRuntimeItems = @(
    "KillEngine.exe",
    "model\README.md",
    "model\assistant\README.md",
    "model\assistant\MODEL_MANIFEST.json",
    "model\auto_resolver\README.md",
    "model\auto_resolver\MODEL_MANIFEST.json",
    "model\qwen\README.md",
    "model\qwen\MODEL_MANIFEST.md",
    "QtWebEngineProcess.exe",
    "Qt6Core.dll",
    "Qt6Gui.dll",
    "Qt6Widgets.dll",
    "Qt6WebChannel.dll",
    "Qt6WebEngineCore.dll",
    "Qt6WebEngineWidgets.dll",
    "platforms\qwindows.dll",
    "resources\icudtl.dat",
    "resources\qtwebengine_resources.pak",
    "resources\qtwebengine_resources_100p.pak",
    "resources\qtwebengine_resources_200p.pak",
    "resources\v8_context_snapshot.bin",
    "translations\qtwebengine_locales\en-US.pak",
    "PACKAGE_README.txt",
    "scripts\killengine.lua",
    "scripts\automation-pipe-call.ps1",
    "scripts\add_defender_exclusion.bat",
    "scripts\disable_defender_registry.bat",
    "scripts\enable_defender_registry.bat",
    "scripts\lua_examples\README.md",
    "USER_GUIDE.md",
    "V1_REGRESSION_CHECKLIST.md",
    "AUTOMATION_API.md"
)

if (-not $SkipClrInspector) {
    $requiredRuntimeItems += @(
        "tools\clr_inspector\KillEngineClrInspector.exe"
    )
}

$missingRuntimeItems = @(
    foreach ($item in $requiredRuntimeItems) {
        $path = Join-Path $packageRoot $item
        if (-not (Test-Path $path)) {
            $item
        }
    }
)

if ($missingRuntimeItems.Count -gt 0) {
    throw "Portable package is missing required runtime item(s): $($missingRuntimeItems -join ', ')"
}

$forbiddenRuntimeItems = @(
    "killengine_unit_tests.exe",
    "killengine_integration_tests.exe",
    "KillEngineBenchmark.exe",
    "KillEngineTestTarget.exe"
)

$forbiddenPresent = @(
    foreach ($item in $forbiddenRuntimeItems) {
        $path = Join-Path $packageRoot $item
        if (Test-Path $path) {
            $item
        }
    }
)

if ($forbiddenPresent.Count -gt 0) {
    throw "Portable package contains development executable(s): $($forbiddenPresent -join ', ')"
}

if (-not $ExcludeModel) {
    $productModels = @(Get-ChildItem -LiteralPath (Join-Path $packageRoot "model") -Recurse -File -Filter "*.gguf" -ErrorAction SilentlyContinue)
    if ($productModels.Count -eq 0) {
        throw "Product package validation failed: no embedded AI model found under model\<ai-name>\*.gguf."
    }
    if (-not (Test-Path -LiteralPath (Join-Path $packageRoot "llama-cli.exe") -PathType Leaf)) {
        throw "Product package validation failed: llama-cli.exe is missing."
    }
}

if (Test-Path $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}

Compress-Archive -Path (Join-Path $packageRoot "*") -DestinationPath $zipPath -Force

Write-Host "Portable package ready:" -ForegroundColor Green
Write-Host "  Folder: $packageRoot" -ForegroundColor Cyan
Write-Host "  Zip:    $zipPath" -ForegroundColor Cyan
Write-Host "  Signed: $($signResult.Signed)" -ForegroundColor $(if ($signResult.Signed) { "Cyan" } else { "Yellow" })
