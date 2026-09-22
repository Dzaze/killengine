/**
 * KillEngine — store Settings (extrait de app.ts, candidat S12,
 * docs/REFACTOR_ROADMAP.md, 29/08/2026).
 *
 * Feuille sans dépendance vers `./app`. Deux effets de bord existaient dans
 * l'ancien `applySettings`/`saveSettings` vers d'autres domaines encore
 * possédés par app.ts (sync `exactScanType`/`unknownScanType` du scan, appel
 * à `refreshDiagnostics`) : conservés via callbacks optionnels injectés par
 * l'appelant, même patron que `riskGate.ts` pour `logAiAudit`.
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'
import { i18n } from '@/i18n'
import { backend, type AppSettings, type AiModelStatus } from '@/services/backend'

const { t } = i18n.global

export const useSettingsStore = defineStore('settings', () => {
  const settingsLoaded = ref(false)
  const settingsSaving = ref(false)
  const settingsStatus = ref('')
  // UX-PIPE-5 (docs/PHASE_TRACKER.md, 18/09/2026) : true seulement quand
  // settingsStatus décrit un échec (persistance disque ou exception de
  // transport), jamais quand elle décrit un succès -- permet à l'UI de
  // distinguer visuellement les deux sans avoir à reparser le texte.
  const settingsSaveError = ref(false)
  // UX-PIPE-5 : échec de setUiLanguage (sélecteur rapide sidebar) -- distinct
  // de settingsStatus/settingsSaveError qui ne sont affichés que dans
  // Paramètres, alors que ce sélecteur est visible sur toutes les vues.
  const languageSwitchError = ref('')
  const appLanguage = ref<'fr' | 'en'>('fr')
  const settingDefaultValueType = ref('Int32')
  const settingScanMaxResults = ref(1000000)
  const settingScanChunkSizeMb = ref(0)
  const settingPerformanceMode = ref<AppSettings['performanceMode']>('Auto')
  const settingScanMaxWorkerThreads = ref(0)
  const settingScanMaxInFlightMb = ref(0)
  const settingCandidateFileBackedThreshold = ref(250000)
  const settingUnknownSnapshotMaxMb = ref(128)
  const settingFastScan = ref(true)
  const settingSmartSearchDebugEnabled = ref(true)
  const settingSmartSearchDebugMaxEvents = ref(30)
  const settingModelPath = ref('')
  const settingModelEnabled = ref(true)
  const settingModelThreads = ref(4)
  const aiModelStatus = ref<AiModelStatus | null>(null)
  const aiModelStatusLoading = ref(false)
  const aiModelStatusError = ref('')

  function applySettings(settings: AppSettings, onApplied?: (settings: AppSettings) => void) {
    appLanguage.value = settings.language === 'en' ? 'en' : 'fr'
    settingDefaultValueType.value = settings.defaultValueType || 'Int32'
    settingScanMaxResults.value = Number(settings.scanMaxResults || 1000000)
    settingScanChunkSizeMb.value = Number(settings.scanChunkSizeMb ?? 0)
    settingPerformanceMode.value = settings.performanceMode || 'Auto'
    settingScanMaxWorkerThreads.value = Number(settings.scanMaxWorkerThreads ?? 0)
    settingScanMaxInFlightMb.value = Number(settings.scanMaxInFlightMb ?? 0)
    settingCandidateFileBackedThreshold.value = Number(settings.candidateFileBackedThreshold || 250000)
    settingUnknownSnapshotMaxMb.value = Number(settings.unknownSnapshotMaxMb || 128)
    settingFastScan.value = settings.fastScan !== false
    settingSmartSearchDebugEnabled.value = settings.smartSearchDebugEnabled !== false
    settingSmartSearchDebugMaxEvents.value = Number(settings.smartSearchDebugMaxEvents || 30)
    settingModelPath.value = settings.modelPath || ''
    settingModelEnabled.value = settings.modelEnabled !== false
    settingModelThreads.value = Number(settings.modelThreads || 4)
    onApplied?.(settings)
  }

  function currentSettings(): AppSettings {
    return {
      language: appLanguage.value,
      defaultValueType: settingDefaultValueType.value,
      scanMaxResults: settingScanMaxResults.value,
      scanChunkSizeMb: settingScanChunkSizeMb.value,
      performanceMode: settingPerformanceMode.value,
      scanMaxWorkerThreads: settingScanMaxWorkerThreads.value,
      scanMaxInFlightMb: settingScanMaxInFlightMb.value,
      candidateFileBackedThreshold: settingCandidateFileBackedThreshold.value,
      unknownSnapshotMaxMb: settingUnknownSnapshotMaxMb.value,
      fastScan: settingFastScan.value,
      smartSearchDebugEnabled: settingSmartSearchDebugEnabled.value,
      smartSearchDebugMaxEvents: settingSmartSearchDebugMaxEvents.value,
      modelPath: settingModelPath.value,
      modelEnabled: settingModelEnabled.value,
      modelThreads: settingModelThreads.value,
    }
  }

  async function refreshAiModelStatus() {
    aiModelStatusLoading.value = true
    aiModelStatusError.value = ''
    try {
      const controller = backend.getController()
      if (!controller.getAiModelStatus) {
        aiModelStatus.value = null
        aiModelStatusError.value = t('settingsStore.aiStatusNotExposed')
        return null
      }
      const status = await controller.getAiModelStatus()
      aiModelStatus.value = status
      aiModelStatusError.value = status.success === false ? String(status.error ?? status.message ?? t('settingsStore.aiStatusUnavailable')) : ''
      return status
    } catch (e) {
      aiModelStatus.value = null
      aiModelStatusError.value = String(e)
      return null
    } finally {
      aiModelStatusLoading.value = false
    }
  }

  async function loadSettings(onApplied?: (settings: AppSettings) => void) {
    try {
      const settings = await backend.getController().getSettings()
      applySettings(settings, onApplied)
      settingsLoaded.value = true
      settingsStatus.value = ''
      await refreshAiModelStatus()
      return settings
    } catch (e) {
      settingsStatus.value = t('settingsStore.loadFailed', { error: String(e) })
      return null
    }
  }

  async function browseForModel() {
    const controller = backend.getController()
    if (!controller.browseForModelFile) {
      aiModelStatusError.value = t('settingsStore.filePickerNotExposed')
      return
    }
    const result = await controller.browseForModelFile()
    if (result.success === true && typeof result.path === 'string') {
      settingModelPath.value = result.path
    }
  }

  async function saveSettings(onApplied?: (settings: AppSettings) => void, onSaved?: () => void) {
    settingsSaving.value = true
    settingsSaveError.value = false
    try {
      const saved = await backend.getController().saveSettings(currentSettings())
      // PORT-3 : le backend applique déjà les valeurs en mémoire (et à l'état
      // moteur effectif) même si la synchronisation disque échoue -- refléter
      // ça ici aussi (garde la saisie de l'utilisateur, pas de perte), mais ne
      // JAMAIS annoncer "sauvegardé" quand saved.success est false.
      applySettings(saved, onApplied)
      settingsLoaded.value = true
      if (saved.success === false) {
        settingsSaveError.value = true
        settingsStatus.value = t('settingsStore.saveFailed', { error: saved.error || t('settingsStore.saveFailedUnknown') })
      } else {
        settingsStatus.value = t('settingsStore.saved')
      }
      await onSaved?.()
      await refreshAiModelStatus()
      return saved
    } catch (e) {
      settingsSaveError.value = true
      settingsStatus.value = t('settingsStore.saveFailed', { error: String(e) })
      return null
    } finally {
      settingsSaving.value = false
    }
  }

  // Persiste la langue immédiatement (QSettings ui/language), indépendamment
  // du "Sauvegarder" complet des Réglages — sans ça, le texte backend (KE_TXT,
  // ex. catalogue Modules) reste bloqué sur la dernière langue sauvegardée
  // et le switch rapide FR/EN de la sidebar (App.vue) n'est qu'à moitié réel.
  async function switchLanguage(language: 'fr' | 'en') {
    // Appliqué côté Vue immédiatement (effectif pour cette session même si la
    // persistance échoue plus bas) -- voir UX-PIPE-5 : mémoire et disque sont
    // deux états différents, ne jamais les confondre dans un seul message.
    appLanguage.value = language
    try {
      const result = await backend.getController().setUiLanguage?.(language)
      languageSwitchError.value = result && result.success === false
        ? (result.error || t('settingsStore.saveFailedUnknown'))
        : ''
    } catch (e) {
      languageSwitchError.value = String(e)
    }
  }

  return {
    settingsLoaded,
    settingsSaving,
    settingsStatus,
    settingsSaveError,
    languageSwitchError,
    appLanguage,
    switchLanguage,
    settingDefaultValueType,
    settingScanMaxResults,
    settingScanChunkSizeMb,
    settingPerformanceMode,
    settingScanMaxWorkerThreads,
    settingScanMaxInFlightMb,
    settingCandidateFileBackedThreshold,
    settingUnknownSnapshotMaxMb,
    settingFastScan,
    settingSmartSearchDebugEnabled,
    settingSmartSearchDebugMaxEvents,
    settingModelPath,
    settingModelEnabled,
    settingModelThreads,
    aiModelStatus,
    aiModelStatusLoading,
    aiModelStatusError,
    applySettings,
    currentSettings,
    loadSettings,
    saveSettings,
    refreshAiModelStatus,
    browseForModel,
  }
})
