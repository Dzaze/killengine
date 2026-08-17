# Patch P1-5 : ExpertView.vue - bouton "Lu par" (Find What Accesses) + panneau scan groupe + watch chain
$ErrorActionPreference = 'Stop'

function Fix-Crlf([string]$s) { return $s.Replace("`r`n", "`n").Replace("`n", "`r`n") }

$f = 'ui\src\views\ExpertView.vue'
$bytes = [System.IO.File]::ReadAllBytes($f)
$hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
$text = [System.Text.Encoding]::UTF8.GetString($bytes)
if ($hasBom -and $text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }

if ($text -notmatch 'runFindWhatAccesses') {

    # ---- 1. Script : fonctions findWhatAccesses apres runFindWhatWrites (ancre : fin de runFindWhatWrites) ----
    # runFindWhatWrites se termine par le fallback synchrone controller.findWhatWrites. Cherchons une ancre sure :
    $anchorFn = '  } catch (e) {'
    # Trop generique. Utilisons l'ancre du bouton findWhatWritesForUiString a la place : inserons les fonctions avant elle.
    $anchorFnDef = 'async function findWhatWritesForUiString(candidate: UiStringCandidate) {'
    if (-not $text.Contains($anchorFnDef)) { throw "Ancre findWhatWritesForUiString introuvable" }

    $fnBlock = Fix-Crlf @'
// ---- Find What Accesses (P1) : instructions qui LISSENT l'adresse ----
const findWhatAccessesResult = ref<Record<string, unknown> | null>(null)
const findWhatAccessesBusy = ref(false)

async function runFindWhatAccesses(address: string, options: Record<string, unknown>) {
  if (!await store.confirmRiskAction('debug', 'Find what accesses', 'Adresse 0x' + address.replace(/^0x/i, '') + ', timeout ' + String(options.timeoutMs ?? '?') + ' ms.')) {
    return { success: false, hitCount: 0, hits: [], cancelled: true, error: 'Capture debugger annulee par l utilisateur.' }
  }
  const controller = backend.getController()
  const fn = controller.findWhatAccessesAsync
  const sig = controller.findWhatAccessesFinished
  if (!fn || !sig) {
    return { success: false, hitCount: 0, hits: [], error: 'Find What Accesses non disponible dans ce backend.' }
  }
  return new Promise<Record<string, unknown>>((resolve) => {
    let requestId: number | null = null
    let settled = false
    const earlyPayloads: Array<Record<string, unknown>> = []
    const timeout = window.setTimeout(() => {
      settled = true
      sig.disconnect?.(handler)
      resolve({ success: false, hitCount: 0, hits: [], error: 'Timeout de la capture Find What Accesses.' })
    }, 20000)

    const handler = (payload: Record<string, unknown>) => {
      if (requestId === null) {
        earlyPayloads.push(payload)
        return
      }
      if (Number(payload.requestId) !== requestId) return
      settled = true
      window.clearTimeout(timeout)
      sig.disconnect?.(handler)
      resolve(payload)
    }
    sig.connect(handler)

    void fn(address, options).then((start) => {
      if (settled) return
      if (start.success !== true || start.started !== true) {
        settled = true
        window.clearTimeout(timeout)
        sig.disconnect?.(handler)
        resolve({ success: false, hitCount: 0, hits: [], error: String(start.error ?? 'Impossible de demarrer Find What Accesses async.') })
        return
      }
      requestId = Number(start.requestId)
      for (const payload of earlyPayloads.splice(0)) {
        handler(payload)
        if (settled) break
      }
    }).catch((error) => {
      if (settled) return
      settled = true
      window.clearTimeout(timeout)
      sig.disconnect?.(handler)
      resolve({ success: false, hitCount: 0, hits: [], error: String(error) })
    })
  })
}

async function findWhatAccessesForSource(candidate: UiStringSourceCandidate) {
  if (!findWhatWritesAcknowledged.value) {
    findWhatAccessesResult.value = { success: false, hitCount: 0, hits: [], error: 'Active le consentement debugger avant de lancer Lu par.' }
    return
  }
  findWhatAccessesBusy.value = true
  findWhatAccessesResult.value = null
  try {
    findWhatAccessesResult.value = await runFindWhatAccesses(candidate.address, {
      size: findWhatWritesSizeForType(candidate.type),
      timeoutMs: findWhatWritesTimeoutMs.value,
      maxHits: 12,
    })
  } catch (e) {
    findWhatAccessesResult.value = { success: false, hitCount: 0, hits: [], error: String(e) }
  } finally {
    findWhatAccessesBusy.value = false
  }
}

'@
    $text = $text.Replace($anchorFnDef, $fnBlock + $anchorFnDef)

    # ---- 2. Template : bouton "Lu par" a cote du bouton ecrit par (ligne 2859 zone sources) ----
    $anchorBtn = Fix-Crlf @'
@click="findWhatWritesForSource(candidate.source)">&#65533;%crit par</button>
'@
    # Ligne avec caractere special - utilisons une ancre ASCII partielle differente.
    # Bouton ligne 3007 : @click="findWhatWritesForSource(candidate)">
    $anchorBtn2 = '@click="findWhatWritesForSource(candidate)">'
    if (-not $text.Contains($anchorBtn2)) { throw "Ancre bouton findWhatWritesForSource(candidate) introuvable" }
    # Trouver la fin de la ligne (le </button> fermant) puis inserer apres
    $idx = $text.IndexOf($anchorBtn2)
    $btnEnd = $text.IndexOf('</button>', $idx)
    if ($btnEnd -lt 0) { throw "Fin de bouton introuvable" }
    $insertPos = $btnEnd + '</button>'.Length
    $btnLuPar = Fix-Crlf @'
 <button class="btn btn-secondary compact" type="button" :disabled="findWhatAccessesBusy || !findWhatWritesAcknowledged" @click="findWhatAccessesForSource(candidate)">Lu par</button>
'@
    $btnLuPar = $btnLuPar.TrimEnd("`r`n")
    $text = $text.Substring(0, $insertPos) + $btnLuPar + $text.Substring($insertPos)

    # ---- 3. Template : affichage resultat findWhatAccesses apres findWhatWritesResult (section existante) ----
    # Inserrons un bloc compact sous la section Trace UI. Cherchons le bloc d'affichage findWhatWritesResult principal
    $anchorRes = 'v-if="findWhatWritesResult"'
    if ($text.Contains($anchorRes)) {
        # Trouver le </section> ou </div> apres - trop risqu. Affichons le resultat dans le panneau scan groupe a la place.
    }

    # ---- 4. Panneau Scan groupe + Watch chains : avant la section "Journal utilisateur" ----
    $anchorSection = '<section v-if="expertDense" class="panel">'
    if (-not $text.Contains($anchorSection)) { throw "Ancre section expertDense introuvable" }
    $idxSec = $text.IndexOf($anchorSection)
    $panel = Fix-Crlf @'
<section v-if="expertDense" class="panel">
        <div class="panel-title">
          <h2>Scan groupe</h2>
          <span>{{ store.groupScanResult ? store.groupScanResult.matchesFound + ' structure(s)' : 'valeurs voisines' }}</span>
        </div>
        <p class="hint">Cherche N valeurs avec offsets fixes connus (ex: HP=100 a +0, Mana=50 a +4). Trouve la base de la structure.</p>
        <div class="group-scan-entries">
          <div v-for="(entry, index) in store.groupScanEntries" :key="index" class="group-scan-row">
            <input v-model="entry.offset" class="input-mini" type="text" placeholder="0" spellcheck="false" />
            <select v-model="entry.type" class="input-mini">
              <option v-for="t in valueTypeOptions" :key="t" :value="t">{{ t }}</option>
            </select>
            <input v-model="entry.value" class="input-mini grow" type="text" placeholder="valeur" spellcheck="false" />
            <button class="btn btn-secondary compact" type="button" :disabled="store.groupScanEntries.length <= 2" @click="store.removeGroupScanEntry(index)">x</button>
          </div>
        </div>
        <div class="row-actions">
          <button class="btn btn-secondary compact" type="button" @click="store.addGroupScanEntry()">+ valeur</button>
          <button class="btn btn-secondary compact" type="button" @click="store.clearGroupScanEntries()">Reset</button>
          <label class="hint">distance max</label>
          <input v-model.number="store.groupScanMaxDistance" class="input-mini" type="number" min="4" max="4096" />
          <button
            class="btn btn-primary compact"
            type="button"
            :disabled="store.groupScanBusy || store.scanBusy || !store.isAttached"
            @click="store.doGroupScan()"
          >
            <span v-if="store.groupScanBusy" class="btn-spinner" aria-hidden="true"></span>
            <span>{{ store.groupScanBusy ? 'Scan...' : 'Scanner groupe' }}</span>
          </button>
        </div>
        <div v-if="store.groupScanResult" class="metrics">
          <span>Trouves: {{ store.groupScanResult.matchesFound }}</span>
          <span v-if="store.groupScanResult.partial">Partiel</span>
          <span v-if="store.groupScanResult.elapsedMs">Temps: {{ store.groupScanResult.elapsedMs }} ms</span>
        </div>
        <div v-if="store.groupScanResult?.matches?.length" class="group-scan-results">
          <div v-for="match in store.groupScanResult.matches.slice(0, 20)" :key="match.address" class="group-scan-result-row">
            <code>0x{{ match.address }}</code>
            <span>{{ match.variantLabel }}</span>
            <span v-if="match.confidence" class="hint">{{ Math.round(match.confidence * 100) }}%</span>
            <button class="btn btn-secondary compact" type="button" @click="store.addAddressToWatch({ address: match.address, type: match.type })">Watch</button>
          </div>
        </div>
        <p v-if="store.groupScanResult?.error" class="error">{{ store.groupScanResult.error }}</p>
        <p v-if="findWhatAccessesResult" :class="findWhatAccessesResult.success ? 'hint' : 'error'">
          Lu par : {{ findWhatAccessesResult.hitCount ?? 0 }} acces - {{ findWhatAccessesResult.error ?? '' }}
        </p>
      </section>

      <section v-if="expertDense" class="panel">
        <div class="panel-title">
          <h2>Watch chaines de pointeurs</h2>
          <span>{{ store.watchedPointerChains.length }} chaine(s)</span>
          <button class="btn btn-secondary compact" type="button" :disabled="store.watchedPointerChains.length === 0" @click="store.refreshWatchedPointerChains()">Rafraichir</button>
          <button class="btn btn-secondary compact" type="button" :disabled="store.watchedPointerChains.length === 0" @click="store.clearWatchedPointerChains()">Vider</button>
        </div>
        <div v-if="store.watchedPointerChains.length === 0" class="hint">Teste une chaine de pointeurs puis clique Watch pour la surveiller en live.</div>
        <div v-else class="watch-list">
          <div v-for="chain in store.watchedPointerChains" :key="chain.id" class="watch-row" :class="{ changed: chain.changed }">
            <strong>{{ chain.label }}</strong>
            <code>0x{{ chain.finalAddress }}</code>
            <span>{{ chain.type }}</span>
            <strong>{{ chain.value }}</strong>
            <span v-if="chain.error" class="error">{{ chain.error }}</span>
            <button class="btn btn-secondary compact" type="button" @click="store.removeWatchedPointerChain(chain.id)">x</button>
          </div>
        </div>
      </section>

      <section v-if="expertDense" class="panel">
'@
    $text = $text.Substring(0, $idxSec) + $panel + $text.Substring($idxSec + $anchorSection.Length)

    $encoding = New-Object System.Text.UTF8Encoding($hasBom)
    [System.IO.File]::WriteAllText($f, $text, $encoding)
    Write-Host "OK: ExpertView.vue patche (Lu par + scan groupe + watch chains)"
} else {
    Write-Host "SKIP: runFindWhatAccesses deja present"
}

# ---- Styles additionnels ----
$text2 = [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes($f))
if ($text2 -notmatch 'group-scan-row') {
    # ajouter les styles a la fin du <style scoped> (avant le dernier </style>)
    $lastStyle = $text2.LastIndexOf('</style>')
    if ($lastStyle -lt 0) { throw "Balise style introuvable" }
    $styles = Fix-Crlf @'
<style scoped>
/* P1 : scan groupe + watch chains */
.group-scan-entries {
  display: flex;
  flex-direction: column;
  gap: 4px;
  margin: 6px 0;
}

.group-scan-row {
  display: flex;
  gap: 6px;
  align-items: center;
}

.group-scan-row .input-mini {
  width: 90px;
  padding: 4px 6px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-size: 12px;
}

.group-scan-row .grow {
  flex: 1;
  min-width: 0;
}

.group-scan-results {
  display: flex;
  flex-direction: column;
  gap: 4px;
  max-height: 220px;
  overflow-y: auto;
  margin-top: 6px;
}

.group-scan-result-row {
  display: flex;
  gap: 8px;
  align-items: center;
  font-size: 12px;
}

.row-actions {
  display: flex;
  gap: 6px;
  align-items: center;
  margin-top: 6px;
}

.row-actions .input-mini {
  width: 70px;
  padding: 4px 6px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-size: 12px;
}
</style>
'@
    # Remplacer le dernier </style> fermant : on insere les styles AVANT le </style> existant
    $insertStyles = $styles.Substring('<style scoped>'.Length).TrimEnd()
    $insertStyles = $insertStyles -replace '</style>\s*$', ''
    $text2 = $text2.Substring(0, $lastStyle) + $insertStyles + "`r`n" + $text2.Substring($lastStyle)
    $encoding = New-Object System.Text.UTF8Encoding($hasBom)
    [System.IO.File]::WriteAllText($f, $text2, $encoding)
    Write-Host "OK: styles scan groupe ajoutes"
}