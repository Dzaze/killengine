<#
PORT-1 (docs/PORTABILITY_ROADMAP.md, 17/09/2026), 4e lot : lancement borne du
runtime IA livre + test minimal, en complement de verify-ai-layout.ps1
(presence statique des fichiers) et verify-native-dependencies.ps1 (imports
PE statiques). Aucun des deux ne voit les DLL chargees dynamiquement --
constate sur ce paquet : ggml.dll choisit et charge au demarrage, via
LoadLibrary selon le CPU detecte, une DLL parmi ggml-cpu-<arch>.dll
(sse42/haswell/alderlake/...), invisible dans dumpbin /dependents de
llama-cli.exe. Seul un vrai lancement le prouve.

Reproduit l'invocation reelle de ai/llama_runtime.cpp (memes flags), avec un
prompt et un nombre de tokens minimaux et un timeout borne, contre le premier
agent dont le manifest resout dans le layout donne (build\bin par defaut, ou
un paquet portable). Echoue si le processus ne demarre pas, timeout, sort en
erreur, ou ne produit aucune sortie.

Usage:
  .\scripts\verify-ai-runtime-smoke.ps1
  .\scripts\verify-ai-runtime-smoke.ps1 -LayoutRoot .\dist\KillEngine-portable
#>
param(
    [string]$LayoutRoot,
    [string]$AgentId = "assistant",
    [int]$TimeoutSeconds = 60
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $LayoutRoot) {
    $LayoutRoot = Join-Path $repoRoot "build\bin"
}
if (-not (Test-Path -LiteralPath $LayoutRoot -PathType Container)) {
    throw "Layout root introuvable: $LayoutRoot"
}
$resolvedRoot = (Resolve-Path -LiteralPath $LayoutRoot).Path

$llamaCli = Join-Path $resolvedRoot "llama-cli.exe"
if (-not (Test-Path -LiteralPath $llamaCli -PathType Leaf)) {
    throw "llama-cli.exe introuvable dans $resolvedRoot -- lancer verify-ai-layout.ps1 d'abord pour un diagnostic complet."
}

$manifestPath = Join-Path $resolvedRoot "model\$AgentId\MODEL_MANIFEST.json"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Manifest introuvable: $manifestPath -- lancer verify-ai-layout.ps1 d'abord pour un diagnostic complet."
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$agentDir = Split-Path $manifestPath
$modelPath = [System.IO.Path]::GetFullPath((Join-Path $agentDir $manifest.modelPath))
if (-not (Test-Path -LiteralPath $modelPath -PathType Leaf)) {
    throw "Modele introuvable pour l'agent '$AgentId': $modelPath"
}

Write-Host "Lancement borne de llama-cli.exe (agent '$AgentId') :" -ForegroundColor Cyan
Write-Host "  Executable: $llamaCli"
Write-Host "  Modele:     $modelPath"

# Memes flags que ai/llama_runtime.cpp (LlamaRuntime::runOneShot), n/context
# reduits au minimum utile pour ce test de fumee.
$arguments = @(
    "-m", $modelPath,
    "-p", "Reponds uniquement par le mot: OK",
    "-n", "8",
    "-c", "512",
    "--temp", "0",
    "--no-display-prompt",
    "--single-turn",
    "--reasoning", "off",
    "--no-warmup"
)

$process = New-Object System.Diagnostics.Process
$process.StartInfo.FileName = $llamaCli
foreach ($arg in $arguments) { $process.StartInfo.ArgumentList.Add($arg) }
$process.StartInfo.WorkingDirectory = $resolvedRoot
$process.StartInfo.UseShellExecute = $false
$process.StartInfo.RedirectStandardOutput = $true
$process.StartInfo.RedirectStandardError = $true

$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
$process.Start() | Out-Null
$stdout = $process.StandardOutput.ReadToEndAsync()
$stderr = $process.StandardError.ReadToEndAsync()
$exited = $process.WaitForExit($TimeoutSeconds * 1000)
$stopwatch.Stop()

if (-not $exited) {
    try { $process.Kill() } catch {}
    throw "llama-cli.exe n'a pas termine sous $TimeoutSeconds s -- DLL de backend manquante/bloquante possible, ou machine trop lente pour ce delai."
}

$stdoutText = $stdout.Result
$stderrText = $stderr.Result

if ($process.ExitCode -ne 0) {
    Write-Host $stderrText -ForegroundColor DarkGray
    throw "llama-cli.exe a quitte avec le code $($process.ExitCode) (voir sortie ci-dessus) -- runtime IA livre non fonctionnel hors de l'arbre de developpement."
}

if ([string]::IsNullOrWhiteSpace($stdoutText)) {
    Write-Host $stderrText -ForegroundColor DarkGray
    throw "llama-cli.exe a quitte en succes mais sans sortie -- inference probablement non fonctionnelle malgre le code de sortie 0."
}

Write-Host "  Duree: $([math]::Round($stopwatch.Elapsed.TotalSeconds, 1)) s"
Write-Host "  Sortie: $($stdoutText.Trim())"
Write-Host ""
Write-Host "Runtime IA livre OK : llama-cli.exe demarre, charge le backend et produit une inference dans ce layout." -ForegroundColor Green
