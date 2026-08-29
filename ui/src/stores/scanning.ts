/**
 * KillEngine — store Scanning/Candidates (extrait de app.ts, sous-ensemble
 * isolé du candidat S11 de docs/REFACTOR_ROADMAP.md, PHASE 224, 30/08/2026).
 *
 * S11 complet ("Scanning / Candidates") est annoté couplage "Moyen" avec S7
 * (write/freeze) dans le roadmap — vérifié en pratique : `finalCandidateTargets`/
 * `ignoredCandidateAddresses`/`keptCandidateAddresses`/`freezeCandidateCurrent`
 * (pipeline d'écriture) et tout ce qui est piloté par le chat/IA
 * (`runAutoEncryptedScan`, `runAutoUnknownObservation`, `runAutoTraceUiString`,
 * `inferUnknownModeFromObservation` — cluster C14/S10, jamais scindé entre
 * agents) restent dans `app.ts`. Ce store ne couvre QUE la mécanique de scan
 * pure (exact/next/unknown/groupe/chiffré) et la pagination/sélection de
 * candidats pour AFFICHAGE — pas la promotion vers une cible d'écriture.
 *
 * Une seule dépendance transversale existait avant l'extraction :
 * `addAddressToWatch` (domaine "Watch", pas encore extrait, reste dans
 * `app.ts`) était appelée directement par `refreshCandidates`/`selectCandidate`
 * pour peupler automatiquement le watch live quand peu de candidats restent.
 * Comme ce store ne peut pas importer depuis `./app` (dépendance circulaire —
 * `app.ts` importe déjà ce fichier), ce callback est injecté une seule fois via
 * `configureCandidateWatchNotifier` plutôt que threadé à travers chaque appel
 * (mode "réglé une fois", différent du callback par-appel de `riskGate.ts`/
 * `settings.ts` car son identité ne change jamais d'un appel à l'autre).
 *
 * `syncScanDefaultsFromSettings` dépend de `settings.ts` (déjà une feuille
 * sans dépendance vers `./app`) — dépendance store-vers-store à sens unique,
 * pas circulaire.
 */
import { defineStore, storeToRefs } from 'pinia'
import { ref } from 'vue'
import {
  backend,
  type CandidatePage,
  type EncryptedScanResult,
  type ExactScanResult,
  type NextScanResult,
  type UndoCandidateScanResult,
  type UnknownNextScanResult,
  type UnknownSnapshotResult,
} from '@/services/backend'
import { useActionLogStore } from './actionLog'
import { useSettingsStore } from './settings'

interface GroupScanEntryInput {
  offset: number
  type: string
  value: string
}

export interface UnknownGuideStep {
  id: number
  time: string
  mode: string
  label: string
  beforeCount: number
  afterCount: number
  status: 'capture' | 'compare' | 'refine' | 'error'
  detail: string
}

type CandidateWatchNotifier = (address: string, type: string) => void

export const useScanningStore = defineStore('scanning', () => {
  const actionLogStore = useActionLogStore()
  const { addActionLog } = actionLogStore
  const settingsStore = useSettingsStore()
  const { settingDefaultValueType, settingUnknownSnapshotMaxMb } = storeToRefs(settingsStore)

  // Domaine "Watch" pas encore extrait (reste dans app.ts) -- voir commentaire
  // d'en-tête. null tant qu'app.ts ne l'a pas configuré (tests/usage isolé de
  // ce store restent valides sans, juste sans peuplement auto du watch).
  let notifyCandidateForWatch: CandidateWatchNotifier | null = null
  function configureCandidateWatchNotifier(fn: CandidateWatchNotifier) {
    notifyCandidateForWatch = fn
  }

  function nowTime(): string {
    return new Date().toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })
  }

  function formatCount(value: number | undefined): string {
    return new Intl.NumberFormat('fr-FR').format(value ?? 0)
  }

  const exactScanValue = ref('')
  const exactScanType = ref('Int32')
  const exactScanResult = ref<ExactScanResult | null>(null)
  const encryptedScanResult = ref<EncryptedScanResult | null>(null)
  const encryptedScanMode = ref<'xor' | 'add' | 'sub' | 'not'>('xor')
  const encryptedScanKey = ref('0')
  const encryptedScanKeySearchBits = ref(0)

  const groupScanEntries = ref<Array<{ offset: string, type: string, value: string }>>([
    { offset: '0', type: 'Int32', value: '' },
    { offset: '4', type: 'Int32', value: '' },
  ])
  const groupScanResult = ref<EncryptedScanResult | null>(null)
  const groupScanBusy = ref(false)
  const groupScanMaxDistance = ref(64)

  // Filtres de region pour le Mode Expert, consommes par le scan exact/groupe/
  // chiffre (hasExpertFilter dans doExactScan). expertModeEnabled/adresses
  // restent aussi lus/ecrits depuis app.ts par les fonctions glue qui restent
  // la-bas (scanAroundPreview, openExpertAtAddress, openExpertForRegion) --
  // storeToRefs cote app.ts garde ces refs partagees sous le meme nom.
  const expertModeEnabled = ref(false)
  const expertStartAddress = ref('')
  const expertStopAddress = ref('')
  const expertAlignment = ref(0)
  const expertWritableOnly = ref(false)
  const expertExecutableOnly = ref(false)
  const expertCopyOnWriteOnly = ref(false)
  const expertRegionSize = ref(0)
  const expertRegionProtection = ref('')
  const expertRegionState = ref('')
  const expertRegionType = ref('')

  const candidatePage = ref<CandidatePage | null>(null)
  const candidatePageIndex = ref(0)
  const candidatePageSize = ref(100)
  const candidateFilter = ref('')
  const nextScanMode = ref('exact')
  const nextScanValue = ref('')
  const nextScanResult = ref<NextScanResult | null>(null)
  const undoCandidateScanResult = ref<UndoCandidateScanResult | null>(null)
  const unknownScanMode = ref('changed')
  const unknownScanType = ref('Auto')
  const unknownWritableOnly = ref(true)
  const unknownCopyOnWriteOnly = ref(false)
  const unknownSnapshotResult = ref<UnknownSnapshotResult | null>(null)
  const unknownNextScanResult = ref<UnknownNextScanResult | null>(null)
  const unknownGuideSteps = ref<UnknownGuideStep[]>([])
  const autoUnknownAwaitingObservation = ref(false)
  const unknownGuideStepIdCounter = ref(0)
  const selectedCandidateAddress = ref('')
  const candidateHistory = ref<number[]>([])
  const scanBusy = ref(false)
  const scanStatusText = ref('')
  const scanProgressPercent = ref(0)

  function setScanProgress(percent: number) {
    if (!Number.isFinite(percent)) return
    scanProgressPercent.value = Math.max(0, Math.min(100, Math.round(percent)))
  }

  function unknownModeLabel(mode: string): string {
    if (mode === 'increased') return 'ça augmente'
    if (mode === 'decreased') return 'ça diminue'
    if (mode === 'unchanged') return 'stable'
    if (mode === 'changed') return 'ça change'
    return mode
  }

  function pushUnknownGuideStep(step: Omit<UnknownGuideStep, 'id' | 'time'>) {
    unknownGuideStepIdCounter.value += 1
    unknownGuideSteps.value.unshift({
      id: unknownGuideStepIdCounter.value,
      time: nowTime(),
      ...step,
    })
    unknownGuideSteps.value = unknownGuideSteps.value.slice(0, 12)
  }

  function extractCandidateCount(result: Record<string, unknown>): number | undefined {
    if (typeof result.candidateStoreSize === 'number') return result.candidateStoreSize
    const ar = result.actionResult as Record<string, unknown> | undefined
    if (ar) {
      if (typeof ar.remaining === 'number') return ar.remaining
      if (typeof ar.candidateStoreSize === 'number') return ar.candidateStoreSize
      if (typeof ar.stored === 'number') return ar.stored
    }
    return undefined
  }

  function syncScanDefaultsFromSettings() {
    exactScanType.value = settingDefaultValueType.value
    unknownScanType.value = 'Auto'
  }

  async function refreshCandidates(notifyWatch: CandidateWatchNotifier | null = notifyCandidateForWatch) {
    try {
      candidatePage.value = await backend
        .getController()
        .getCandidates(candidatePageIndex.value, candidatePageSize.value, candidateFilter.value)
      if (
        candidatePage.value
        && !candidatePage.value.displaySuppressed
        && candidatePage.value.totalCount > 0
        && candidatePage.value.totalCount <= 20
      ) {
        for (const candidate of candidatePage.value.candidates) {
          notifyWatch?.(candidate.address, candidate.type)
        }
      }
    } catch (e) {
      console.error('[KillEngine] Failed to get candidates:', e)
      candidatePage.value = null
    }
  }

  async function doExactScan() {
    if (!exactScanValue.value.trim() || scanBusy.value) return

    // Le mode Auto (multi-type) est crucial pour les cibles modernes : il cherche
    // Int/UInt, Float et variantes fixed-point en une fois.
    const isAutoType = exactScanType.value.toLowerCase() === 'auto'

    // Si le Mode Expert est activé et qu'au moins un filtre est défini, on utilise l'API expert.
    const hasExpertFilter =
      expertModeEnabled.value
      && (expertStartAddress.value.trim()
        || expertStopAddress.value.trim()
        || expertAlignment.value > 0
        || expertWritableOnly.value
        || expertExecutableOnly.value
        || expertCopyOnWriteOnly.value)
    try {
      scanBusy.value = true
      setScanProgress(0)
      scanStatusText.value = isAutoType ? 'Scan multi-type en cours...' : 'Scan exact en cours...'
      addActionLog('scan', `Scan ${isAutoType ? 'multi-type' : 'exact'} ${exactScanValue.value}`, `${exactScanType.value}${hasExpertFilter ? ' · filtres expert actifs' : ''}.`, 'info')

      if (isAutoType) {
        const controller = backend.getController()
        if (controller.startExactScanMultiType) {
          exactScanResult.value = await controller.startExactScanMultiType(
            exactScanValue.value,
            exactScanType.value,
          )
        } else {
          // Fallback : si le backend n'expose pas le scan multi-type, on utilise le scan simple.
          exactScanResult.value = await backend.startExactScanAsync(
            exactScanValue.value,
            'Int32',
            {},
          )
        }
      } else {
        exactScanResult.value = await backend.startExactScanAsync(
          exactScanValue.value,
          exactScanType.value,
          hasExpertFilter
            ? {
                startAddress: expertStartAddress.value.trim() || undefined,
                stopAddress: expertStopAddress.value.trim() || undefined,
                alignment: expertAlignment.value > 0 ? expertAlignment.value : undefined,
                writableOnly: expertWritableOnly.value,
                executableOnly: expertExecutableOnly.value,
                copyOnWriteOnly: expertCopyOnWriteOnly.value,
              }
            : {},
        )
      }
      setScanProgress(Math.max(scanProgressPercent.value, 95))
      candidatePageIndex.value = 0
      scanStatusText.value = 'Chargement des candidats...'
      await refreshCandidates()
      setScanProgress(100)
      scanStatusText.value = exactScanResult.value.cancelled ? 'Scan annulé.' : 'Scan terminé.'
      addActionLog(
        'scan',
        exactScanResult.value.cancelled ? 'Scan exact annulé' : 'Scan exact terminé',
        `${exactScanResult.value.candidateStoreSize} candidat(s), ${exactScanResult.value.regionsScanned} région(s).`,
        exactScanResult.value.success ? 'success' : 'warning',
      )
    } catch (e) {
      exactScanResult.value = {
        success: false,
        partial: false,
        cancelled: false,
        regionsScanned: 0,
        bytesScanned: 0,
        matchesFound: 0,
        matchesReturned: 0,
        error: String(e),
        matches: [],
        candidateStoreSize: 0,
      }
      scanStatusText.value = 'Scan échoué.'
      addActionLog('scan', 'Scan exact échoué', String(e), 'error')
    } finally {
      scanBusy.value = false
    }
  }

  async function doGroupScan() {
    if (scanBusy.value) return
    const controller = backend.getController()
    if (!controller.scanGroupScan) {
      groupScanResult.value = {
        success: false, partial: false, regionsScanned: 0, bytesScanned: 0,
        matchesFound: 0, matchesReturned: 0,
        error: 'Scan groupe non exposé par ce backend.', matches: [],
      }
      addActionLog('scan', 'Scan groupe indisponible', groupScanResult.value.error, 'warning')
      return
    }
    const entries = groupScanEntries.value
      .filter((entry) => entry.value.trim() !== '' && entry.offset.trim() !== '')
      .map((entry): GroupScanEntryInput => ({ offset: Number(entry.offset), type: entry.type, value: entry.value.trim() }))
    if (entries.length < 2) {
      groupScanResult.value = {
        success: false, partial: false, regionsScanned: 0, bytesScanned: 0,
        matchesFound: 0, matchesReturned: 0,
        error: 'Renseigne au moins 2 valeurs avec leurs offsets.', matches: [],
      }
      return
    }

    try {
      scanBusy.value = true
      groupScanBusy.value = true
      setScanProgress(0)
      scanStatusText.value = 'Scan groupe en cours...'
      addActionLog('scan', `Scan groupe (${entries.length} valeurs)`, entries.map((e) => `+${e.offset}:${e.value}`).join(' '), 'info')
      groupScanResult.value = await controller.scanGroupScan(entries, {
        maxDistance: groupScanMaxDistance.value,
        maxResults: 1000,
        startAddress: expertStartAddress.value.trim() || undefined,
        stopAddress: expertStopAddress.value.trim() || undefined,
        alignment: expertAlignment.value > 0 ? expertAlignment.value : undefined,
        writableOnly: true,
      })
      setScanProgress(1)
      if (groupScanResult.value.success) {
        addActionLog('scan', 'Scan groupe terminé', `${groupScanResult.value.matchesFound} structure(s) trouvée(s).`, 'success')
      } else {
        addActionLog('scan', 'Scan groupe échoué', groupScanResult.value.error, 'error')
      }
    } catch (e) {
      groupScanResult.value = {
        success: false, partial: false, regionsScanned: 0, bytesScanned: 0,
        matchesFound: 0, matchesReturned: 0, error: String(e), matches: [],
      }
      addActionLog('scan', 'Scan groupe échoué', String(e), 'error')
    } finally {
      scanBusy.value = false
      groupScanBusy.value = false
      scanStatusText.value = ''
    }
  }

  function addGroupScanEntry() {
    const last = groupScanEntries.value[groupScanEntries.value.length - 1]
    const lastOffset = last ? Number(last.offset || '0') : 0
    groupScanEntries.value.push({ offset: String(lastOffset + 4), type: last?.type ?? 'Int32', value: '' })
  }

  function removeGroupScanEntry(index: number) {
    if (groupScanEntries.value.length <= 2) return
    groupScanEntries.value.splice(index, 1)
  }

  function clearGroupScanEntries() {
    groupScanEntries.value = [{ offset: '0', type: 'Int32', value: '' }, { offset: '4', type: 'Int32', value: '' }]
    groupScanResult.value = null
  }

  async function doEncryptedScan() {
    if (!exactScanValue.value.trim() || scanBusy.value) return

    const controller = backend.getController()
    if (!controller.scanEncryptedValue) {
      encryptedScanResult.value = {
        success: false,
        partial: false,
        regionsScanned: 0,
        bytesScanned: 0,
        matchesFound: 0,
        matchesReturned: 0,
        error: 'Scan chiffré non exposé par ce backend.',
        matches: [],
      }
      addActionLog('scan', 'Scan chiffré indisponible', encryptedScanResult.value.error, 'warning')
      return
    }

    try {
      scanBusy.value = true
      setScanProgress(0)
      scanStatusText.value = 'Scan chiffré en cours...'
      addActionLog('scan', `Scan chiffré ${encryptedScanMode.value.toUpperCase()} ${exactScanValue.value}`, `${exactScanType.value}.`, 'info')
      encryptedScanResult.value = await controller.scanEncryptedValue(
        exactScanValue.value,
        exactScanType.value === 'Auto' ? 'Int32' : exactScanType.value,
        {
          mode: encryptedScanMode.value,
          key: encryptedScanKey.value,
          keySearchBits: encryptedScanKeySearchBits.value,
          startAddress: expertStartAddress.value.trim() || undefined,
          stopAddress: expertStopAddress.value.trim() || undefined,
          alignment: expertAlignment.value > 0 ? expertAlignment.value : undefined,
          writableOnly: true,
          executableOnly: expertExecutableOnly.value,
          copyOnWriteOnly: expertCopyOnWriteOnly.value,
          maxResults: 1000,
        },
      )
      setScanProgress(100)
      scanStatusText.value = encryptedScanResult.value.success ? 'Scan chiffré terminé.' : 'Scan chiffré échoué.'
      addActionLog(
        'scan',
        encryptedScanResult.value.success ? 'Scan chiffré terminé' : 'Scan chiffré échoué',
        `${encryptedScanResult.value.matchesFound} match(es), ${encryptedScanResult.value.regionsScanned} région(s).`,
        encryptedScanResult.value.success ? 'success' : 'warning',
      )
    } catch (e) {
      encryptedScanResult.value = {
        success: false,
        partial: false,
        regionsScanned: 0,
        bytesScanned: 0,
        matchesFound: 0,
        matchesReturned: 0,
        error: String(e),
        matches: [],
      }
      scanStatusText.value = 'Scan chiffré échoué.'
      addActionLog('scan', 'Scan chiffré échoué', String(e), 'error')
    } finally {
      scanBusy.value = false
    }
  }

  async function doNextScan() {
    if (scanBusy.value) return
    if ((candidatePage.value?.totalCount ?? 0) <= 0) {
      scanStatusText.value = 'Aucun candidat à réduire. Lance d’abord un premier scan.'
      nextScanResult.value = {
        success: false,
        checked: 0,
        unreadable: 0,
        remaining: 0,
        error: scanStatusText.value,
      }
      addActionLog('scan', 'Next scan refusé', scanStatusText.value, 'warning')
      return
    }
    try {
      scanBusy.value = true
      setScanProgress(0)
      scanStatusText.value = 'Réduction des candidats...'
      addActionLog('scan', `Next scan ${nextScanMode.value}`, nextScanValue.value ? `Valeur ${nextScanValue.value}.` : 'Sans valeur explicite.', 'info')
      nextScanResult.value = await backend.startNextScanAsync(nextScanMode.value, nextScanValue.value)
      setScanProgress(Math.max(scanProgressPercent.value, 95))
      candidatePageIndex.value = 0
      scanStatusText.value = 'Actualisation des candidats...'
      await refreshCandidates()
      setScanProgress(100)
      scanStatusText.value = nextScanResult.value.cancelled ? 'Scan annulé.' : 'Next scan terminé.'
      addActionLog(
        'scan',
        nextScanResult.value.cancelled ? 'Next scan annulé' : 'Next scan terminé',
        `${nextScanResult.value.remaining} candidat(s) restant(s).`,
        nextScanResult.value.success ? 'success' : 'warning',
      )
    } catch (e) {
      nextScanResult.value = {
        success: false,
        cancelled: false,
        checked: 0,
        unreadable: 0,
        remaining: 0,
        error: String(e),
      }
      scanStatusText.value = 'Next scan échoué.'
      addActionLog('scan', 'Next scan échoué', String(e), 'error')
    } finally {
      scanBusy.value = false
    }
  }

  async function undoCandidateScan() {
    if (scanBusy.value) return null
    try {
      undoCandidateScanResult.value = await backend.getController().undoCandidateScan()
      if (undoCandidateScanResult.value.success) {
        candidatePageIndex.value = 0
        await refreshCandidates()
        scanStatusText.value = `Réduction restaurée : ${undoCandidateScanResult.value.count} candidat(s).`
        addActionLog('rollback', 'Réduction restaurée', `${undoCandidateScanResult.value.count} candidat(s).`, 'success')
      } else {
        scanStatusText.value = undoCandidateScanResult.value.error || 'Aucune réduction à restaurer.'
        addActionLog('rollback', 'Réduction non restaurée', scanStatusText.value, 'warning')
      }
      return undoCandidateScanResult.value
    } catch (e) {
      undoCandidateScanResult.value = {
        success: false,
        restored: false,
        count: candidatePage.value?.totalCount ?? 0,
        error: String(e),
      }
      scanStatusText.value = 'Restauration impossible.'
      addActionLog('rollback', 'Restauration impossible', String(e), 'error')
      return undoCandidateScanResult.value
    }
  }

  async function cancelActiveScan() {
    if (!scanBusy.value) return
    scanStatusText.value = 'Annulation demandée...'
    try {
      const result = await backend.cancelActiveScan()
      if (result.success !== true && result.error) {
        scanStatusText.value = String(result.error)
      }
    } catch (e) {
      scanStatusText.value = 'Annulation impossible : ' + String(e)
    }
  }

  async function captureUnknownSnapshot() {
    if (scanBusy.value) return
    try {
      scanBusy.value = true
      scanProgressPercent.value = 15
      scanStatusText.value = 'Capture unknown en cours...'
      addActionLog('scan', 'Capture unknown', `${unknownScanType.value}.`, 'info')
      unknownSnapshotResult.value = await backend.captureUnknownSnapshotAsync({
        writableOnly: unknownWritableOnly.value,
        copyOnWriteOnly: unknownWritableOnly.value && unknownCopyOnWriteOnly.value,
        unknownSnapshotMaxMb: settingUnknownSnapshotMaxMb.value,
      })
      scanProgressPercent.value = 100
      scanStatusText.value = unknownSnapshotResult.value.cancelled ? 'Scan annulé.' : 'Snapshot capturé.'
      candidatePage.value = null
      candidatePageIndex.value = 0
      nextScanResult.value = null
      unknownNextScanResult.value = null
      unknownScanMode.value = 'changed'
      unknownGuideSteps.value = []
      autoUnknownAwaitingObservation.value = unknownSnapshotResult.value.success === true
      pushUnknownGuideStep({
        mode: 'capture',
        label: 'capture',
        beforeCount: 0,
        afterCount: 0,
        status: unknownSnapshotResult.value.success ? 'capture' : 'error',
        detail: unknownSnapshotResult.value.success
          ? `${unknownSnapshotResult.value.regionsCaptured} région(s), ${unknownSnapshotResult.value.bytesCaptured} octet(s) / limite ${unknownSnapshotResult.value.captureLimitBytes ?? 0}.${unknownSnapshotResult.value.captureLimitReached ? ' Limite atteinte.' : ''}${unknownSnapshotResult.value.writableOnly ? ' Writable only.' : ''}`
          : unknownSnapshotResult.value.error || 'Capture refusée.',
      })
      addActionLog('scan', 'Snapshot unknown capturé', `${unknownSnapshotResult.value.regionsCaptured} région(s).`, 'success')
    } catch (e) {
      unknownSnapshotResult.value = {
        success: false,
        partial: false,
        cancelled: false,
        regionsCaptured: 0,
        regionsSkipped: 0,
        bytesCaptured: 0,
        error: String(e),
      }
      scanStatusText.value = 'Capture unknown échouée.'
      pushUnknownGuideStep({
        mode: 'capture',
        label: 'capture',
        beforeCount: 0,
        afterCount: 0,
        status: 'error',
        detail: String(e),
      })
      addActionLog('scan', 'Capture unknown échouée', String(e), 'error')
    } finally {
      scanBusy.value = false
    }
  }

  async function doUnknownNextScan() {
    if (scanBusy.value) return
    if (!unknownSnapshotResult.value?.success) {
      scanStatusText.value = 'Capture d’abord une image unknown avant de comparer.'
      unknownNextScanResult.value = {
        success: false,
        partial: false,
        cancelled: false,
        checkedBytes: 0,
        matchesFound: 0,
        stored: 0,
        error: scanStatusText.value,
      }
      addActionLog('scan', 'Comparaison unknown refusée', scanStatusText.value, 'warning')
      return
    }
    try {
      scanBusy.value = true
      scanProgressPercent.value = 15
      const candidateCount = candidatePage.value?.totalCount ?? 0
      const isRefine = candidateCount > 0
      scanStatusText.value = isRefine
        ? `Raffinage unknown ${unknownScanMode.value}...`
        : 'Comparaison unknown en cours...'
      addActionLog(
        'scan',
        isRefine ? `Raffinage unknown ${unknownScanMode.value}` : `Comparaison unknown ${unknownScanMode.value}`,
        isRefine ? `${candidateCount} candidat(s).` : unknownScanType.value,
        'info',
      )
      unknownNextScanResult.value = await backend.unknownNextScanAsync(unknownScanMode.value, unknownScanType.value)
      scanProgressPercent.value = 85
      candidatePageIndex.value = 0
      scanStatusText.value = 'Actualisation des candidats...'
      await refreshCandidates()
      scanProgressPercent.value = 100
      scanStatusText.value = unknownNextScanResult.value.cancelled ? 'Scan annulé.' : 'Comparaison unknown terminée.'
      addActionLog('scan', 'Comparaison unknown terminée', `${unknownNextScanResult.value.stored} candidat(s).`, 'success')
    } catch (e) {
      unknownNextScanResult.value = {
        success: false,
        partial: false,
        cancelled: false,
        checkedBytes: 0,
        matchesFound: 0,
        stored: 0,
        error: String(e),
      }
      scanStatusText.value = 'Comparaison unknown échouée.'
      addActionLog('scan', 'Comparaison unknown échouée', String(e), 'error')
    } finally {
      scanBusy.value = false
    }
  }

  async function runUnknownGuideStep(mode: 'increased' | 'decreased' | 'unchanged' | 'changed') {
    if (scanBusy.value) return
    if (!unknownSnapshotResult.value?.success && (candidatePage.value?.totalCount ?? 0) <= 0) {
      scanStatusText.value = 'Capture d’abord une valeur unknown.'
      addActionLog('scan', 'Unknown guidé refusé', scanStatusText.value, 'warning')
      pushUnknownGuideStep({
        mode,
        label: unknownModeLabel(mode),
        beforeCount: 0,
        afterCount: 0,
        status: 'error',
        detail: scanStatusText.value,
      })
      return
    }

    const beforeCount = candidatePage.value?.totalCount ?? 0
    unknownScanMode.value = mode
    await doUnknownNextScan()

    const afterCount = candidatePage.value?.totalCount
      ?? unknownNextScanResult.value?.stored
      ?? nextScanResult.value?.remaining
      ?? 0
    const usedRefine = beforeCount > 0
    const error = unknownNextScanResult.value?.error
    const cancelled = unknownNextScanResult.value?.cancelled
    const status: UnknownGuideStep['status'] = error || cancelled ? 'error' : usedRefine ? 'refine' : 'compare'
    const detail = error
      ? String(error)
      : cancelled
        ? 'Opération annulée.'
        : usedRefine
          ? `${beforeCount} -> ${afterCount} candidat(s).`
          : `${formatCount(unknownNextScanResult.value?.matchesFound)} trouvé(s), ${afterCount} stocké(s).`

    pushUnknownGuideStep({
      mode,
      label: unknownModeLabel(mode),
      beforeCount,
      afterCount,
      status,
      detail,
    })
    autoUnknownAwaitingObservation.value = status !== 'error' && afterCount > 25
  }

  function selectCandidate(address: string, type: string) {
    selectedCandidateAddress.value = address
    exactScanType.value = type
    notifyCandidateForWatch?.(address, type)
    addActionLog('select', `Adresse sélectionnée 0x${address}`, `Type ${type}.`, 'info')
  }

  async function nextCandidatePage() {
    if (!candidatePage.value) return
    const nextStart = (candidatePageIndex.value + 1) * candidatePageSize.value
    if (nextStart >= candidatePage.value.totalCount) return
    candidatePageIndex.value += 1
    await refreshCandidates()
  }

  async function previousCandidatePage() {
    if (candidatePageIndex.value === 0) return
    candidatePageIndex.value -= 1
    await refreshCandidates()
  }

  return {
    configureCandidateWatchNotifier,
    exactScanValue,
    exactScanType,
    exactScanResult,
    encryptedScanResult,
    encryptedScanMode,
    encryptedScanKey,
    encryptedScanKeySearchBits,
    groupScanEntries,
    groupScanResult,
    groupScanBusy,
    groupScanMaxDistance,
    expertModeEnabled,
    expertStartAddress,
    expertStopAddress,
    expertAlignment,
    expertWritableOnly,
    expertExecutableOnly,
    expertCopyOnWriteOnly,
    expertRegionSize,
    expertRegionProtection,
    expertRegionState,
    expertRegionType,
    candidatePage,
    candidatePageIndex,
    candidatePageSize,
    candidateFilter,
    nextScanMode,
    nextScanValue,
    nextScanResult,
    undoCandidateScanResult,
    unknownScanMode,
    unknownScanType,
    unknownWritableOnly,
    unknownCopyOnWriteOnly,
    unknownSnapshotResult,
    unknownNextScanResult,
    unknownGuideSteps,
    autoUnknownAwaitingObservation,
    unknownGuideStepIdCounter,
    selectedCandidateAddress,
    candidateHistory,
    scanBusy,
    scanStatusText,
    scanProgressPercent,
    setScanProgress,
    unknownModeLabel,
    pushUnknownGuideStep,
    extractCandidateCount,
    syncScanDefaultsFromSettings,
    refreshCandidates,
    doExactScan,
    doGroupScan,
    addGroupScanEntry,
    removeGroupScanEntry,
    clearGroupScanEntries,
    doEncryptedScan,
    doNextScan,
    undoCandidateScan,
    cancelActiveScan,
    captureUnknownSnapshot,
    doUnknownNextScan,
    runUnknownGuideStep,
    selectCandidate,
    nextCandidatePage,
    previousCandidatePage,
  }
})
