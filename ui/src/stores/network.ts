/**
 * KillEngine — store Réseau (connexions, DLL, proxy HTTP, spoof DNS, lag switch)
 * Extrait de app.ts, PHASE réseau unifiée, 05/09/2026.
 */
import { ref } from 'vue'
import { backend, type NetworkConnection, type NetworkModule, type HttpProxyRequest } from '@/services/backend'
import { useActionLogStore } from '@/stores/actionLog'

export interface DnsSpoofEntry {
  domain: string
  targetIp: string
}

export function useNetworkStore() {
  const actionLogStore = useActionLogStore()

  // ── Lecture seule ──────────────────────────────────────────────
  const networkConnections = ref<NetworkConnection[]>([])
  const networkModules = ref<NetworkModule[]>([])
  const networkConnectionsBusy = ref(false)
  const networkModulesBusy = ref(false)
  const networkLastRefresh = ref<string | null>(null)
  const liveRefreshEnabled = ref(false)

  // Filtres UI (stockés dans le store pour persister entre les refreshs)
  const _networkFilterProtocol = ref('all')
  const _networkFilterState = ref('all')
  const _networkFilterIp = ref('')

  // ── Proxy HTTP ─────────────────────────────────────────────────
  const httpProxyActive = ref(false)
  const httpProxyBusy = ref(false)
  const httpProxyPort = ref(8080)
  const httpProxyInterceptHttps = ref(true)
  const httpProxyRequests = ref<HttpProxyRequest[]>([])
  const selectedHttpRequest = ref<string | null>(null)
  const httpRequestBodyEditor = ref('')

  // ── Spoof DNS ──────────────────────────────────────────────────
  const dnsSpoofEntries = ref<DnsSpoofEntry[]>([])
  const dnsSpoofBusy = ref(false)
  const dnsSpoofDomain = ref('')
  const dnsSpoofTargetIp = ref('127.0.0.1')

  // ── Lag switch ─────────────────────────────────────────────────
  const lagSwitchActive = ref(false)
  const lagSwitchBusy = ref(false)
  const lagSwitchDelayMs = ref(1000)

  // ── Actions : Lecture seule ────────────────────────────────────
  async function refreshNetworkConnections() {
    const controller = backend.getController()
    networkConnectionsBusy.value = true
    try {
      const result = await controller.getProcessNetworkConnections?.()
      if (result?.success) {
        networkConnections.value = result.connections ?? []
      }
    } finally {
      networkConnectionsBusy.value = false
      networkLastRefresh.value = new Date().toLocaleTimeString()
    }
  }

  async function refreshNetworkModules() {
    const controller = backend.getController()
    networkModulesBusy.value = true
    try {
      const result = await controller.getProcessNetworkModules?.()
      if (result?.success) {
        networkModules.value = result.modules ?? []
      }
    } finally {
      networkModulesBusy.value = false
    }
  }

  async function refreshAllNetwork() {
    await Promise.all([refreshNetworkConnections(), refreshNetworkModules()])
  }

  let _liveRefreshInterval: ReturnType<typeof setInterval> | null = null
  let _httpProxyRefreshInterval: ReturnType<typeof setInterval> | null = null

  function startLiveRefresh() {
    if (_liveRefreshInterval) return
    liveRefreshEnabled.value = true
    _liveRefreshInterval = setInterval(async () => {
      if (!liveRefreshEnabled.value) { stopLiveRefresh(); return }
      await refreshNetworkConnections()
    }, 2000)
  }

  function stopLiveRefresh() {
    liveRefreshEnabled.value = false
    if (_liveRefreshInterval) {
      clearInterval(_liveRefreshInterval)
      _liveRefreshInterval = null
    }
  }

  function startHttpProxyPolling() {
    if (_httpProxyRefreshInterval) return
    _httpProxyRefreshInterval = setInterval(async () => {
      if (!httpProxyActive.value) { stopHttpProxyPolling(); return }
      await refreshHttpProxyRequests()
    }, 1500)
  }

  function stopHttpProxyPolling() {
    if (_httpProxyRefreshInterval) {
      clearInterval(_httpProxyRefreshInterval)
      _httpProxyRefreshInterval = null
    }
  }

  // ── Actions : Proxy HTTP ───────────────────────────────────────
  // startHttpProxyAsync/stopHttpProxyAsync ne bloquent plus le thread GUI
  // (injection DLL + attente du handler jusqu'à 6s/3s sur thread séparé côté
  // backend) : elles renvoient juste {started:true} immédiatement, le vrai
  // résultat arrive via onHttpProxyStartFinished/onHttpProxyStopFinished
  // (branchés dans app.ts) — busy reste true jusque-là.
  async function startHttpProxy() {
    const controller = backend.getController()
    httpProxyBusy.value = true
    try {
      const result = await controller.startHttpProxyAsync?.(httpProxyPort.value, httpProxyInterceptHttps.value)
      if (!result?.started) {
        httpProxyBusy.value = false
        actionLogStore.addActionLog('http_proxy', 'Proxy HTTP échoué', result?.error ?? 'raison inconnue', 'error')
      }
    } catch (e) {
      httpProxyBusy.value = false
      actionLogStore.addActionLog('http_proxy', 'Proxy HTTP échoué', String(e), 'error')
    }
  }

  function onHttpProxyStartFinished(result: Record<string, unknown>) {
    httpProxyBusy.value = false
    if (result.success) {
      httpProxyActive.value = true
      startHttpProxyPolling()
      actionLogStore.addActionLog('http_proxy', 'Proxy HTTP démarré', `Port ${result.port ?? httpProxyPort.value}`, 'success')
    } else {
      actionLogStore.addActionLog('http_proxy', 'Proxy HTTP échoué', String(result.error ?? 'raison inconnue'), 'error')
    }
  }

  async function stopHttpProxy() {
    const controller = backend.getController()
    httpProxyBusy.value = true
    try {
      const result = await controller.stopHttpProxyAsync?.()
      if (!result?.started) {
        httpProxyBusy.value = false
      }
    } catch (e) {
      httpProxyBusy.value = false
    }
  }

  function onHttpProxyStopFinished(result: Record<string, unknown>) {
    httpProxyBusy.value = false
    if (result.success) {
      httpProxyActive.value = false
      httpProxyRequests.value = []
      stopHttpProxyPolling()
      actionLogStore.addActionLog('http_proxy', 'Proxy HTTP arrêté', '', 'success')
    }
  }

  async function refreshHttpProxyRequests() {
    const controller = backend.getController()
    const result = await controller.getHttpProxyRequests?.()
    if (result?.success) {
      httpProxyRequests.value = result.requests ?? []
    }
  }

  async function modifySelectedHttpRequest(newBody: string) {
    if (!selectedHttpRequest.value) return
    const controller = backend.getController()
    httpProxyBusy.value = true
    try {
      const result = await controller.modifyHttpRequest?.(selectedHttpRequest.value, newBody)
      if (result?.success) {
        httpRequestBodyEditor.value = ''
        selectedHttpRequest.value = null
        await refreshHttpProxyRequests()
        actionLogStore.addActionLog('http_proxy', 'Requête HTTP modifiée', '', 'success')
      }
    } finally {
      httpProxyBusy.value = false
    }
  }

  // ── Actions : Spoof DNS ────────────────────────────────────────
  // spoofDnsAsync/restoreDnsAsync ne bloquent plus le thread GUI (élévation
  // UAC + PowerShell sur thread séparé côté backend) : elles renvoient juste
  // {started:true} immédiatement, le vrai résultat arrive via un signal
  // (onDnsSpoofFinished/onDnsRestoreFinished, branchés dans app.ts) — busy
  // reste true jusque-là.
  async function addDnsSpoofEntry() {
    if (!dnsSpoofDomain.value.trim() || !dnsSpoofTargetIp.value.trim()) return
    const controller = backend.getController()
    dnsSpoofBusy.value = true
    try {
      const result = await controller.spoofDnsAsync?.(dnsSpoofDomain.value, dnsSpoofTargetIp.value)
      if (!result?.started) {
        dnsSpoofBusy.value = false
        actionLogStore.addActionLog('dns_spoof', 'Spoof DNS échoué', result?.error ?? 'raison inconnue', 'error')
      }
    } catch (e) {
      dnsSpoofBusy.value = false
      actionLogStore.addActionLog('dns_spoof', 'Spoof DNS échoué', String(e), 'error')
    }
  }

  function onDnsSpoofFinished(result: Record<string, unknown>) {
    dnsSpoofBusy.value = false
    const domain = String(result.domain ?? '')
    const targetIp = String(result.targetIp ?? '')
    if (result.success) {
      dnsSpoofEntries.value.push({ domain, targetIp })
      actionLogStore.addActionLog('dns_spoof', `DNS spoofé: ${domain} → ${targetIp}`, '', 'success')
      if (dnsSpoofDomain.value === domain) dnsSpoofDomain.value = ''
    } else {
      actionLogStore.addActionLog('dns_spoof', 'Spoof DNS échoué', String(result.error ?? 'raison inconnue'), 'error')
    }
  }

  async function removeDnsSpoofEntry(domain: string) {
    const controller = backend.getController()
    dnsSpoofBusy.value = true
    try {
      const result = await controller.restoreDnsAsync?.(domain)
      if (!result?.started) {
        dnsSpoofBusy.value = false
      }
    } catch (e) {
      dnsSpoofBusy.value = false
    }
  }

  function onDnsRestoreFinished(result: Record<string, unknown>) {
    dnsSpoofBusy.value = false
    const domain = String(result.domain ?? '')
    if (result.success) {
      dnsSpoofEntries.value = dnsSpoofEntries.value.filter(e => e.domain !== domain)
      actionLogStore.addActionLog('dns_spoof', `DNS restauré: ${domain}`, '', 'success')
    }
  }

  // ── Actions : Lag switch ───────────────────────────────────────
  // setLagSwitchAsync ne bloque plus le thread GUI (injection/désinstallation
  // sur thread séparé côté backend, jusqu'à 6s/3s) : elle renvoie juste
  // {started:true} immédiatement, le vrai résultat arrive via
  // onLagSwitchFinished (branché dans app.ts) — busy reste true jusque-là.
  async function toggleLagSwitch() {
    const controller = backend.getController()
    lagSwitchBusy.value = true
    try {
      const enabled = !lagSwitchActive.value
      const result = await controller.setLagSwitchAsync?.(enabled, lagSwitchDelayMs.value)
      if (!result?.started) {
        lagSwitchBusy.value = false
        actionLogStore.addActionLog('lag_switch', 'Lag switch échoué', result?.error ?? 'raison inconnue', 'error')
      }
    } catch (e) {
      lagSwitchBusy.value = false
      actionLogStore.addActionLog('lag_switch', 'Lag switch échoué', String(e), 'error')
    }
  }

  function onLagSwitchFinished(result: Record<string, unknown>) {
    lagSwitchBusy.value = false
    if (result.success) {
      const active = Boolean(result.active)
      lagSwitchActive.value = active
      actionLogStore.addActionLog(
        'lag_switch',
        active ? 'Lag switch activé' : 'Lag switch désactivé',
        active ? `+${lagSwitchDelayMs.value}ms sur recv/WSARecv` : '',
        active ? 'warning' : 'success'
      )
    } else {
      actionLogStore.addActionLog('lag_switch', 'Lag switch échoué', String(result.error ?? 'raison inconnue'), 'error')
    }
  }

  return {
    // Lecture seule
    networkConnections,
    networkModules,
    networkConnectionsBusy,
    networkModulesBusy,
    networkLastRefresh,
    liveRefreshEnabled,
    _networkFilterProtocol,
    _networkFilterState,
    _networkFilterIp,
    refreshNetworkConnections,
    refreshNetworkModules,
    refreshAllNetwork,
    startLiveRefresh,
    stopLiveRefresh,
    // Proxy HTTP
    httpProxyActive,
    httpProxyBusy,
    httpProxyPort,
    httpProxyInterceptHttps,
    httpProxyRequests,
    selectedHttpRequest,
    httpRequestBodyEditor,
    startHttpProxy,
    stopHttpProxy,
    onHttpProxyStartFinished,
    onHttpProxyStopFinished,
    refreshHttpProxyRequests,
    modifySelectedHttpRequest,
    // Spoof DNS
    dnsSpoofEntries,
    dnsSpoofBusy,
    dnsSpoofDomain,
    dnsSpoofTargetIp,
    addDnsSpoofEntry,
    removeDnsSpoofEntry,
    onDnsSpoofFinished,
    onDnsRestoreFinished,
    // Lag switch
    lagSwitchActive,
    lagSwitchBusy,
    lagSwitchDelayMs,
    toggleLagSwitch,
    onLagSwitchFinished,
    // HTTP proxy polling
    startHttpProxyPolling,
    stopHttpProxyPolling,
  }
}
