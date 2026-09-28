/**
 * KillEngine — store du comparateur visuel de 2 à 6 candidats (UX-PRODUIT-16).
 *
 * Fondation sans dépendance vers `./app` (même principe que `activity.ts`) :
 * connecté depuis `app.ts::init()` (signal comparisonFinished), consommé par
 * CandidateComparisonPanel.vue et les 3 sources de sélection (CandidatePanel,
 * ExpertView Trace UI string, WatchLivePanel).
 *
 * Une seule comparaison active à la fois côté backend (même contrat que
 * Memory Timeline) : stageSeriesFromSelection() remplace toujours la liste en
 * attente, jamais un ajout incrémental discret qui masquerait à l'utilisateur
 * qu'un ancien brouillon vient d'être écrasé.
 */
import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import { backend } from '@/services/backend'
import { useActionLogStore } from './actionLog'

export interface ComparisonSeriesDraft {
  id: string
  address: string
  type: string
  factor: number
  label: string
}

export interface ComparisonSeriesInfo extends ComparisonSeriesDraft {
  pointCount: number
}

export interface ComparisonPoint {
  batchId: number
  timestampMs: number
  isValid: boolean
  ok: boolean
  exactValueText: string
  numericValue: number
  isNaN: boolean
  isInfinite: boolean
  scaledValueText: string
  scaledValueExact: boolean
}

export interface ComparisonCorrelation {
  seriesIdA: string
  seriesIdB: string
  computable: boolean
  coefficient: number
  pairCount: number
  reason: string
}

export interface ComparisonMarker {
  id: number
  timestampMs: number
  text: string
}

const kMinSeries = 2
const kMaxSeries = 6
const kSamplesFetchLimit = 3000 // > la borne backend de 2401 points/série -- une seule page couvre toujours tout.

function asSeriesInfo(value: unknown): ComparisonSeriesInfo | null {
  if (!value || typeof value !== 'object') return null
  const item = value as Record<string, unknown>
  const id = String(item.id ?? '').trim()
  if (!id) return null
  return {
    id,
    address: String(item.address ?? ''),
    type: String(item.type ?? 'Int32'),
    factor: Number(item.factor ?? 1),
    label: String(item.label ?? id),
    pointCount: Number(item.pointCount ?? 0),
  }
}

function asPoint(value: unknown): ComparisonPoint | null {
  if (!value || typeof value !== 'object') return null
  const item = value as Record<string, unknown>
  return {
    batchId: Number(item.batchId ?? 0),
    timestampMs: Number(item.timestampMs ?? 0),
    isValid: item.isValid === true,
    ok: item.ok === true,
    exactValueText: String(item.exactValueText ?? ''),
    numericValue: Number(item.numericValue ?? 0),
    isNaN: item.isNaN === true,
    isInfinite: item.isInfinite === true,
    scaledValueText: String(item.scaledValueText ?? ''),
    scaledValueExact: item.scaledValueExact === true,
  }
}

function asCorrelation(value: unknown): ComparisonCorrelation | null {
  if (!value || typeof value !== 'object') return null
  const item = value as Record<string, unknown>
  return {
    seriesIdA: String(item.seriesIdA ?? ''),
    seriesIdB: String(item.seriesIdB ?? ''),
    computable: item.computable === true,
    coefficient: Number(item.coefficient ?? 0),
    pairCount: Number(item.pairCount ?? 0),
    reason: String(item.reason ?? ''),
  }
}

function asMarker(value: unknown): ComparisonMarker | null {
  if (!value || typeof value !== 'object') return null
  const item = value as Record<string, unknown>
  return {
    id: Number(item.id ?? 0),
    timestampMs: Number(item.timestampMs ?? 0),
    text: String(item.text ?? ''),
  }
}

let nextDraftIdCounter = 0

export const useCandidateComparisonStore = defineStore('candidateComparison', () => {
  const actionLogStore = useActionLogStore()

  const pendingSeries = ref<ComparisonSeriesDraft[]>([])
  const activeSeries = ref<ComparisonSeriesInfo[]>([])
  const collecting = ref(false)
  const stopReason = ref('')
  const tourCount = ref(0)
  const skippedTicks = ref(0)
  const samplesBySeries = ref<Map<string, ComparisonPoint[]>>(new Map())
  const correlations = ref<ComparisonCorrelation[]>([])
  const markers = ref<ComparisonMarker[]>([])
  const intervalMs = ref(100)
  const maxDurationMs = ref(30000)
  const busy = ref(false)
  const error = ref('')
  // AUDIT-PIPE-A8 : identité serveur de la capture EN COURS (ou de la
  // dernière capture démarrée), retournée par startCandidateComparison.
  // Repassée à recordObservation() ci-dessous -- jamais la session/le
  // process actuellement attaché, qui peut avoir changé depuis. Reste
  // renseignée après l'arrêt/la fin d'une capture (utile pour consigner une
  // preuve juste après la fin), et n'est écrasée que par un nouveau
  // startComparison() réussi.
  const currentCaptureId = ref('')

  const canStart = computed(() => pendingSeries.value.length >= kMinSeries && pendingSeries.value.length <= kMaxSeries && !collecting.value)
  const hasResults = computed(() => activeSeries.value.length > 0)

  /// Remplace toujours la sélection en attente -- pas d'ajout incrémental
  /// discret (voir commentaire de tête). `factor` par défaut 1.
  function stageSeriesFromSelection(items: Array<{ address: string, type: string, factor?: number, label: string }>) {
    pendingSeries.value = items.slice(0, kMaxSeries).map((item) => ({
      id: `s${nextDraftIdCounter++}_${item.address.replace(/^0x/i, '')}`,
      address: item.address,
      type: item.type,
      factor: item.factor && item.factor > 0 ? item.factor : 1,
      label: item.label || item.address,
    }))
  }

  function clearStaged() {
    pendingSeries.value = []
  }

  function removeStagedById(id: string) {
    pendingSeries.value = pendingSeries.value.filter((s) => s.id !== id)
  }

  async function startComparison(): Promise<{ success: boolean, error?: string }> {
    if (!canStart.value) {
      return { success: false, error: 'Sélection invalide (2 à 6 candidats requis).' }
    }
    const controller = backend.getController()
    if (!controller.startCandidateComparison) {
      error.value = 'Comparateur indisponible dans ce backend.'
      return { success: false, error: error.value }
    }

    busy.value = true
    error.value = ''
    try {
      const result = await controller.startCandidateComparison(pendingSeries.value, {
        intervalMs: intervalMs.value,
        maxDurationMs: maxDurationMs.value,
      })
      if (result.success === true) {
        collecting.value = true
        stopReason.value = ''
        samplesBySeries.value = new Map()
        correlations.value = []
        markers.value = []
        currentCaptureId.value = String(result.captureId ?? '')
        actionLogStore.addActionLog('investigation', 'Comparaison de candidats démarrée',
          `${pendingSeries.value.length} série(s)`, 'success')
        await refreshStatus()
      } else {
        error.value = String(result.error ?? 'Démarrage refusé.')
      }
      return { success: result.success === true, error: error.value || undefined }
    } catch (e) {
      error.value = String(e)
      return { success: false, error: error.value }
    } finally {
      busy.value = false
    }
  }

  async function stopComparison() {
    const controller = backend.getController()
    if (!controller.stopCandidateComparison) return
    try {
      await controller.stopCandidateComparison()
    } catch (e) {
      actionLogStore.addActionLog('investigation', 'Arrêt de la comparaison en échec', String(e), 'error')
    }
  }

  async function refreshStatus() {
    const controller = backend.getController()
    if (!controller.getCandidateComparisonStatus) return
    try {
      const result = await controller.getCandidateComparisonStatus()
      collecting.value = result.collecting === true
      stopReason.value = String(result.stopReason ?? '')
      tourCount.value = Number(result.tourCount ?? 0)
      skippedTicks.value = Number(result.skippedTicks ?? 0)
      activeSeries.value = Array.isArray(result.series)
        ? result.series.map(asSeriesInfo).filter((s): s is ComparisonSeriesInfo => s !== null)
        : []
      markers.value = Array.isArray(result.markers)
        ? result.markers.map(asMarker).filter((m): m is ComparisonMarker => m !== null)
        : []
    } catch {
      // Silencieux -- poll de statut, le prochain appel réessaiera.
    }
  }

  async function fetchSamples(seriesId: string) {
    const controller = backend.getController()
    if (!controller.getCandidateComparisonSamples) return
    try {
      const result = await controller.getCandidateComparisonSamples(seriesId, 0, kSamplesFetchLimit)
      if (result.success === true && Array.isArray(result.points)) {
        const next = new Map(samplesBySeries.value)
        next.set(seriesId, result.points.map(asPoint).filter((p): p is ComparisonPoint => p !== null))
        samplesBySeries.value = next
      }
    } catch {
      // Silencieux -- l'appelant peut réessayer via un nouveau refresh.
    }
  }

  async function refreshAllSamples() {
    await Promise.all(activeSeries.value.map((s) => fetchSamples(s.id)))
  }

  async function refreshCorrelations() {
    const controller = backend.getController()
    if (!controller.getCandidateComparisonCorrelations) return
    try {
      const result = await controller.getCandidateComparisonCorrelations()
      correlations.value = Array.isArray(result.correlations)
        ? result.correlations.map(asCorrelation).filter((c): c is ComparisonCorrelation => c !== null)
        : []
    } catch {
      // Silencieux.
    }
  }

  /// Repère horodaté (16C) -- refusé côté backend hors capture active, texte
  /// vide/>500 caractères, ou 100 repères déjà atteints. Rafraîchit
  /// `markers` depuis le résultat (id/timestamp attribués par le backend,
  /// même horloge que les points), pas d'ajout optimiste local.
  async function addMarker(text: string): Promise<{ success: boolean, error?: string }> {
    const trimmed = text.trim()
    if (!trimmed) {
      return { success: false, error: 'Texte du repère vide.' }
    }
    const controller = backend.getController()
    if (!controller.addCandidateComparisonMarker) {
      return { success: false, error: 'Repères indisponibles dans ce backend.' }
    }
    try {
      const result = await controller.addCandidateComparisonMarker(trimmed)
      if (result.success === true) {
        markers.value = [...markers.value, {
          id: Number(result.id ?? 0),
          timestampMs: Number(result.timestampMs ?? 0),
          text: trimmed,
        }]
        return { success: true }
      }
      return { success: false, error: String(result.error ?? 'Repère refusé.') }
    } catch (e) {
      return { success: false, error: String(e) }
    }
  }

  /// Export JSON versionné et borné de la capture courante (16C) -- écrit
  /// directement sur disque côté backend, retourne le chemin. N'affecte
  /// jamais les exports Memory Timeline existants (format distinct).
  async function exportComparison(): Promise<{ success: boolean, filepath?: string, error?: string }> {
    const controller = backend.getController()
    if (!controller.exportCandidateComparisonToJson) {
      return { success: false, error: 'Export indisponible dans ce backend.' }
    }
    try {
      const result = await controller.exportCandidateComparisonToJson()
      if (result.success === true) {
        const filepath = String(result.filepath ?? '')
        actionLogStore.addActionLog('investigation', 'Comparaison exportée', filepath, 'success')
        return { success: true, filepath }
      }
      const errorMessage = String(result.error ?? 'Export échoué.')
      actionLogStore.addActionLog('investigation', 'Export de la comparaison en échec', errorMessage, 'error')
      return { success: false, error: errorMessage }
    } catch (e) {
      actionLogStore.addActionLog('investigation', 'Export de la comparaison en échec', String(e), 'error')
      return { success: false, error: String(e) }
    }
  }

  /// AUDIT-PIPE-A8 : consigne une preuve d'effet liée à `currentCaptureId`
  /// (la capture identifiée serveur, pas la session actuellement attachée)
  /// -- toujours niveau "unverified", jamais de promotion automatique. Le
  /// label/source/conditions de la preuve sont construits côté backend à
  /// partir de la provenance figée de cette capture, pas de l'état live du
  /// store (qui peut avoir changé si une autre capture a démarré depuis).
  async function recordObservation(note: string): Promise<{ success: boolean, error?: string }> {
    if (!currentCaptureId.value) {
      return { success: false, error: 'Aucune capture identifiée -- démarre une comparaison avant de consigner.' }
    }
    const controller = backend.getController()
    if (!controller.recordCandidateComparisonObservation) {
      return { success: false, error: 'Consignation indisponible dans ce backend.' }
    }
    busy.value = true
    error.value = ''
    try {
      const result = await controller.recordCandidateComparisonObservation(currentCaptureId.value, note.trim())
      if (result.success === true) {
        actionLogStore.addActionLog('investigation', 'Preuve consignée (comparaison de candidats)', note.trim(), 'success')
        return { success: true }
      }
      error.value = String(result.error ?? 'Consignation refusée.')
      actionLogStore.addActionLog('investigation', 'Consignation de preuve en échec', error.value, 'error')
      return { success: false, error: error.value }
    } catch (e) {
      error.value = String(e)
      return { success: false, error: error.value }
    } finally {
      busy.value = false
    }
  }

  /// Appelé depuis app.ts::init() à la réception du signal comparisonFinished.
  async function handleComparisonFinished(reason: string) {
    collecting.value = false
    stopReason.value = reason
    await refreshStatus()
    await refreshAllSamples()
    await refreshCorrelations()
    actionLogStore.addActionLog('investigation', 'Comparaison de candidats terminée', reason, 'info')
  }

  return {
    pendingSeries,
    activeSeries,
    collecting,
    stopReason,
    tourCount,
    skippedTicks,
    samplesBySeries,
    correlations,
    markers,
    intervalMs,
    maxDurationMs,
    busy,
    error,
    currentCaptureId,
    canStart,
    hasResults,
    stageSeriesFromSelection,
    clearStaged,
    removeStagedById,
    startComparison,
    stopComparison,
    refreshStatus,
    fetchSamples,
    refreshAllSamples,
    refreshCorrelations,
    addMarker,
    exportComparison,
    recordObservation,
    handleComparisonFinished,
  }
})
