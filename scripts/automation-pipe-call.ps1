<#
Client JSON-RPC minimal pour le connecteur d'automatisation local de KillEngine
(apps/desktop/automation_pipe_server.h). Envoie UNE requete, lit UNE reponse,
ferme la connexion. KillEngine doit avoir ete lance avec la variable
d'environnement KILLENGINE_AUTOMATION_PIPE=1 pour que le pipe existe.

Usage:
  .\scripts\automation-pipe-call.ps1 -Method ping -ParamsJson '["hello"]'
  .\scripts\automation-pipe-call.ps1 -Method attachProcess -ParamsJson '[12345]'
  .\scripts\automation-pipe-call.ps1 -Method getProcesses -ParamsJson '[]'

Gros payloads (ex: reinjecter la liste de candidats scanUiStrings dans
trackUiStringCandidates) : -ParamsJsonFile evite la limite de longueur de la
ligne de commande, -OutFile evite de faire transiter une grosse reponse par la
sortie standard (utile pour la garder hors du contexte d'un agent qui pilote
ce script).
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$Method,

    [string]$ParamsJson = '[]',

    [string]$ParamsJsonFile,

    [string]$OutFile,

    [string]$PipeName = 'KillEngineAutomationPipe',

    [int]$TimeoutMs = 5000
)

$ErrorActionPreference = 'Stop'

$rawParamsJson = if ($ParamsJsonFile) { Get-Content -Path $ParamsJsonFile -Raw } else { $ParamsJson }

try {
    $paramsObject = $rawParamsJson | ConvertFrom-Json
} catch {
    Write-Error "ParamsJson invalide (doit etre un tableau JSON, ex: '[1234]' ou '[]') : $_"
    exit 1
}

$requestObject = [ordered]@{
    id     = [int](Get-Random -Maximum 1000000)
    method = $Method
    # ConvertFrom-Json "deballe" un tableau JSON a un seul element en simple
    # scalaire (piege connu de PowerShell) -- @(...) force la reconversion en
    # tableau pour que ConvertTo-Json reserialise bien "params" comme [...].
    params = @($paramsObject)
}
$requestJson = $requestObject | ConvertTo-Json -Compress -Depth 10

$pipeClient = New-Object System.IO.Pipes.NamedPipeClientStream('.', $PipeName, [System.IO.Pipes.PipeDirection]::InOut)
try {
    $pipeClient.Connect($TimeoutMs)
} catch {
    Write-Error "Connexion au pipe '$PipeName' echouee (KillEngine est-il lance avec KILLENGINE_AUTOMATION_PIPE=1 ?) : $_"
    exit 1
}

$writer = New-Object System.IO.StreamWriter($pipeClient)
$writer.AutoFlush = $true
$reader = New-Object System.IO.StreamReader($pipeClient)

try {
    $writer.WriteLine($requestJson)
    $responseLine = $reader.ReadLine()
    if ($OutFile) {
        # Set-Content -Encoding utf8NoBOM n'existe qu'a partir de PowerShell 7 ;
        # cette methode .NET evite l'ecart de comportement entre powershell.exe
        # (5.1) et pwsh (7+) tout en ecrivant sans BOM dans les deux cas.
        [System.IO.File]::WriteAllText($OutFile, $responseLine, (New-Object System.Text.UTF8Encoding($false)))
        Write-Output "OK -> $OutFile ($($responseLine.Length) caracteres)"
    } else {
        Write-Output $responseLine
    }
} finally {
    # StreamReader/StreamWriter.Dispose() ferme deja le stream sous-jacent
    # (leaveOpen=false par defaut) : ne disposer que $pipeClient ici, sinon le
    # deuxieme Dispose() explicite leve "Cannot access a closed pipe".
    $pipeClient.Dispose()
}
