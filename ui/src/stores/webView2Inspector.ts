/**
 * Store Pinia pour l'inspection WebView2/CDP
 * Calqué sur clrInspector.ts - gestion de l'état de connexion CDP,
 * liste des targets, évaluation JS, et résultats de recherche DOM.
 */
import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import { backend, type WebView2InspectorStatus, type WebView2CdpTarget, type WebView2EvaluateResult, type WebView2FindResult } from '@/services/backend'

export interface WebView2InspectorState {
  isConnecting: boolean
  isConnected: boolean
  error: string | null
  targets: WebView2CdpTarget[]
  selectedTargetId: string | null
  evaluateScript: string
  evaluateResult: WebView2EvaluateResult | null
  findResults: WebView2FindResult[]
  isEvaluating: boolean
  isFinding: boolean
}

export const useWebView2InspectorStore = defineStore('webView2Inspector', () => {
  // State
  const isConnecting = ref(false)
  const isConnected = ref(false)
  const error = ref<string | null>(null)
  const targets = ref<WebView2CdpTarget[]>([])
  const selectedTargetId = ref<string | null>(null)
  const evaluateScript = ref('')
  const evaluateResult = ref<WebView2EvaluateResult | null>(null)
  const findResults = ref<WebView2FindResult[]>([])
  const isEvaluating = ref(false)
  const isFinding = ref(false)

  // Getters
  const selectedTarget = computed(() => {
    return targets.value.find(t => t.targetId === selectedTargetId.value) || null
  })

  const canConnect = computed(() => {
    return selectedTargetId.value !== null && !isConnecting.value && !isConnected.value
  })

  const canEvaluate = computed(() => {
    return isConnected.value && evaluateScript.value.trim().length > 0 && !isEvaluating.value
  })

  const canFind = computed(() => {
    return isConnected.value && !isFinding.value
  })

  // Actions
  async function refreshStatus(): Promise<WebView2InspectorStatus> {
    const controller = backend.getController()
    if (!controller?.getWebView2InspectorStatus) {
      return { success: false, error: 'Méthode non disponible', connected: false }
    }

    try {
      const status = await controller.getWebView2InspectorStatus()
      isConnected.value = status.connected || false
      return status
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e)
      error.value = msg
      return { success: false, error: msg, connected: false }
    }
  }

  async function listTargets(): Promise<WebView2CdpTarget[]> {
    const controller = backend.getController()
    if (!controller?.listWebView2CdpTargets) {
      error.value = 'Méthode listWebView2CdpTargets non disponible'
      return []
    }

    try {
      const result = await controller.listWebView2CdpTargets()
      if (result.success && result.targets) {
        targets.value = result.targets
        return result.targets
      } else {
        error.value = result.error || 'Échec de la liste des targets'
        return []
      }
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e)
      error.value = msg
      return []
    }
  }

  async function connect(targetId?: string): Promise<boolean> {
    const controller = backend.getController()
    if (!controller?.connectWebView2Inspector) {
      error.value = 'Méthode connectWebView2Inspector non disponible'
      return false
    }

    const target = targetId || selectedTargetId.value
    if (!target) {
      error.value = 'Aucun target sélectionné'
      return false
    }

    isConnecting.value = true
    error.value = null

    try {
      const result = await controller.connectWebView2Inspector(target)
      if (result.success) {
        isConnected.value = true
        selectedTargetId.value = target
        return true
      } else {
        error.value = result.error || 'Échec de la connexion'
        return false
      }
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e)
      error.value = msg
      return false
    } finally {
      isConnecting.value = false
    }
  }

  async function disconnect(): Promise<boolean> {
    const controller = backend.getController()
    if (!controller?.disconnectWebView2Inspector) {
      error.value = 'Méthode disconnectWebView2Inspector non disponible'
      return false
    }

    try {
      const result = await controller.disconnectWebView2Inspector()
      if (result.success) {
        isConnected.value = false
        selectedTargetId.value = null
        evaluateResult.value = null
        findResults.value = []
        return true
      } else {
        error.value = result.error || 'Échec de la déconnexion'
        return false
      }
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e)
      error.value = msg
      return false
    }
  }

  async function evaluateJavaScript(script?: string): Promise<WebView2EvaluateResult | null> {
    const controller = backend.getController()
    if (!controller?.evaluateWebView2JavaScript) {
      error.value = 'Méthode evaluateWebView2JavaScript non disponible'
      return null
    }

    const code = script || evaluateScript.value
    if (!code.trim()) {
      error.value = 'Script vide'
      return null
    }

    isEvaluating.value = true
    error.value = null

    try {
      const result = await controller.evaluateWebView2JavaScript(code)
      evaluateResult.value = result
      if (!result.success) {
        error.value = result.error || 'Échec de l\'évaluation'
      }
      return result
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e)
      error.value = msg
      return null
    } finally {
      isEvaluating.value = false
    }
  }

  async function findDisplayedValues(value: string, options?: Record<string, unknown>): Promise<WebView2FindResult[]> {
    const controller = backend.getController()
    if (!controller?.findWebView2DisplayedValues) {
      error.value = 'Méthode findWebView2DisplayedValues non disponible'
      return []
    }

    isFinding.value = true
    error.value = null

    try {
      const result = await controller.findWebView2DisplayedValues(value, options || {})
      if (result.success && result.matches) {
        findResults.value = result.matches
        return result.matches
      } else {
        error.value = result.error || 'Échec de la recherche'
        return []
      }
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e)
      error.value = msg
      return []
    } finally {
      isFinding.value = false
    }
  }

  async function findDisplayedText(text: string, options?: Record<string, unknown>): Promise<WebView2FindResult[]> {
    const controller = backend.getController()
    if (!controller?.findWebView2DisplayedText) {
      error.value = 'Méthode findWebView2DisplayedText non disponible'
      return []
    }

    isFinding.value = true
    error.value = null

    try {
      const result = await controller.findWebView2DisplayedText(text, options || {})
      if (result.success && result.matches) {
        findResults.value = result.matches
        return result.matches
      } else {
        error.value = result.error || 'Échec de la recherche'
        return []
      }
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e)
      error.value = msg
      return []
    } finally {
      isFinding.value = false
    }
  }

  function selectTarget(targetId: string) {
    selectedTargetId.value = targetId
  }

  function clearError() {
    error.value = null
  }

  function clearResults() {
    evaluateResult.value = null
    findResults.value = []
  }

  function reset() {
    isConnecting.value = false
    isConnected.value = false
    error.value = null
    targets.value = []
    selectedTargetId.value = null
    evaluateScript.value = ''
    evaluateResult.value = null
    findResults.value = []
    isEvaluating.value = false
    isFinding.value = false
  }

  return {
    // State
    isConnecting,
    isConnected,
    error,
    targets,
    selectedTargetId,
    evaluateScript,
    evaluateResult,
    findResults,
    isEvaluating,
    isFinding,
    // Getters
    selectedTarget,
    canConnect,
    canEvaluate,
    canFind,
    // Actions
    refreshStatus,
    listTargets,
    connect,
    disconnect,
    evaluateJavaScript,
    findDisplayedValues,
    findDisplayedText,
    selectTarget,
    clearError,
    clearResults,
    reset,
  }
})
