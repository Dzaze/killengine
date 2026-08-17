$ErrorActionPreference = 'Stop'

# Test completion contre le llama-server persistant avec prompt anti-think
$prompt = @'
Tu es le planner local de KillEngine, un moteur de recherche memoire type cheat engine.
Reponds uniquement avec un objet JSON compact et rien d'autre. Ne raisonnes pas, pas de bloc <think>, pas d'explication: uniquement le JSON final.
Schema obligatoire: {"tool":"exact_scan|next_scan","args":{...}}
Exemple: j'ai 41250 argent => {"tool":"exact_scan","args":{"value":"41250","valueType":"Int32"}}
Requete utilisateur: j'ai 41250 argent
'@

$payload = @{
    prompt = $prompt
    n_predict = 384
    temperature = 0
    cache_prompt = $true
    stream = $false
} | ConvertTo-Json

$sw = [System.Diagnostics.Stopwatch]::StartNew()
try {
    $r = Invoke-WebRequest -Uri 'http://127.0.0.1:8827/completion' -Method Post -Body ([System.Text.Encoding]::UTF8.GetBytes($payload)) -ContentType 'application/json' -UseBasicParsing -TimeoutSec 120
    $json = $r.Content | ConvertFrom-Json
    $sw.Stop()
    Write-Output ('TOTAL_REQUEST_MS: ' + $sw.ElapsedMilliseconds)
    Write-Output ('CONTENT: ' + $json.content.Substring(0, [Math]::Min(400, $json.content.Length)))
    Write-Output ('TIMINGS prompt_ms: ' + [math]::Round($json.timings.prompt_ms) + ' predicted_ms: ' + [math]::Round($json.timings.predicted_ms) + ' tokens_predicted: ' + $json.tokens_predicted + ' tokens_cached: ' + $json.tokens_cached)
} catch {
    Write-Output ('FAILED: ' + $_.Exception.Message)
}