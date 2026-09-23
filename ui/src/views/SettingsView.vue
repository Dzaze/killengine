<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { useAppStore } from '@/stores/app'
import AssistantToolsPanel from '@/components/settings/AssistantToolsPanel.vue'
import PanelIntro from '@/components/common/PanelIntro.vue'

const store = useAppStore()
const { locale, t } = useI18n()

const valueTypes = ['Int8', 'UInt8', 'Int16', 'UInt16', 'Int32', 'UInt32', 'Int64', 'UInt64', 'Float32', 'Float64']
const performanceModes = ['Auto', 'Eco', 'Normal', 'Performance', 'Max']
const unknownSnapshotPresets = [-1, 128, 512, 1024, 2048, 4096, 8192]
const unknownDepthLabel = (mb: number) => (mb === -1 ? t('settings.autoLabel') : t('settings.mbLabel', { mb }))
const externalAiApiKeyInput = ref('')
async function saveExternalAiApiKey() {
  const result = await store.setExternalAiApiKey(externalAiApiKeyInput.value)
  if (result?.success) {
    externalAiApiKeyInput.value = ''
  }
}
const visibleModelCandidates = computed(() =>
  (store.aiModelStatus?.modelCandidates ?? []).slice(0, 6),
)
const visibleExecutableCandidates = computed(() =>
  (store.aiModelStatus?.executableCandidates ?? []).slice(0, 6),
)
// UX-PRODUIT-11B : embeddedAgents est déjà regroupé par identité logique
// (id, role) côté C++ (settings_diagnostics_manager.cpp) -- une entrée ici
// = un rôle IA distinct, jamais un manifeste brut. `sources` liste les
// copies (une par racine où le manifeste a été trouvé), dépliées à la
// demande via "Voir les sources" plutôt que noyer le résumé.
const visibleEmbeddedAgents = computed(() =>
  (store.aiModelStatus?.embeddedAgents ?? []).slice(0, 8).map((agent) => {
    const id = String(agent.id ?? '')
    const role = String(agent.role ?? 'agent')
    const sources = Array.isArray(agent.sources)
      ? (agent.sources as Array<Record<string, unknown>>).map((source) => ({
          manifestPath: String(source.manifestPath ?? ''),
          folderName: String(source.folderName ?? ''),
          rootPath: String(source.rootPath ?? ''),
          modelFound: source.modelFound === true,
        }))
      : []
    return {
      key: `${id}::${role}`,
      id,
      displayName: String(agent.displayName ?? agent.id ?? 'Agent IA'),
      role,
      modelPath: String(agent.modelPath ?? ''),
      modelFound: agent.modelFound === true,
      valid: agent.valid !== false,
      sources,
    }
  }),
)
const visibleAgentIssues = computed(() =>
  (store.aiModelStatus?.embeddedAgentIssues ?? []).slice(0, 8).map((issue) => ({
    folderName: String(issue.folderName ?? ''),
    rootPath: String(issue.rootPath ?? ''),
    error: String(issue.error ?? ''),
  })),
)
const expandedAgentSourceKeys = ref<Set<string>>(new Set())
function toggleAgentSources(key: string) {
  const next = new Set(expandedAgentSourceKeys.value)
  if (next.has(key)) {
    next.delete(key)
  } else {
    next.add(key)
  }
  expandedAgentSourceKeys.value = next
}

function formatBytes(value: number | undefined) {
  const bytes = value ?? 0
  if (bytes >= 1024 * 1024 * 1024) return t('settings.gbUnit', { value: (bytes / (1024 * 1024 * 1024)).toFixed(2) })
  if (bytes >= 1024 * 1024) return t('settings.mbUnit', { value: (bytes / (1024 * 1024)).toFixed(1) })
  if (bytes >= 1024) return t('settings.kbUnit', { value: (bytes / 1024).toFixed(1) })
  return t('settings.bUnit', { value: bytes })
}

async function refreshAll() {
  await store.doPing()
  await store.loadSettings()
  await store.refreshTemporaryStorageStatus()
}

// UX-PRODUIT-8B : les diagnostics (noyau, automation, runtime/journaux,
// WebView2) ont leur propre chargement dans Modules > Diagnostics — ce
// bouton n'y navigue plus qu'un lien, il ne les rafraîchit plus ici.
function openDiagnostics() {
  store.pendingModulesTab = 'diagnostics'
  store.activeView = 'modules'
}

onMounted(() => {
  void refreshAll()
})

watch(
  () => store.appLanguage,
  (language) => {
    locale.value = language
  },
)

watch(
  () => locale.value,
  (language) => {
    store.appLanguage = language === 'en' ? 'en' : 'fr'
  },
  { immediate: true },
)

async function saveAll() {
  locale.value = store.appLanguage
  await store.saveSettings()
}

</script>

<template>
  <div class="settings-view">
    <div class="header">
      <div>
        <h1>{{ $t('nav.settings') }}</h1>
        <p>{{ store.statusText }}</p>
      </div>
      <button class="btn btn-secondary" @click="refreshAll">
        {{ $t('settings.refresh') }}
      </button>
    </div>

    <PanelIntro
      :what="$t('settings.intro.what')"
      :purpose="$t('settings.intro.purpose')"
      :how="$t('settings.intro.how')"
    />

    <nav class="settings-toc" aria-label="Sommaire">
      <a href="#settings-interface">{{ $t('settings.tocInterface') }}</a>
      <a href="#settings-scan-storage">{{ $t('settings.tocScanStorage') }}</a>
      <a href="#settings-ai">{{ $t('settings.tocAi') }}</a>
      <a href="#settings-advanced">{{ $t('settings.tocAdvanced') }}</a>
    </nav>

    <section id="settings-interface" class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.interfaceTitle') }}</h2>
      </div>
      <div class="setting-row">
        <div>
          <strong>{{ $t('settings.language') }}</strong>
          <span>{{ store.appLanguage === 'fr' ? $t('settings.french') : $t('settings.english') }}</span>
        </div>
        <div class="segmented">
          <button :class="{ active: store.appLanguage === 'fr' }" @click="store.switchLanguage('fr')">FR</button>
          <button :class="{ active: store.appLanguage === 'en' }" @click="store.switchLanguage('en')">EN</button>
        </div>
      </div>
    </section>

    <section id="settings-scan-storage" class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.scanTitle') }}</h2>
      </div>
      <div class="settings-grid">
        <label>
          <span>{{ $t('settings.defaultType') }}</span>
          <select v-model="store.settingDefaultValueType" class="input select">
            <option v-for="type in valueTypes" :key="type">{{ type }}</option>
          </select>
        </label>
        <label>
          <span>{{ $t('settings.maxResults') }}</span>
          <input v-model.number="store.settingScanMaxResults" class="input" type="number" min="1000" max="10000000" step="1000" />
        </label>
        <label>
          <span>{{ $t('settings.performanceMode') }}</span>
          <select v-model="store.settingPerformanceMode" class="input select">
            <option v-for="mode in performanceModes" :key="mode">{{ mode }}</option>
          </select>
        </label>
        <label>
          <span>{{ $t('settings.scanChunk') }}</span>
          <input v-model.number="store.settingScanChunkSizeMb" class="input" type="number" min="0" max="64" step="1" />
        </label>
        <label>
          <span>{{ $t('settings.maxThreads') }}</span>
          <input v-model.number="store.settingScanMaxWorkerThreads" class="input" type="number" min="0" max="128" step="1" />
        </label>
        <label>
          <span>{{ $t('settings.scanInFlightMemory') }}</span>
          <input v-model.number="store.settingScanMaxInFlightMb" class="input" type="number" min="0" max="32768" step="64" />
        </label>
        <label>
          <span>{{ $t('settings.candidateFileThreshold') }}</span>
          <input v-model.number="store.settingCandidateFileBackedThreshold" class="input" type="number" min="1" max="5000000" step="1000" />
        </label>
        <label>
          <span>{{ $t('settings.unknownSnapshotMax') }}</span>
          <select v-model.number="store.settingUnknownSnapshotMaxMb" class="input select">
            <option v-for="mb in unknownSnapshotPresets" :key="mb" :value="mb">{{ unknownDepthLabel(mb) }}</option>
          </select>
        </label>
        <label class="toggle-row">
          <input v-model="store.settingFastScan" type="checkbox" />
          <span>{{ $t('settings.fastScanDefault') }}</span>
        </label>
      </div>
      <p class="hint">
        {{ $t('settings.scanHint') }}
      </p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.tempStorageTitle') }}</h2>
        <div class="panel-actions">
          <button class="btn btn-secondary compact" :disabled="store.scanBusy" @click="store.refreshTemporaryStorageStatus()">
            {{ $t('settings.refreshAction') }}
          </button>
          <button class="btn btn-primary compact" :disabled="store.scanBusy" @click="store.clearTemporaryStorage()">
            {{ $t('settings.cleanupNow') }}
          </button>
        </div>
      </div>
      <div class="runtime-grid">
        <div class="runtime-cell">
          <span>{{ $t('settings.total') }}</span>
          <strong>{{ formatBytes(store.temporaryStorageStatus?.totalBytes) }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.active') }}</span>
          <strong>{{ formatBytes(store.temporaryStorageStatus?.activeBytes) }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.orphans') }}</span>
          <strong>{{ store.temporaryStorageStatus?.orphanFileCount ?? 0 }} · {{ formatBytes(store.temporaryStorageStatus?.orphanBytes) }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.activeFiles') }}</span>
          <strong>{{ store.temporaryStorageStatus?.activeFileCount ?? 0 }}</strong>
        </div>
      </div>
      <div class="path-row">
        <span>{{ $t('settings.tempFolder') }}</span>
        <code>{{ store.temporaryStorageStatus?.tempPath || '-' }}</code>
      </div>
      <div class="path-row">
        <span>{{ $t('settings.candidates') }}</span>
        <code>{{ formatBytes(store.temporaryStorageStatus?.candidateBytes) }} · {{ store.temporaryStorageStatus?.candidateFileBacked ? $t('settings.fileBacked') : $t('settings.ram') }}</code>
      </div>
      <div class="path-row">
        <span>{{ $t('settings.undoSnapshot') }}</span>
        <code>{{ formatBytes(store.temporaryStorageStatus?.undoBytes) }} / {{ formatBytes(store.temporaryStorageStatus?.snapshotBytes) }}</code>
      </div>
      <p v-if="store.temporaryStorageCleanupResult" class="status-line">
        {{ store.temporaryStorageCleanupResult.message || (store.temporaryStorageCleanupResult.success ? $t('settings.cleanupDone') : store.temporaryStorageCleanupResult.error) }}
      </p>
      <p v-if="store.temporaryStorageError" class="error">{{ store.temporaryStorageError }}</p>
      <p class="hint">
        {{ $t('settings.tempStorageHint') }}
      </p>
    </section>

    <section id="settings-ai" class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.localAiTitle') }}</h2>
        <div class="panel-actions">
          <!-- UX-PIPE-2 : "installé" (available) et "prêt à répondre" (ready) sont
               deux états distincts -- une IA désactivée (session ou réglage) ne
               doit jamais s'afficher comme prête, ni comme fichiers manquants. -->
          <span class="status-pill" :class="store.aiModelStatus?.ready ? 'ok' : 'warn'">
            {{ store.aiModelStatus?.ready
              ? $t('settings.aiReady')
              : store.aiModelStatus?.available
                ? $t('settings.aiDisabled')
                : $t('settings.aiUnavailable') }}
          </span>
          <button class="btn btn-secondary compact" :disabled="store.aiModelStatusLoading" @click="store.refreshAiModelStatus()">
            {{ store.aiModelStatusLoading ? $t('settings.checking') : $t('settings.verify') }}
          </button>
        </div>
      </div>
      <div class="settings-grid">
        <label class="wide">
          <span>{{ $t('settings.ggufCustomPath') }}</span>
          <div class="model-path-row">
            <input v-model="store.settingModelPath" class="input" :placeholder="$t('settings.ggufPathPlaceholder')" />
            <button class="btn btn-secondary compact" type="button" @click="store.browseForModel()">{{ $t('settings.browse') }}</button>
          </div>
        </label>
        <label>
          <span>{{ $t('settings.modelThreads') }}</span>
          <input v-model.number="store.settingModelThreads" class="input" type="number" min="1" max="32" step="1" />
        </label>
      </div>
      <div class="model-status-grid">
        <div class="runtime-cell">
          <span>{{ $t('settings.backend') }}</span>
          <strong>{{ store.aiModelStatus?.backend || '-' }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.product') }}</span>
          <strong>{{ $t('settings.embeddedAiRequired') }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.aiAgents') }}</span>
          <strong>{{ store.aiModelStatus?.embeddedAgentCount ?? visibleEmbeddedAgents.length }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.model') }}</span>
          <strong>{{ store.aiModelStatus?.modelFound ? $t('settings.found') : $t('settings.notFound') }}</strong>
        </div>
        <div class="runtime-cell">
          <span>llama-cli</span>
          <strong>{{ store.aiModelStatus?.executableFound ? $t('settings.found') : $t('settings.notFound') }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.threads') }}</span>
          <strong>{{ store.aiModelStatus?.threads ?? store.settingModelThreads }}</strong>
        </div>
      </div>
      <div class="resolved-model-box">
        <strong>{{ $t('settings.resolvedModelTitle') }}</strong>
        <div class="path-row">
          <span>{{ $t('settings.modelDetected') }}</span>
          <code>{{ store.aiModelStatus?.modelPath || store.settingModelPath || '-' }}</code>
        </div>
        <p v-if="store.aiModelStatus?.configuredModelMissing" class="warning">
          {{ store.aiModelStatus.configuredModelWarning }}
        </p>
        <div class="path-row">
          <span>{{ $t('settings.activeRuntime') }}</span>
          <code>{{ store.aiModelStatus?.executablePath || '-' }}</code>
        </div>
        <p class="hint">{{ $t('settings.resolvedModelHint') }}</p>
      </div>
      <div v-if="visibleEmbeddedAgents.length" class="embedded-agent-list">
        <strong>{{ $t('settings.aiRolesDetected', { count: store.aiModelStatus?.embeddedAgentCount ?? visibleEmbeddedAgents.length }) }}</strong>
        <div v-for="agent in visibleEmbeddedAgents" :key="agent.key" class="ai-role-row">
          <div class="ai-role-row-main">
            <span :class="agent.modelFound && agent.valid ? 'ok-text' : 'dim-text'">{{ agent.modelFound && agent.valid ? $t('settings.roleValid') : $t('settings.roleIncomplete') }}</span>
            <code>{{ agent.displayName }} · {{ agent.role }}</code>
            <button
              v-if="agent.sources.length"
              class="btn btn-secondary compact"
              type="button"
              @click="toggleAgentSources(agent.key)"
            >
              {{ expandedAgentSourceKeys.has(agent.key) ? $t('settings.hideSources') : $t('settings.viewSources') }}
            </button>
          </div>
          <div v-if="expandedAgentSourceKeys.has(agent.key)" class="ai-role-sources">
            <div v-for="(source, idx) in agent.sources" :key="idx" class="candidate-path">
              <span :class="source.modelFound ? 'ok-text' : 'dim-text'">{{ source.modelFound ? $t('settings.ok') : '--' }}</span>
              <code>{{ source.manifestPath || source.folderName }}</code>
            </div>
          </div>
        </div>
        <div v-if="visibleAgentIssues.length" class="ai-role-issues">
          <strong>{{ $t('settings.agentIssuesTitle') }}</strong>
          <div v-for="(issue, idx) in visibleAgentIssues" :key="idx" class="candidate-path">
            <span class="dim-text">!</span>
            <code>{{ issue.folderName }} · {{ issue.error }}</code>
          </div>
        </div>
      </div>
      <p v-if="store.aiModelStatus?.message" class="status-line">{{ store.aiModelStatus.message }}</p>
      <p v-if="store.aiModelStatus?.modelError && !store.aiModelStatus?.modelFound" class="warning">{{ store.aiModelStatus.modelError }}</p>
      <p v-if="store.aiModelStatusError" class="error">{{ store.aiModelStatusError }}</p>
      <p class="hint">
        {{ $t('settings.localAiHint1') }}
      </p>
      <details class="model-candidates">
        <summary>{{ $t('settings.inspectedPaths') }}</summary>
        <div class="candidate-columns">
          <div>
            <strong>{{ $t('settings.ggufModels') }}</strong>
            <div v-for="candidate in visibleModelCandidates" :key="String(candidate.path)" class="candidate-path">
              <span :class="candidate.exists ? 'ok-text' : 'dim-text'">{{ candidate.exists ? $t('settings.ok') : '--' }}</span>
              <code>{{ candidate.path }}</code>
            </div>
          </div>
          <div>
            <strong>llama-cli</strong>
            <div v-for="candidate in visibleExecutableCandidates" :key="String(candidate.path)" class="candidate-path">
              <span :class="candidate.exists ? 'ok-text' : 'dim-text'">{{ candidate.exists ? $t('settings.ok') : '--' }}</span>
              <code>{{ candidate.path }}</code>
            </div>
          </div>
        </div>
      </details>
      <p class="hint">
        {{ $t('settings.localAiHint2') }}
      </p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.externalAiTitle') }}</h2>
        <div class="panel-actions">
          <span class="status-pill" :class="store.externalAiActiveBackend === 'claude' ? 'ok' : 'warn'">
            {{ store.externalAiActiveBackend === 'claude' ? $t('settings.claudeActive') : $t('settings.localActive') }}
          </span>
        </div>
      </div>
      <p class="hint">
        {{ $t('settings.externalAiHint1') }}
      </p>
      <p class="hint">
        {{ $t('settings.externalAiHint2Prefix') }} <strong>{{ $t('settings.automationModeStrong') }}</strong> {{ $t('settings.externalAiHint2Suffix') }}
      </p>
      <div class="settings-grid">
        <label>
          <span>{{ $t('settings.activeBackend') }}</span>
          <select
            class="input"
            :value="store.externalAiActiveBackend"
            :disabled="store.externalAiBusy"
            @change="store.setActiveAiBackend(($event.target as HTMLSelectElement).value as 'local' | 'claude')"
          >
            <option value="local">{{ $t('settings.localEmbeddedOption') }}</option>
            <option value="claude">{{ $t('settings.claudeApiOption') }}</option>
          </select>
        </label>
      </div>
      <div class="model-path-row">
        <input
          v-model="externalAiApiKeyInput"
          class="input"
          type="password"
          autocomplete="off"
          :placeholder="$t('settings.claudeApiKeyPlaceholder')"
        />
        <button
          class="btn btn-secondary compact"
          type="button"
          :disabled="store.externalAiBusy || !externalAiApiKeyInput.trim()"
          @click="saveExternalAiApiKey()"
        >
          {{ $t('settings.save') }}
        </button>
        <button
          class="btn btn-secondary compact"
          type="button"
          :disabled="store.externalAiBusy || !store.externalAiHasApiKey"
          @click="store.clearExternalAiApiKey()"
        >
          {{ $t('settings.remove') }}
        </button>
      </div>
      <div class="model-status-grid">
        <div class="runtime-cell">
          <span>{{ $t('settings.keySaved') }}</span>
          <strong>{{ store.externalAiHasApiKey ? $t('settings.yes') : $t('settings.no') }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.requestsThisSession') }}</span>
          <strong>{{ store.externalAiRequestCount }}</strong>
        </div>
      </div>
      <p v-if="store.externalAiError" class="error">{{ store.externalAiError }}</p>
      <p class="hint">
        {{ $t('settings.externalAiHint3') }}
      </p>
    </section>

    <AssistantToolsPanel />

    <section id="settings-advanced" class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.workspaceTitle') }}</h2>
      </div>
      <p class="hint">{{ $t('settings.workspaceMovedHint') }}</p>
      <button class="btn btn-primary" type="button" @click="store.activeView = 'project'">
        {{ $t('settings.openProjectView') }}
      </button>
    </section>


    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.diagnosticTitle') }}</h2>
      </div>
      <p class="hint">{{ $t('settings.diagnosticsMovedHint') }}</p>
      <button class="btn btn-primary" type="button" @click="openDiagnostics()">
        {{ $t('settings.openDiagnostics') }}
      </button>
      <div class="setting-row inline-setting">
        <div>
          <strong>{{ $t('settings.debugSmartSearch') }}</strong>
          <span>{{ store.settingSmartSearchDebugEnabled ? $t('settings.enabled') : $t('settings.disabled') }}</span>
        </div>
        <label class="toggle-row">
          <input v-model="store.settingSmartSearchDebugEnabled" type="checkbox" />
          <span>{{ $t('settings.writeJsonl') }}</span>
        </label>
      </div>
      <div class="setting-row inline-setting">
        <div>
          <strong>{{ $t('settings.eventsDisplayed') }}</strong>
          <span>{{ store.settingSmartSearchDebugMaxEvents }}</span>
        </div>
        <input v-model.number="store.settingSmartSearchDebugMaxEvents" class="input short-input" type="number" min="5" max="200" step="5" />
      </div>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.portabilityTitle') }}</h2>
      </div>
      <p class="hint">
        {{ $t('settings.portabilityHint') }}
      </p>
      <p class="hint">
        <strong>{{ $t('settings.portabilityApiKeyLabel') }}</strong> — {{ $t('settings.portabilityApiKeyText') }}
      </p>
      <p class="hint">
        <strong>{{ $t('settings.portabilityKernelDriverLabel') }}</strong> — {{ $t('settings.portabilityKernelDriverText') }}
      </p>
      <p class="hint">
        <strong>{{ $t('settings.portabilityDefenderLabel') }}</strong> — {{ $t('settings.portabilityDefenderText') }}
      </p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.sessionTitle') }}</h2>
      </div>
      <div class="actions-row">
        <button class="btn btn-primary" :disabled="store.settingsSaving" @click="saveAll">
          {{ store.settingsSaving ? $t('settings.saving') : $t('settings.saveSettings') }}
        </button>
        <button class="btn btn-secondary" @click="store.resetWorkflow()">
          {{ $t('settings.resetWorkflow') }}
        </button>
        <button class="btn btn-secondary" :disabled="!store.isAttached" @click="store.detach()">
          {{ $t('process.detach') }}
        </button>
      </div>
      <!-- UX-PIPE-5 : distinction visuelle explicite, plus seulement le texte
           -- sinon un échec de persistance disque n'affichait qu'un message
           neutre indiscernable d'un succès. -->
      <p v-if="store.settingsStatus" class="status-line" :class="{ error: store.settingsSaveError }">{{ store.settingsStatus }}</p>
    </section>
  </div>
</template>

<style scoped>
.settings-view {
  max-width: 980px;
  padding: 24px 32px;
}

.header,
.panel-title,
.setting-row,
.ping-line,
.actions-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.panel-title span,
.empty-line {
  color: var(--text-muted);
  font-size: 12px;
}

.panel-actions {
  display: flex;
  align-items: center;
  gap: 8px;
}

.workspace-actions {
  flex-wrap: wrap;
  justify-content: flex-start;
  margin-top: 10px;
}

.workspace-export {
  margin-top: 12px;
}

.workspace-import {
  margin-top: 12px;
  padding-top: 10px;
  border-top: 1px solid var(--border);
}

.project-panel,
.bookmark-list,
.audit-panel {
  margin-top: 12px;
  padding-top: 10px;
  border-top: 1px solid var(--border);
}

.project-name-input {
  min-width: 180px;
}

.bookmark-create {
  display: grid;
  grid-template-columns: 1fr 1fr 120px 140px 1.5fr auto;
  gap: 8px;
  padding: 10px 0;
}

.bookmark-note-input {
  min-width: 0;
}

.project-row,
.bookmark-row,
.audit-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
  padding: 8px 0;
  border-top: 1px solid rgba(255, 255, 255, 0.06);
}

.project-row:first-of-type,
.bookmark-row:first-of-type,
.audit-row:first-of-type {
  border-top: none;
}

.project-row div,
.bookmark-row div,
.audit-row div {
  display: flex;
  min-width: 0;
  flex-direction: column;
  gap: 3px;
}

.project-row span,
.bookmark-row span,
.audit-row span {
  overflow: hidden;
  color: var(--text-muted);
  font-size: 12px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

@media (max-width: 980px) {
  .bookmark-create {
    grid-template-columns: 1fr 1fr;
  }
}

.workspace-import-input {
  width: 100%;
  min-height: 120px;
  resize: vertical;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
  line-height: 1.45;
  padding: 10px;
}

.import-preview {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin-top: 8px;
}

.import-preview span {
  border: 1px solid var(--border);
  border-radius: 999px;
  color: var(--text-muted);
  font-size: 11px;
  padding: 4px 8px;
}

.template-list {
  margin-top: 12px;
  padding-top: 10px;
  border-top: 1px solid var(--border);
}

.template-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
  padding: 8px 0;
  border-top: 1px solid rgba(255, 255, 255, 0.06);
}

.template-row:first-of-type {
  border-top: none;
}

.template-row div {
  display: flex;
  min-width: 0;
  flex-direction: column;
  gap: 3px;
}

.template-row span {
  overflow: hidden;
  color: var(--text-muted);
  font-size: 12px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.template-detail {
  margin-top: 10px;
  padding: 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.template-field-row {
  display: grid;
  grid-template-columns: 58px minmax(70px, 0.5fr) minmax(90px, 1fr) minmax(90px, 1fr) minmax(140px, 1.4fr);
  gap: 8px;
  align-items: center;
  min-height: 28px;
  padding: 5px 0;
  border-top: 1px solid rgba(255, 255, 255, 0.06);
  color: var(--text-muted);
  font-size: 12px;
}

.template-field-row:first-of-type {
  border-top: none;
}

.template-field-row code,
.template-field-row strong,
.template-field-row span {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.workspace-export pre {
  max-height: 320px;
  overflow: auto;
  padding: 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
  line-height: 1.45;
  white-space: pre-wrap;
}

.header {
  margin-bottom: 18px;
}

.header h1 {
  color: var(--text-primary);
  font-size: 22px;
}

.header p {
  margin-top: 4px;
  color: var(--text-muted);
  font-size: 13px;
}

.panel {
  margin-bottom: 12px;
  padding: 14px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
}

.panel-title {
  margin-bottom: 12px;
}

.panel-title h2 {
  color: var(--text-primary);
  font-size: 15px;
}

.panel-title h3 {
  color: var(--text-primary);
  font-size: 13px;
}

.setting-row strong,
.runtime-cell strong {
  display: block;
  color: var(--text-primary);
  font-size: 14px;
}

.settings-grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 10px;
}

.settings-grid label,
.toggle-row {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.settings-grid label span,
.toggle-row span,
.hint,
.status-line {
  color: var(--text-muted);
  font-size: 12px;
}

.status-line.error {
  color: var(--error);
  font-weight: 600;
}

.input {
  min-width: 0;
  width: 100%;
  min-height: 34px;
  padding: 8px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-size: 13px;
  outline: none;
  transition: border-color 0.15s, box-shadow 0.15s, background 0.15s;
}

.list-filter-input {
  margin: 8px 0;
}

.list-show-more {
  align-self: center;
  margin: 8px auto 0;
  display: block;
}

.input::placeholder {
  color: var(--text-muted);
}

.input:focus {
  border-color: rgba(122, 162, 247, 0.8);
  box-shadow: 0 0 0 2px rgba(122, 162, 247, 0.15);
}

.input:disabled {
  cursor: not-allowed;
  opacity: 0.45;
}

.input[type="number"] {
  appearance: textfield;
  -moz-appearance: textfield;
}

.input[type="number"]::-webkit-outer-spin-button,
.input[type="number"]::-webkit-inner-spin-button {
  margin: 0;
  appearance: none;
  -webkit-appearance: none;
}

.select {
  cursor: pointer;
}

.select option {
  background: var(--bg-primary);
  color: var(--text-primary);
}

.settings-grid .wide {
  grid-column: span 2;
}

.model-path-row {
  display: flex;
  gap: 8px;
}

.model-path-row .input {
  flex: 1;
  min-width: 0;
}

.toggle-row {
  align-items: flex-start;
  justify-content: center;
}

.settings-grid .toggle-row,
.inline-setting .toggle-row {
  flex-direction: row;
  align-items: center;
  justify-content: flex-start;
  min-height: 34px;
}

.toggle-row input {
  width: 15px;
  height: 15px;
  margin: 0;
  accent-color: var(--accent);
}

.inline-setting {
  padding-top: 10px;
  border-top: 1px solid var(--border);
}

.short-input {
  max-width: 120px;
}

.hint,
.status-line {
  margin-top: 10px;
}

.setting-row span,
.runtime-cell span,
.path-row span {
  display: block;
  color: var(--text-muted);
  font-size: 12px;
}

.segmented {
  display: inline-flex;
  padding: 3px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.segmented button {
  min-width: 42px;
  padding: 6px 10px;
  border: none;
  border-radius: 4px;
  background: transparent;
  color: var(--text-muted);
  cursor: pointer;
}

.segmented button.active {
  background: var(--bg-accent);
  color: var(--accent);
}

.runtime-grid {
  display: grid;
  grid-template-columns: repeat(4, minmax(120px, 1fr));
  gap: 8px;
}

.model-status-grid {
  display: grid;
  grid-template-columns: repeat(4, minmax(120px, 1fr));
  gap: 8px;
  margin-top: 12px;
}

.runtime-cell {
  min-height: 72px;
  padding: 11px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.runtime-cell strong {
  overflow: hidden;
  margin-top: 8px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.status-pill {
  border: 1px solid var(--border);
  border-radius: 999px;
  font-size: 11px;
  padding: 4px 8px;
}

.status-pill.ok {
  border-color: rgba(158, 206, 106, 0.45);
  color: var(--success);
}

.status-pill.warn {
  border-color: rgba(224, 175, 104, 0.45);
  color: var(--warning);
}

.model-candidates {
  margin-top: 10px;
  color: var(--text-muted);
  font-size: 12px;
}

.resolved-model-box {
  margin-top: 10px;
  padding: 10px 12px;
  border: 1px solid var(--border);
  border-radius: 8px;
  background: var(--bg-tertiary);
}

.resolved-model-box > strong {
  display: block;
  margin-bottom: 4px;
  color: var(--text-primary);
}

.resolved-model-box .path-row {
  padding: 6px 0;
}

.resolved-model-box .hint {
  margin-top: 6px;
}

.embedded-agent-list {
  margin-top: 10px;
  color: var(--text-muted);
  font-size: 12px;
}

.embedded-agent-list > strong {
  color: var(--text-primary);
}

.ai-role-row {
  margin-top: 8px;
  padding: 6px 0;
  border-top: 1px solid var(--border);
}

.ai-role-row:first-of-type {
  border-top: none;
}

.ai-role-row-main {
  display: flex;
  align-items: center;
  gap: 8px;
}

.ai-role-row-main code {
  overflow: hidden;
  flex: 1;
  min-width: 0;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.ai-role-sources {
  margin-top: 6px;
  margin-left: 34px;
}

.ai-role-issues {
  margin-top: 10px;
  padding-top: 8px;
  border-top: 1px solid var(--border);
}

.ai-role-issues > strong {
  color: var(--text-primary);
}

.model-candidates summary {
  cursor: pointer;
}

.candidate-columns {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 12px;
  margin-top: 8px;
}

.candidate-path {
  display: grid;
  grid-template-columns: 28px minmax(0, 1fr);
  gap: 6px;
  align-items: center;
  margin-top: 5px;
}

.candidate-path code {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.ok-text {
  color: var(--success);
}

.dim-text {
  color: var(--text-muted);
}

.ping-line {
  margin-top: 12px;
}

.path-row {
  display: grid;
  grid-template-columns: 150px minmax(0, 1fr);
  gap: 12px;
  align-items: center;
  padding: 8px 0;
  border-top: 1px solid var(--border);
}

.path-row:first-of-type {
  border-top: none;
}

.remembered-patterns {
  margin-top: 12px;
}

.remembered-pattern-row {
  display: grid;
  grid-template-columns: minmax(160px, 1fr) 80px 50px minmax(0, 1.5fr) auto;
  gap: 10px;
  align-items: center;
  padding: 6px 8px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
  margin-bottom: 4px;
  font-size: 12px;
}

.remembered-pattern-row code {
  overflow: hidden;
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.remembered-pattern-label {
  display: flex;
  flex-direction: column;
  gap: 2px;
  min-width: 0;
}

.remembered-pattern-label strong {
  overflow: hidden;
  color: var(--text-primary);
  font-size: 12px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.debug-list {
  display: flex;
  max-height: 360px;
  flex-direction: column;
  gap: 6px;
  overflow-y: auto;
}

.kernel-learning {
  display: grid;
  gap: 10px;
  margin: 12px 0;
  padding: 10px;
  border: 1px solid color-mix(in srgb, var(--accent) 28%, var(--border));
  border-radius: 6px;
  background: color-mix(in srgb, var(--accent) 7%, var(--bg-secondary));
}

.kernel-learning-head,
.kernel-learning-step {
  display: flex;
  justify-content: space-between;
  gap: 10px;
}

.kernel-learning-head strong {
  color: var(--text-primary);
}

.kernel-learning-head span,
.kernel-learning-step span {
  color: var(--text-muted);
  font-size: 12px;
}

.kernel-learning-steps {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 8px;
}

.kernel-learning-step {
  min-height: 72px;
  flex-direction: column;
  justify-content: flex-start;
  padding: 8px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.kernel-learning-step strong {
  color: var(--text-primary);
  font-size: 12px;
}

.log-viewer {
  max-height: 320px;
  overflow: auto;
  padding: 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-secondary);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
  line-height: 1.45;
  white-space: pre-wrap;
}

.debug-row {
  padding: 9px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.debug-head {
  display: flex;
  justify-content: space-between;
  gap: 10px;
  margin-bottom: 5px;
}

.debug-head strong {
  color: var(--text-primary);
  font-size: 13px;
}

.debug-head span,
.debug-row p {
  color: var(--text-muted);
  font-size: 12px;
}

.debug-row p {
  margin-top: 5px;
}

.error {
  color: var(--error) !important;
}

code {
  overflow-wrap: anywhere;
  color: var(--text-secondary);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
}

.btn {
  padding: 8px 14px;
  border: none;
  border-radius: 6px;
  cursor: pointer;
  font-size: 13px;
  transition: all 0.15s;
}

.compact {
  padding: 5px 9px;
  font-size: 12px;
}

.btn:disabled {
  cursor: not-allowed;
  opacity: 0.45;
}

.btn-secondary {
  background: var(--bg-accent);
  color: var(--text-secondary);
}

.danger-action {
  color: var(--error);
}

.btn-primary {
  background: var(--accent);
  color: var(--bg-primary);
  font-weight: 600;
}

.btn-primary:hover:not(:disabled) {
  background: var(--accent-hover);
}

@media (max-width: 850px) {
  .runtime-grid,
  .model-status-grid,
  .candidate-columns,
  .path-row,
  .kernel-learning-steps,
  .settings-grid {
    grid-template-columns: 1fr;
  }

  .setting-row,
  .ping-line,
  .actions-row {
    align-items: stretch;
    flex-direction: column;
  }
}

.settings-toc {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-bottom: 14px;
}

.settings-toc a {
  padding: 5px 10px;
  border: 1px solid var(--border);
  border-radius: 999px;
  color: var(--text-muted);
  font-size: 12px;
  text-decoration: none;
}

.settings-toc a:hover {
  color: var(--text-primary);
  border-color: rgba(122, 162, 247, 0.6);
}
</style>
