# Patch U2: améliore exportInvestigationMarkdown (top 5 checkpoints scorés + nextBestAction + garde-fous)
$ErrorActionPreference = 'Stop'
$path = 'ui\src\stores\app.ts'
$content = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)

# Localiser l'ancien bloc "## Checkpoints" ... jusqu'à "## Bilan Auto" dans exportInvestigationMarkdown
$startMarker = "      '## Checkpoints',"
$bilanMarker = "      '## Bilan Auto',"
$startIdx = $content.IndexOf($startMarker)
if ($startIdx -lt 0) { Write-Host 'START MARKER NOT FOUND'; exit 1 }
$bilanIdx = $content.IndexOf($bilanMarker, $startIdx)
if ($bilanIdx -lt 0) { Write-Host 'BILAN MARKER NOT FOUND'; exit 1 }

$bt = [char]96  # backtick
$newBlock = @"
      '## Checkpoints (Top 5 scores)',
      ...run.checkpoints
        .slice()
        .sort((a, b) => Number(b.confidenceScore ?? 0) - Number(a.confidenceScore ?? 0))
        .slice(0, 5)
        .map((checkpoint) => `- `${String(checkpoint.label ?? checkpoint.address ?? checkpoint.id ?? 'checkpoint')}` `${String(checkpoint.kind ?? 'checkpoint')}``${checkpoint.confidenceScore != null ? ` (score ${String(checkpoint.confidenceScore)}${checkpoint.confidenceLabel ? ` ${bt}${String(checkpoint.confidenceLabel)}${bt} : ''})` : ''}${checkpoint.requiresConfirmation === true ? ' [confirmation requise]' : ' [safe]'}`),
      '',
      '## Meilleure prochaine action',
      ...(() => {
        const next = autoResolveReport.value?.nextBestAction
        if (next && typeof next === 'object') {
          return [
            `Action: `${String(next.label ?? next.id ?? 'non definie')}``,
            `Outil: `${String(next.tool ?? next.id ?? '-')}``,
            `Risque: `${String(next.risk ?? 'safe')}``,
            `Confiance: `${String(next.confidence ?? '-')}``,
            `Raison: `${String(next.reason ?? '')}``,
          ]
        }
        const top = run.checkpoints
          .slice()
          .sort((a, b) => Number(b.confidenceScore ?? 0) - Number(a.confidenceScore ?? 0))[0]
        if (top) {
          return [
            `Action (checkpoint top score): `${String(top.label ?? top.address ?? 'checkpoint')}``,
            `Outil: `${checkpointIsActionable(top) ? 'valider le checkpoint' : 'continuer la reduction'}``,
            `Risque: `${top.requiresConfirmation === true ? 'confirmation requise' : 'safe'}``,
            `Confiance: `${String(top.confidenceScore ?? '-')} `${String(top.confidenceLabel ?? '')}``.trim(),
            `Raison: `${String(top.reason ?? 'meilleur checkpoint score de l'enquete')}``,
          ]
        }
        return ['Aucune action determinable pour l instant.']
      })(),
      '',
      '## Garde-fous actifs',
      ...(() => {
        const rails = autoResolveReport.value?.guardrails ?? []
        if (!Array.isArray(rails) || rails.length === 0) return ['- Aucun garde-fou specifique remonte (mode Safe actif par defaut).']
        return rails.map((rail) => `- `${String(rail.label ?? rail.id ?? 'garde-fou')}`: `${String(rail.reason ?? '')}``.trim())
      })(),
      '',
"@

$newBlock = $newBlock.Replace('${bt}', [string][char]96)

# Le here-string contient des backticks PowerShell d'échappement ? Non: double-quote here-string garde les backticks littéraux sauf ` variable.
# En double-quoted here-string, le backtick seul avant un caractère non spécial reste littéral, mais `` est un backtick échappé.
# On a écrit `` (double) pour insérer un backtick littéral dans TS. Vérifions et remplaçons les paires par un seul:
$newBlock = $newBlock -replace '``', [string][char]96

$oldBlock = $content.Substring($startIdx, $bilanIdx - $startIdx)
$content = $content.Replace($oldBlock, $newBlock + "`r`n")
[System.IO.File]::WriteAllText($path, $content, (New-Object System.Text.UTF8Encoding($false)))
Write-Host 'exportInvestigationMarkdown patched'