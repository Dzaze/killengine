<#
Client CDP (Chrome DevTools Protocol) minimal, reutilisable, pour piloter la
vraie UI Qt/WebEngine de KillEngine.exe (pas un mock, pas une mutation directe
du store Pinia) depuis un script PowerShell -- meme technique que
[[killengine_cdp_screenshot_technique]] (memoire de session), formalisee ici
pour AM-4 (docs/PHASE_TRACKER.md, 16/09/2026).

KillEngine.exe doit avoir ete lance avec QTWEBENGINE_REMOTE_DEBUGGING=127.0.0.1:<port>
pour qu'un port CDP soit ouvert.

A dot-sourcer :
    . "$PSScriptRoot\lib\cdp-client.ps1"
    $cdp = Connect-KillEngineCdpPage -Port 9420
    Invoke-CdpEval $cdp "document.title" | Write-Host
    Close-CdpSession $cdp
#>

function Connect-KillEngineCdpPage {
    param(
        [Parameter(Mandatory = $true)][int]$Port,
        [int]$TimeoutSec = 20
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $page = $null
    $lastError = $null
    while ((Get-Date) -lt $deadline) {
        try {
            $targets = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/json" -TimeoutSec 3
            $page = $targets | Where-Object { $_.title -eq 'KillEngine' -and $_.type -eq 'page' } | Select-Object -First 1
            if ($page) { break }
        } catch {
            $lastError = $_
        }
        Start-Sleep -Milliseconds 300
    }
    if (-not $page) {
        throw "Aucune page CDP 'KillEngine' trouvee sur le port $Port apres ${TimeoutSec}s. Derniere erreur: $lastError"
    }

    $ws = New-Object System.Net.WebSockets.ClientWebSocket
    $ct = New-Object System.Threading.CancellationToken
    $ws.ConnectAsync([Uri]$page.webSocketDebuggerUrl, $ct).Wait()

    return [pscustomobject]@{
        WebSocket = $ws
        CancellationToken = $ct
        NextId = 1
        Port = $Port
    }
}

function Close-CdpSession {
    param([Parameter(Mandatory = $true)]$Session)
    try {
        if ($Session.WebSocket.State -eq [System.Net.WebSockets.WebSocketState]::Open) {
            $Session.WebSocket.CloseAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure, 'done', $Session.CancellationToken).Wait()
        }
    } catch {
        # best-effort : le process KillEngine peut deja avoir ferme la connexion
    }
}

function Send-CdpRaw {
    param(
        [Parameter(Mandatory = $true)]$Session,
        [Parameter(Mandatory = $true)][string]$Method,
        [hashtable]$Params = @{}
    )
    $id = $Session.NextId
    $Session.NextId += 1
    $payload = @{ id = $id; method = $Method; params = $Params } | ConvertTo-Json -Compress -Depth 8
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($payload)
    $seg = New-Object System.ArraySegment[byte] (, $bytes)
    $Session.WebSocket.SendAsync($seg, [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $Session.CancellationToken).Wait()
    return $id
}

function Receive-CdpResponse {
    param(
        [Parameter(Mandatory = $true)]$Session,
        [Parameter(Mandatory = $true)][int]$ExpectedId,
        [int]$TimeoutSec = 30
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $buffer = New-Object byte[] 1048576
    while ((Get-Date) -lt $deadline) {
        $seg = New-Object System.ArraySegment[byte] (, $buffer)
        $task = $Session.WebSocket.ReceiveAsync($seg, $Session.CancellationToken)
        if (-not $task.Wait(1000)) { continue }
        $result = $task.Result
        $text = [System.Text.Encoding]::UTF8.GetString($buffer, 0, $result.Count)
        $obj = $text | ConvertFrom-Json
        if ($obj.id -eq $ExpectedId) { return $obj }
        # Message CDP non lie a cette requete (evenement asynchrone type
        # Page.frameNavigated) : on l'ignore et on continue d'attendre la bonne reponse.
    }
    throw "Timeout CDP (${TimeoutSec}s) en attendant la reponse id=$ExpectedId"
}

<#
Evalue une expression JS dans la page KillEngine reelle. Par defaut attend la
resolution d'une promesse (awaitPromise) puisque les scenarios AM-4 pilotent
des actions async (attach/scan/etc.). Leve une exception PowerShell si le JS
leve une exception cote page (jamais un echec silencieux).
#>
function Invoke-CdpEval {
    param(
        [Parameter(Mandatory = $true)]$Session,
        [Parameter(Mandatory = $true)][string]$Expression,
        [switch]$NoAwaitPromise,
        [int]$TimeoutSec = 30
    )
    $params = @{
        expression = $Expression
        returnByValue = $true
        awaitPromise = -not $NoAwaitPromise.IsPresent
    }
    $id = Send-CdpRaw -Session $Session -Method 'Runtime.evaluate' -Params $params
    $resp = Receive-CdpResponse -Session $Session -ExpectedId $id -TimeoutSec $TimeoutSec

    if ($resp.result.exceptionDetails) {
        $desc = $resp.result.exceptionDetails.exception.description
        if (-not $desc) { $desc = $resp.result.exceptionDetails.text }
        throw "Exception JS cote page KillEngine: $desc`nExpression: $Expression"
    }

    $value = $resp.result.result.value
    return $value
}

<#
Clique un element reel via son gestionnaire @click Vue (dispatchEvent('click'),
pas de coordonnees ecran) -- suffisant pour les boutons de ce projet (aucun
gestionnaire ne depend de coordonnees pointer/hover). Leve une exception si
l'element est introuvable ou desactive (disabled), pour ne jamais valider
silencieusement un clic qui n'a rien fait.
#>
function Invoke-CdpClick {
    param(
        [Parameter(Mandatory = $true)]$Session,
        [Parameter(Mandatory = $true)][string]$Selector
    )
    $expr = @"
(() => {
    const el = document.querySelector($($Selector | ConvertTo-Json));
    if (!el) return 'ELEMENT_NOT_FOUND';
    if (el.disabled) return 'ELEMENT_DISABLED';
    el.click();
    return 'OK';
})()
"@
    $result = Invoke-CdpEval -Session $Session -Expression $expr -NoAwaitPromise
    if ($result -ne 'OK') {
        throw "Invoke-CdpClick('$Selector') a echoue: $result"
    }
}

<#
Clique le premier element correspondant a $Selector dont le texte visible
contient $Text (utile pour les boutons de nav sans classe unique par item).
#>
function Invoke-CdpClickByText {
    param(
        [Parameter(Mandatory = $true)]$Session,
        [Parameter(Mandatory = $true)][string]$Selector,
        [Parameter(Mandatory = $true)][string]$Text
    )
    $expr = @"
(() => {
    const els = [...document.querySelectorAll($($Selector | ConvertTo-Json))];
    const el = els.find(e => e.textContent && e.textContent.trim().includes($($Text | ConvertTo-Json)));
    if (!el) return 'ELEMENT_NOT_FOUND';
    if (el.disabled) return 'ELEMENT_DISABLED';
    el.click();
    return 'OK';
})()
"@
    $result = Invoke-CdpEval -Session $Session -Expression $expr -NoAwaitPromise
    if ($result -ne 'OK') {
        throw "Invoke-CdpClickByText('$Selector', '$Text') a echoue: $result"
    }
}

<#
Ecrit dans un <input>/<textarea> en passant par le setter natif de la
prototype puis un evenement 'input' (et 'change') reel -- une simple
assignation .value ne declenche PAS la reactivite Vue (v-model ecoute
l'evenement 'input'), deja constate et documente lors de la validation live
R1 (docs/PHASE_TRACKER.md, "remplissage du formulaire ... via le setter natif
+ evenement input/change, pas une simple assignation .value").
#>
function Invoke-CdpSetInputValue {
    param(
        [Parameter(Mandatory = $true)]$Session,
        [Parameter(Mandatory = $true)][string]$Selector,
        [Parameter(Mandatory = $true)][string]$Text
    )
    $expr = @"
(() => {
    const el = document.querySelector($($Selector | ConvertTo-Json));
    if (!el) return 'ELEMENT_NOT_FOUND';
    const proto = el.tagName === 'TEXTAREA' ? window.HTMLTextAreaElement.prototype
        : el.tagName === 'SELECT' ? window.HTMLSelectElement.prototype
        : window.HTMLInputElement.prototype;
    const setter = Object.getOwnPropertyDescriptor(proto, 'value').set;
    setter.call(el, $($Text | ConvertTo-Json));
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
    return 'OK';
})()
"@
    $result = Invoke-CdpEval -Session $Session -Expression $expr -NoAwaitPromise
    if ($result -ne 'OK') {
        throw "Invoke-CdpSetInputValue('$Selector') a echoue: $result"
    }
}

<#
Lit le texte visible (textContent, trim) du premier element correspondant.
Retourne $null si absent (pas une exception -- appele aussi pour verifier
qu'un element a bien DISPARU).
#>
function Get-CdpText {
    param(
        [Parameter(Mandatory = $true)]$Session,
        [Parameter(Mandatory = $true)][string]$Selector
    )
    $expr = "document.querySelector($($Selector | ConvertTo-Json))?.textContent?.trim() ?? null"
    return Invoke-CdpEval -Session $Session -Expression $expr -NoAwaitPromise
}

<#
Repete $Expression jusqu'a ce qu'elle retourne une valeur "truthy" ou que le
delai soit ecoule -- jamais une boucle sans borne (chaque scenario AM-4 doit
echouer avec un diagnostic clair plutot que de bloquer indefiniment).
#>
function Wait-CdpCondition {
    param(
        [Parameter(Mandatory = $true)]$Session,
        [Parameter(Mandatory = $true)][string]$Expression,
        [int]$TimeoutSec = 15,
        [int]$PollMs = 300,
        [string]$Description = $Expression
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $last = $null
    while ((Get-Date) -lt $deadline) {
        $last = Invoke-CdpEval -Session $Session -Expression $Expression -NoAwaitPromise
        if ($last) { return $last }
        Start-Sleep -Milliseconds $PollMs
    }
    throw "Timeout (${TimeoutSec}s) en attendant: $Description (derniere valeur: $last)"
}

function Save-CdpScreenshot {
    param(
        [Parameter(Mandatory = $true)]$Session,
        [Parameter(Mandatory = $true)][string]$Path
    )
    $id = Send-CdpRaw -Session $Session -Method 'Page.captureScreenshot' -Params @{ format = 'png' }
    $resp = Receive-CdpResponse -Session $Session -ExpectedId $id -TimeoutSec 15
    if (-not $resp.result.data) {
        Write-Warning "Save-CdpScreenshot: pas de donnees image retournees."
        return
    }
    $bytes = [Convert]::FromBase64String($resp.result.data)
    [System.IO.File]::WriteAllBytes($Path, $bytes)
}
