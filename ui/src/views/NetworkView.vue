<script setup lang="ts">
import { computed, onMounted, onBeforeUnmount, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { useAppStore } from '@/stores/app'
import InfoDot from '@/components/expert/InfoDot.vue'
import PanelIntro from '@/components/common/PanelIntro.vue'

const store = useAppStore()
const { t } = useI18n()

// ── Blocage réseau (existant) ────────────────────────────────────
const isNetworkBlocked = computed(() => store.networkBlockStatus?.blocked === true)
const networkBlockStatusLabel = computed(() => {
  if (!store.networkBlockStatus) return t('network.block.unknown')
  return isNetworkBlocked.value ? t('network.block.blocked') : t('network.block.unblocked')
})

function toggleNetworkBlock() {
  if (isNetworkBlocked.value) {
    void store.unblockProcessNetwork()
  } else {
    void store.blockProcessNetwork()
  }
}

// ── Connexions actives ───────────────────────────────────────────
const connectionFilterProtocol = computed({
  get: () => store._networkFilterProtocol ?? 'all',
  set: (v: string) => { store._networkFilterProtocol = v }
})
const connectionFilterState = computed({
  get: () => store._networkFilterState ?? 'all',
  set: (v: string) => { store._networkFilterState = v }
})
const connectionFilterIp = computed({
  get: () => store._networkFilterIp ?? '',
  set: (v: string) => { store._networkFilterIp = v }
})

const filteredConnections = computed(() => {
  let conns = store.networkConnections ?? []
  const proto = connectionFilterProtocol.value
  const state = connectionFilterState.value
  const ip = connectionFilterIp.value.toLowerCase()
  if (proto !== 'all') conns = conns.filter(c => c.protocol.toLowerCase() === proto)
  if (state !== 'all') conns = conns.filter(c => (c.state ?? '').toUpperCase() === state)
  if (ip) conns = conns.filter(c => c.remoteAddr?.toLowerCase().includes(ip) || c.remoteHost?.toLowerCase().includes(ip))
  // Tri: ESTABLISHED d'abord, puis par remoteAddr
  return conns.sort((a, b) => {
    if (a.state === 'ESTABLISHED' && b.state !== 'ESTABLISHED') return -1
    if (a.state !== 'ESTABLISHED' && b.state === 'ESTABLISHED') return 1
    return (a.remoteAddr ?? '').localeCompare(b.remoteAddr ?? '')
  })
})

const totalConnections = computed(() => (store.networkConnections ?? []).length)
const establishedConnections = computed(() => (store.networkConnections ?? []).filter(c => c.state === 'ESTABLISHED').length)

function stateBadgeClass(state: string) {
  switch (state) {
    case 'ESTABLISHED': return 'state-established'
    case 'TIME_WAIT': return 'state-timewait'
    case 'CLOSE_WAIT': return 'state-closewait'
    case 'LISTEN': return 'state-listen'
    case 'SYN_SENT': case 'SYN_RECEIVED': return 'state-syn'
    default: return 'state-other'
  }
}

function toggleLiveRefresh() {
  if (store.liveRefreshEnabled) {
    store.stopLiveRefresh()
  } else {
    store.startLiveRefresh()
  }
}

// ── Modules DLL réseau ───────────────────────────────────────────
const categoryIcon = (cat: string) => {
  switch (cat) {
    case 'winsock': return '🔌'
    case 'http': return '🌐'
    case 'dns': return '📡'
    case 'crypto': return '🔑'
    case 'system': return '⚙️'
    default: return '📦'
  }
}

const categoryLabel = (cat: string) => {
  switch (cat) {
    case 'winsock': return t('network.modules.category_winsock')
    case 'http': return t('network.modules.category_http')
    case 'dns': return t('network.modules.category_dns')
    case 'crypto': return t('network.modules.category_crypto')
    case 'system': return t('network.modules.category_system')
    default: return cat
  }
}

// ── Proxy HTTP ───────────────────────────────────────────────────
function selectHttpRequest(id: string) {
  store.selectedHttpRequest = id
  const req = (store.httpProxyRequests ?? []).find(r => r.id === id)
  store.httpRequestBodyEditor = req?.requestBody ?? ''
}

// ── Cleanup ──────────────────────────────────────────────────────
onBeforeUnmount(() => {
  store.stopLiveRefresh()
})

onMounted(() => {
  void store.refreshProcessNetworkBlockStatus()
  if (store.isAttached) {
    void store.refreshAllNetwork()
  }
})

watch(() => store.isAttached, (attached) => {
  if (attached) {
    void store.refreshAllNetwork()
  } else {
    store.stopLiveRefresh()
  }
})
</script>

<template>
  <div class="network-view">
    <div class="header">
      <div>
        <h1>{{ $t('network.title') }}</h1>
        <p>{{ store.isAttached ? store.processName : $t('network.empty') }}</p>
      </div>
      <InfoDot topic="network" align="right" />
    </div>

    <PanelIntro
      :what="$t('network.intro')"
      :purpose="$t('network.introPurpose')"
      :how="$t('network.introHow')"
    />

    <div v-if="!store.isAttached" class="empty-state">
      <p>{{ $t('network.emptyDetail') }}</p>
      <button class="btn btn-secondary" @click="store.activeView = 'process'">{{ $t('network.goToProcess') }}</button>
    </div>

    <template v-else>
      <!-- ═══════════════════════════════════════════ -->
      <!-- SECTION A — VUE (lecture seule)            -->
      <!-- ═══════════════════════════════════════════ -->

      <!-- Section A1 : Connexions actives -->
      <section class="panel">
        <div class="panel-head">
          <h2>{{ $t('network.connections.title') }}</h2>
          <div class="panel-head-actions">
            <button class="btn btn-secondary compact" :disabled="store.networkConnectionsBusy" @click="store.refreshNetworkConnections()">
              {{ $t('network.connections.refresh') }}
            </button>
            <button
              class="btn btn-secondary compact"
              :class="{ active: store.liveRefreshEnabled }"
              :disabled="store.networkConnectionsBusy"
              @click="toggleLiveRefresh"
              :title="store.liveRefreshEnabled ? $t('network.connections.stopLiveTitle') : $t('network.connections.startLiveTitle')"
            >
              {{ $t('network.connections.live') }} 🔄
            </button>
          </div>
        </div>

        <div class="status-band">
          <span>{{ $t('network.connections.totalPrefix') }} <strong>{{ totalConnections }}</strong> {{ $t('network.connections.totalLabel') }}</span>
          <span>{{ $t('network.connections.activePrefix') }} <strong>{{ establishedConnections }}</strong> {{ $t('network.connections.establishedLabel') }}</span>
          <span v-if="store.networkLastRefresh" class="last-refresh">{{ $t('network.connections.lastRefresh') }} : {{ store.networkLastRefresh }}</span>
        </div>

        <div class="filter-bar">
          <select v-model="connectionFilterProtocol" class="filter-select">
            <option value="all">{{ $t('network.connections.allProtocols') }}</option>
            <option value="tcp">TCP</option>
            <option value="udp">UDP</option>
          </select>
          <select v-model="connectionFilterState" class="filter-select">
            <option value="all">{{ $t('network.connections.allStates') }}</option>
            <option value="ESTABLISHED">ESTABLISHED</option>
            <option value="TIME_WAIT">TIME_WAIT</option>
            <option value="CLOSE_WAIT">CLOSE_WAIT</option>
            <option value="LISTEN">LISTEN</option>
          </select>
          <input v-model="connectionFilterIp" class="filter-input" :placeholder="$t('network.connections.remoteAddr') + '...'" />
        </div>

        <div v-if="filteredConnections.length === 0" class="empty-table">
          {{ $t('network.connections.empty') }}
        </div>
        <table v-else class="data-table">
          <thead>
            <tr>
              <th>{{ $t('network.connections.protocol') }}</th>
              <th>{{ $t('network.connections.remoteHost') }}</th>
              <th>{{ $t('network.connections.remoteAddr') }}</th>
              <th>{{ $t('network.connections.state') }}</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="conn in filteredConnections" :key="conn.localAddr + conn.remoteAddr">
              <td class="proto-cell">{{ conn.protocol }}</td>
              <td class="host-cell">
                <span v-if="conn.remoteHost" class="hostname">{{ conn.remoteHost }}</span>
                <span v-if="conn.remoteHost" class="hostname-ip">({{ conn.remoteAddr }})</span>
                <span v-else class="no-host">{{ conn.remoteAddr }}</span>
              </td>
              <td class="addr-cell">{{ conn.remoteAddr }}</td>
              <td>
                <span class="state-badge" :class="stateBadgeClass(conn.state ?? '')">{{ conn.state ?? '—' }}</span>
              </td>
            </tr>
          </tbody>
        </table>
      </section>

      <!-- Section A2 : Modules DLL réseau -->
      <section class="panel">
        <div class="panel-head">
          <h2>{{ $t('network.modules.title') }}</h2>
          <button class="btn btn-secondary compact" :disabled="store.networkModulesBusy" @click="store.refreshNetworkModules()">
            {{ $t('network.connections.refresh') }}
          </button>
        </div>

        <div v-if="(store.networkModules ?? []).length === 0" class="empty-table">
          {{ $t('network.modules.empty') }}
        </div>
        <table v-else class="data-table">
          <thead>
            <tr>
              <th>{{ $t('network.modules.category') }}</th>
              <th>{{ $t('network.modules.name') }}</th>
              <th>{{ $t('network.modules.path') }}</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="mod in store.networkModules" :key="mod.name + mod.path">
              <td class="cat-cell">
                <span class="cat-icon">{{ categoryIcon(mod.category) }}</span>
                {{ categoryLabel(mod.category) }}
              </td>
              <td class="name-cell">{{ mod.name }}</td>
              <td class="path-cell">{{ mod.path }}</td>
            </tr>
          </tbody>
        </table>
        <div v-if="(store.networkModules ?? []).length > 0" class="table-footer">
          {{ store.networkModules.length }} {{ $t('network.modules.count') }}
        </div>
      </section>

      <!-- ═══════════════════════════════════════════ -->
      <!-- SECTION B — ACTION (risque élevé)          -->
      <!-- ═══════════════════════════════════════════ -->

      <!-- Section B1 : Proxy HTTP -->
      <section class="panel action-panel">
        <div class="panel-head">
          <h2>{{ $t('network.proxy.title') }}</h2>
          <span class="risk-badge risk-injection">injection</span>
        </div>
        <PanelIntro
          :what="$t('network.proxy.intro')"
          :purpose="$t('network.proxy.purpose')"
          :how="$t('network.proxy.how')"
        />

        <div class="action-config">
          <label>
            {{ $t('network.proxy.port') }}
            <input v-model.number="store.httpProxyPort" class="input small-input" type="number" min="1024" max="65535" />
          </label>
          <label class="checkbox-label">
            <input v-model="store.httpProxyInterceptHttps" type="checkbox" />
            {{ $t('network.proxy.interceptHttps') }}
          </label>
          <button
            v-if="!store.httpProxyActive"
            class="btn btn-primary"
            :disabled="store.httpProxyBusy"
            @click="store.startHttpProxy()"
          >
            {{ store.httpProxyBusy ? $t('network.proxy.starting') : $t('network.proxy.start') }}
          </button>
          <button
            v-else
            class="btn btn-secondary"
            :disabled="store.httpProxyBusy"
            @click="store.stopHttpProxy()"
          >
            {{ store.httpProxyBusy ? $t('network.proxy.stopping') : $t('network.proxy.stop') }}
          </button>
        </div>

        <div v-if="store.httpProxyActive" class="proxy-status">
          <span class="status-dot active" /> {{ $t('network.proxy.active') }} (port {{ store.httpProxyPort }})
        </div>

        <div v-if="store.httpProxyActive && (store.httpProxyRequests ?? []).length > 0" class="proxy-requests">
          <h4>{{ $t('network.proxy.requests') }}</h4>
          <div v-for="req in store.httpProxyRequests" :key="req.id" class="req-item" :class="{ selected: store.selectedHttpRequest === req.id }" @click="selectHttpRequest(req.id)">
            <div class="req-head">
              <span class="req-method" :class="'method-' + req.method.toLowerCase()">{{ req.method }}</span>
              <span class="req-url">{{ req.url }}</span>
              <span v-if="req.modified" class="req-modified">✏️ {{ $t('network.proxy.modified') }}</span>
            </div>
            <div v-if="store.selectedHttpRequest === req.id" class="req-editor">
              <textarea v-model="store.httpRequestBodyEditor" class="body-editor" :placeholder="$t('network.proxy.editorPlaceholder')" />
              <button class="btn btn-primary compact" :disabled="store.httpProxyBusy" @click.stop="store.modifySelectedHttpRequest(store.httpRequestBodyEditor)">
                {{ $t('network.proxy.apply') }}
              </button>
            </div>
          </div>
        </div>
      </section>

      <!-- Section B2 : Spoof DNS -->
      <section class="panel action-panel">
        <div class="panel-head">
          <h2>{{ $t('network.dnsSpoof.title') }}</h2>
          <span class="risk-badge risk-uac">UAC</span>
        </div>
        <PanelIntro
          :what="$t('network.dnsSpoof.intro')"
          :purpose="$t('network.dnsSpoof.purpose')"
          :how="$t('network.dnsSpoof.how')"
        />

        <div class="action-config">
          <input v-model="store.dnsSpoofDomain" class="input" :placeholder="$t('network.dnsSpoof.domain')" />
          <input v-model="store.dnsSpoofTargetIp" class="input" :placeholder="$t('network.dnsSpoof.targetIp')" />
          <button class="btn btn-primary" :disabled="store.dnsSpoofBusy || !store.dnsSpoofDomain.trim()" @click="store.addDnsSpoofEntry()">
            {{ store.dnsSpoofBusy ? $t('network.dnsSpoof.adding') : $t('network.dnsSpoof.add') }}
          </button>
        </div>

        <div v-if="(store.dnsSpoofEntries ?? []).length > 0" class="dns-entries">
          <h4>{{ $t('network.dnsSpoof.entries') }}</h4>
          <div v-for="entry in store.dnsSpoofEntries" :key="entry.domain" class="dns-entry">
            <span class="dns-domain">{{ entry.domain }}</span>
            <span class="dns-arrow">→</span>
            <span class="dns-ip">{{ entry.targetIp }}</span>
            <button class="btn btn-secondary compact" :disabled="store.dnsSpoofBusy" @click="store.removeDnsSpoofEntry(entry.domain)">
              {{ $t('network.dnsSpoof.remove') }}
            </button>
          </div>
        </div>
        <div v-else class="empty-table">{{ $t('network.dnsSpoof.noEntries') }}</div>
      </section>

      <!-- Section B3 : Lag switch -->
      <section class="panel action-panel">
        <div class="panel-head">
          <h2>{{ $t('network.lagSwitch.title') }}</h2>
          <span class="risk-badge risk-injection">injection</span>
        </div>
        <PanelIntro
          :what="$t('network.lagSwitch.intro')"
          :purpose="$t('network.lagSwitch.purpose')"
          :how="$t('network.lagSwitch.how')"
        />

        <div class="action-config">
          <label>
            {{ $t('network.lagSwitch.delay') }}
            <input v-model.number="store.lagSwitchDelayMs" class="input small-input" type="number" min="0" max="10000" step="100" />
            {{ $t('network.lagSwitch.ms') }}
          </label>
          <button
            class="btn"
            :class="store.lagSwitchActive ? 'btn-secondary' : 'btn-primary'"
            :disabled="store.lagSwitchBusy"
            @click="store.toggleLagSwitch()"
          >
            {{ store.lagSwitchBusy ? $t('network.lagSwitch.changing') : (store.lagSwitchActive ? $t('network.lagSwitch.disable') : $t('network.lagSwitch.enable')) }}
          </button>
        </div>

        <div v-if="store.lagSwitchActive" class="proxy-status">
          <span class="status-dot active" /> {{ $t('network.lagSwitch.activeDetail', { delay: store.lagSwitchDelayMs }) }}
        </div>
      </section>

      <!-- Section C : Blocage réseau (existant) -->
      <section class="panel">
        <div class="panel-head">
          <h2>{{ $t('network.block.title') }}</h2>
        </div>
        <PanelIntro
          :what="$t('network.block.intro')"
          :purpose="$t('network.block.purpose')"
          :how="$t('network.block.how')"
        />

        <div class="status-band">
          <span>{{ $t('network.block.status') }} : <strong>{{ networkBlockStatusLabel }}</strong></span>
          <span v-if="store.networkBlockStatus?.exePath">{{ $t('network.block.target') }} <strong>{{ store.networkBlockStatus.exePath }}</strong></span>
        </div>

        <div class="actions-row">
          <button
            class="btn"
            :class="isNetworkBlocked ? 'btn-secondary' : 'btn-primary'"
            :disabled="store.networkBlockBusy"
            @click="toggleNetworkBlock"
          >
            {{ isNetworkBlocked ? $t('network.block.toggleRestore') : $t('network.block.toggle') }}
          </button>
          <span class="hint">
            {{ $t('network.block.uacHint') }}
          </span>
        </div>
      </section>
    </template>
  </div>
</template>

<style scoped>
.network-view {
  padding: 24px 32px;
  max-width: 900px;
}

.header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 18px;
}

.header h1 {
  color: var(--text-primary);
  font-size: 22px;
}

.header p {
  color: var(--text-muted);
  margin-top: 4px;
}

.empty-state {
  padding: 32px;
  border: 1px solid var(--border);
  background: var(--bg-secondary);
  border-radius: 8px;
  color: var(--text-secondary);
  text-align: center;
}

.panel {
  border: 1px solid var(--border);
  background: var(--bg-secondary);
  border-radius: 8px;
  padding: 18px;
  margin-bottom: 16px;
}

.panel-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  margin-bottom: 12px;
}

.panel-head h2 {
  font-size: 16px;
  color: var(--text-primary);
  margin: 0;
}

.panel-head-actions {
  display: flex;
  gap: 8px;
}

.status-band {
  display: flex;
  gap: 20px;
  padding: 10px 14px;
  margin-bottom: 12px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
  color: var(--text-muted);
  font-size: 12px;
}

.last-refresh {
  margin-left: auto;
  color: var(--text-muted);
}

.filter-bar {
  display: flex;
  gap: 8px;
  margin-bottom: 12px;
  flex-wrap: wrap;
}

.filter-select,
.filter-input {
  font-size: 12px;
  padding: 4px 8px;
  border: 1px solid var(--border);
  border-radius: 4px;
  background: var(--bg-tertiary);
  color: var(--text-primary);
}

.filter-input {
  flex: 1;
  min-width: 120px;
}

.data-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 12px;
}

.data-table th {
  text-align: left;
  padding: 6px 10px;
  border-bottom: 1px solid var(--border);
  color: var(--text-muted);
  font-weight: 600;
  font-size: 11px;
  text-transform: uppercase;
}

.data-table td {
  padding: 6px 10px;
  border-bottom: 1px solid var(--border);
  color: var(--text-primary);
}

.data-table tbody tr:hover {
  background: var(--bg-tertiary);
}

.empty-table {
  padding: 16px;
  text-align: center;
  color: var(--text-muted);
  font-size: 12px;
}

.table-footer {
  margin-top: 8px;
  font-size: 12px;
  color: var(--text-muted);
  text-align: right;
}

/* États TCP — badges colorés */
.state-badge {
  display: inline-block;
  padding: 2px 8px;
  border-radius: 10px;
  font-size: 11px;
  font-weight: 600;
  border: 1px solid var(--border);
}

.state-established {
  color: var(--success);
  border-color: color-mix(in srgb, var(--success) 50%, var(--border));
}

.state-timewait {
  color: var(--text-muted);
  border-color: color-mix(in srgb, var(--text-muted) 50%, var(--border));
}

.state-closewait {
  color: var(--warning);
  border-color: color-mix(in srgb, var(--warning) 50%, var(--border));
}

.state-listen {
  color: #60a5fa;
  border-color: color-mix(in srgb, #60a5fa 50%, var(--border));
}

.state-syn {
  color: #fbbf24;
  border-color: color-mix(in srgb, #fbbf24 50%, var(--border));
}

.state-other {
  color: var(--text-muted);
}

/* Cellules du tableau connexions */
.proto-cell {
  font-weight: 600;
  font-family: 'Consolas', monospace;
}

.host-cell {
  max-width: 250px;
}

.hostname {
  font-weight: 600;
  color: var(--text-primary);
}

.hostname-ip {
  font-size: 11px;
  color: var(--text-muted);
  margin-left: 4px;
}

.no-host {
  font-family: 'Consolas', monospace;
  font-size: 11px;
  color: var(--text-muted);
}

.addr-cell {
  font-family: 'Consolas', monospace;
  font-size: 11px;
  color: var(--text-muted);
}

/* Cellules du tableau DLL */
.cat-cell {
  white-space: nowrap;
}

.cat-icon {
  margin-right: 4px;
}

.name-cell {
  font-family: 'Consolas', monospace;
  font-weight: 600;
}

.path-cell {
  font-family: 'Consolas', monospace;
  font-size: 11px;
  color: var(--text-muted);
  word-break: break-all;
}

/* Action panels */
.action-panel {
  border-color: color-mix(in srgb, var(--accent) 30%, var(--border));
}

.risk-badge {
  font-size: 10px;
  font-weight: 700;
  padding: 2px 8px;
  border-radius: 10px;
  text-transform: uppercase;
  letter-spacing: 0.5px;
}

.risk-injection {
  color: var(--error);
  border: 1px solid color-mix(in srgb, var(--error) 50%, var(--border));
}

.risk-uac {
  color: var(--warning);
  border: 1px solid color-mix(in srgb, var(--warning) 50%, var(--border));
}

.action-config {
  display: flex;
  align-items: center;
  gap: 12px;
  flex-wrap: wrap;
  margin-bottom: 12px;
}

.action-config .input {
  flex: 1;
  min-width: 120px;
}

.action-config .small-input {
  width: 80px;
}

.checkbox-label {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 12px;
  color: var(--text-secondary);
}

.proxy-status {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 12px;
  border-radius: 6px;
  background: color-mix(in srgb, var(--success) 10%, var(--bg-tertiary));
  color: var(--success);
  font-size: 12px;
  font-weight: 600;
  margin-bottom: 12px;
}

.status-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: var(--text-muted);
}

.status-dot.active {
  background: var(--success);
  box-shadow: 0 0 6px var(--success);
}

/* Proxy requests */
.proxy-requests {
  margin-top: 12px;
}

.proxy-requests h4 {
  font-size: 13px;
  color: var(--text-primary);
  margin: 0 0 8px;
}

.req-item {
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 8px 12px;
  margin-bottom: 6px;
  cursor: pointer;
  transition: border-color 0.15s;
}

.req-item:hover {
  border-color: var(--accent);
}

.req-item.selected {
  border-color: var(--accent);
  background: color-mix(in srgb, var(--accent) 5%, var(--bg-tertiary));
}

.req-head {
  display: flex;
  align-items: center;
  gap: 8px;
}

.req-method {
  font-size: 11px;
  font-weight: 700;
  padding: 2px 6px;
  border-radius: 4px;
  font-family: 'Consolas', monospace;
}

.method-get { color: #22c55e; background: color-mix(in srgb, #22c55e 15%, transparent); }
.method-post { color: #3b82f6; background: color-mix(in srgb, #3b82f6 15%, transparent); }
.method-put { color: #f59e0b; background: color-mix(in srgb, #f59e0b 15%, transparent); }
.method-delete { color: #ef4444; background: color-mix(in srgb, #ef4444 15%, transparent); }

.req-url {
  font-size: 12px;
  font-family: 'Consolas', monospace;
  color: var(--text-primary);
  word-break: break-all;
}

.req-modified {
  font-size: 11px;
  color: var(--warning);
  margin-left: auto;
}

.req-editor {
  margin-top: 8px;
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.body-editor {
  width: 100%;
  min-height: 80px;
  font-family: 'Consolas', monospace;
  font-size: 12px;
  padding: 8px;
  border: 1px solid var(--border);
  border-radius: 4px;
  background: var(--bg-tertiary);
  color: var(--text-primary);
  resize: vertical;
}

/* DNS entries */
.dns-entries {
  margin-top: 12px;
}

.dns-entries h4 {
  font-size: 13px;
  color: var(--text-primary);
  margin: 0 0 8px;
}

.dns-entry {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 6px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  margin-bottom: 4px;
  font-size: 12px;
}

.dns-domain {
  font-family: 'Consolas', monospace;
  font-weight: 600;
  color: var(--text-primary);
}

.dns-arrow {
  color: var(--accent);
  font-weight: 700;
}

.dns-ip {
  font-family: 'Consolas', monospace;
  color: var(--text-muted);
}

/* Existing block panel */
.actions-row {
  display: flex;
  align-items: center;
  gap: 14px;
}

.hint {
  color: var(--text-muted);
  font-size: 12px;
}

.intro {
  margin-bottom: 18px;
}

.btn.active {
  border-color: var(--accent);
  background: color-mix(in srgb, var(--accent) 15%, var(--bg-tertiary));
  color: var(--accent);
}
</style>
