<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { useAppStore, type WorkspaceBookmark } from '@/stores/app'
import AssistantToolsPanel from '@/components/settings/AssistantToolsPanel.vue'
import PanelIntro from '@/components/common/PanelIntro.vue'

const store = useAppStore()
const { locale } = useI18n()

const runtimeRows = computed(() => [
  { label: 'Backend', value: store.isConnected ? 'connecté' : 'déconnecté' },
  { label: 'Processus', value: store.isAttached ? store.processName : 'aucun' },
  { label: 'Version', value: store.version },
  { label: 'Workflow', value: store.workflowStatus },
])

const debugEvents = computed(() => [...store.smartSearchDebugEvents].reverse())
const learnedAutoProfile = computed(() => store.autoResolveReport?.learnedProfile ?? {})
const strategyWins = computed(() => {
  const wins = learnedAutoProfile.value.strategyWins
  return wins && typeof wins === 'object' ? wins as Record<string, unknown> : {}
})
const valueTypes = ['Int8', 'UInt8', 'Int16', 'UInt16', 'Int32', 'UInt32', 'Int64', 'UInt64', 'Float32', 'Float64']
const performanceModes = ['Auto', 'Eco', 'Normal', 'Performance', 'Max']
const unknownSnapshotPresets = [-1, 128, 512, 1024, 2048, 4096, 8192]
const unknownDepthLabel = (mb: number) => (mb === -1 ? 'Auto' : `${mb} Mo`)
const workspaceExportText = ref('')
const workspaceExportStatus = ref('')
const auditExportText = ref('')
const auditExportStatus = ref('')
const workspaceImportText = ref('')
const workspaceImportPreview = ref<Record<string, unknown> | null>(null)
const workspaceImportStatus = ref('')
const selectedStructureTemplateId = ref<number | null>(null)
const workspaceProjectName = ref('')
const bookmarkLabel = ref('')
const bookmarkAddress = ref('')
const bookmarkType = ref('Int32')
const bookmarkValue = ref('')
const bookmarkNote = ref('')
const kernelReadAddress = ref('')
const kernelReadSize = ref(16)
const kernelWriteAddress = ref('')
const kernelWriteBytes = ref('')
const kernelCapabilityLabel = computed(() => {
  const status = store.kernelDriverStatus
  if (!status) return 'Non testé'
  if (status.capabilities.processMemoryAccess) return 'Lecture/écriture prêtes'
  if (status.status === 'connected') return 'Probe seul'
  return 'Indisponible'
})
const kernelLearningSteps = [
  { label: '1. Vérifier', detail: 'Tester le driver et confirmer Accès mémoire kernel.' },
  { label: '2. Lire', detail: 'Relire 4/8 octets avant d’écrire pour prouver la bonne adresse.' },
  { label: '3. Écrire', detail: 'Écrire une seule adresse, puis relire immédiatement.' },
  { label: '4. Interpréter', detail: 'Si ça revient, chercher la source avec Écrit par.' },
]
const selectedStructureTemplate = computed(() =>
  store.structureTemplates.find((template) => template.id === selectedStructureTemplateId.value) ?? null,
)
const visibleModelCandidates = computed(() =>
  (store.aiModelStatus?.modelCandidates ?? []).slice(0, 6),
)
const visibleExecutableCandidates = computed(() =>
  (store.aiModelStatus?.executableCandidates ?? []).slice(0, 6),
)
const visibleEmbeddedAgents = computed(() =>
  (store.aiModelStatus?.embeddedAgents ?? []).slice(0, 8).map((agent) => ({
    id: String(agent.id ?? ''),
    displayName: String(agent.displayName ?? agent.id ?? 'Agent IA'),
    role: String(agent.role ?? 'agent'),
    modelPath: String(agent.modelPath ?? ''),
    modelFound: agent.modelFound === true,
    valid: agent.valid !== false,
  })),
)

function formatBytes(value: number | undefined) {
  const bytes = value ?? 0
  if (bytes >= 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} Go`
  if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} Mo`
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} Ko`
  return `${bytes} o`
}

function eventSummary(event: Record<string, unknown>) {
  const parts = [
    event.intent ? `intent=${String(event.intent)}` : '',
    event.tool ? `tool=${String(event.tool)}` : '',
    event.workflowStatus ? `workflow=${String(event.workflowStatus)}` : '',
    event.actionStatus ? `action=${String(event.actionStatus)}` : '',
    event.candidateCount !== undefined ? `candidats=${String(event.candidateCount)}` : '',
    event.targetValue ? `cible=${String(event.targetValue)}` : '',
  ].filter(Boolean)
  return parts.join('  ')
}

async function refreshAll() {
  await store.doPing()
  await store.loadSettings()
  await store.refreshKernelDriverStatus()
  await store.refreshAutomationPipeStatus()
  await store.refreshDiagnostics()
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

function showWorkspaceExport() {
  workspaceExportText.value = store.exportWorkspaceJson()
  workspaceExportStatus.value = ''
}

function showWorkspaceMarkdownExport() {
  workspaceExportText.value = store.exportWorkspaceMarkdown()
  workspaceExportStatus.value = ''
}

function showAuditJsonExport() {
  auditExportText.value = store.exportActionLogJson()
  auditExportStatus.value = ''
}

function showAuditMarkdownExport() {
  auditExportText.value = store.exportActionLogMarkdown()
  auditExportStatus.value = ''
}

async function copyWorkspaceExport() {
  if (!workspaceExportText.value) return
  await navigator.clipboard?.writeText(workspaceExportText.value)
  workspaceExportStatus.value = 'Export copié.'
}

async function copyAuditExport() {
  if (!auditExportText.value) return
  await navigator.clipboard?.writeText(auditExportText.value)
  auditExportStatus.value = 'Audit copié.'
}

function previewWorkspaceImport() {
  workspaceImportPreview.value = store.previewWorkspaceImport(workspaceImportText.value)
  workspaceImportStatus.value = workspaceImportPreview.value.success === true ? 'Aperçu prêt.' : String(workspaceImportPreview.value.error ?? 'Import invalide.')
}

function importWorkspace() {
  const result = store.importWorkspaceJson(workspaceImportText.value)
  workspaceImportPreview.value = result
  workspaceImportStatus.value = result.success === true ? 'Workspace importé.' : String(result.error ?? 'Import refusé.')
  if (result.success === true) workspaceImportText.value = ''
}

function saveWorkspaceProject() {
  const project = store.saveCurrentWorkspaceProject(workspaceProjectName.value)
  if (project) workspaceProjectName.value = ''
}

function addManualBookmark() {
  const bookmark = store.addWorkspaceBookmark({
    kind: bookmarkAddress.value.trim() ? 'address' : 'note',
    label: bookmarkLabel.value || bookmarkAddress.value || 'Note workspace',
    address: bookmarkAddress.value,
    type: bookmarkAddress.value.trim() ? bookmarkType.value : undefined,
    value: bookmarkValue.value,
    note: bookmarkNote.value,
  })
  if (bookmark) {
    bookmarkLabel.value = ''
    bookmarkAddress.value = ''
    bookmarkValue.value = ''
    bookmarkNote.value = ''
  }
}

function bookmarkToWrite(bookmark: WorkspaceBookmark) {
  store.useWorkspaceBookmarkAsWriteTarget(bookmark.id)
}

function bookmarkToTrainer(bookmark: WorkspaceBookmark, action: 'write' | 'freeze_polling') {
  store.createTrainerFeatureFromBookmark(bookmark.id, action)
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
        Rafraîchir
      </button>
    </div>

    <PanelIntro
      what="Les paramètres de KillEngine : langue, comportements par défaut, outils de diagnostic."
      purpose="Ajuster l'app à tes préférences et vérifier l'état des composants (helpers, connexions) sans passer par un fichier de config."
      how="Modifie un réglage dans la section correspondante ; les changements s'appliquent immédiatement."
    />

    <section class="panel">
      <div class="panel-title">
        <h2>Interface</h2>
      </div>
      <div class="setting-row">
        <div>
          <strong>Langue</strong>
          <span>{{ store.appLanguage === 'fr' ? 'Français' : 'English' }}</span>
        </div>
        <div class="segmented">
          <button :class="{ active: store.appLanguage === 'fr' }" @click="store.appLanguage = 'fr'">FR</button>
          <button :class="{ active: store.appLanguage === 'en' }" @click="store.appLanguage = 'en'">EN</button>
        </div>
      </div>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>Scan</h2>
      </div>
      <div class="settings-grid">
        <label>
          <span>Type par défaut</span>
          <select v-model="store.settingDefaultValueType" class="input select">
            <option v-for="type in valueTypes" :key="type">{{ type }}</option>
          </select>
        </label>
        <label>
          <span>Résultats maximum</span>
          <input v-model.number="store.settingScanMaxResults" class="input" type="number" min="1000" max="10000000" step="1000" />
        </label>
        <label>
          <span>Mode performance</span>
          <select v-model="store.settingPerformanceMode" class="input select">
            <option v-for="mode in performanceModes" :key="mode">{{ mode }}</option>
          </select>
        </label>
        <label>
          <span>Chunk mémoire (Mo, 0 = Auto)</span>
          <input v-model.number="store.settingScanChunkSizeMb" class="input" type="number" min="0" max="64" step="1" />
        </label>
        <label>
          <span>Threads max (0 = Auto)</span>
          <input v-model.number="store.settingScanMaxWorkerThreads" class="input" type="number" min="0" max="128" step="1" />
        </label>
        <label>
          <span>Mémoire scan en vol (Mo, 0 = Auto)</span>
          <input v-model.number="store.settingScanMaxInFlightMb" class="input" type="number" min="0" max="32768" step="64" />
        </label>
        <label>
          <span>Seuil fichier candidats</span>
          <input v-model.number="store.settingCandidateFileBackedThreshold" class="input" type="number" min="1" max="5000000" step="1000" />
        </label>
        <label>
          <span>Snapshot unknown max</span>
          <select v-model.number="store.settingUnknownSnapshotMaxMb" class="input select">
            <option v-for="mb in unknownSnapshotPresets" :key="mb" :value="mb">{{ unknownDepthLabel(mb) }}</option>
          </select>
        </label>
        <label class="toggle-row">
          <input v-model="store.settingFastScan" type="checkbox" />
          <span>Fast scan par défaut</span>
        </label>
      </div>
      <p class="hint">
        Gros process : mets le seuil fichier à 1 pour purger la RAM plus tôt. Les fichiers temporaires sont supprimés au nouveau scan ou à la fermeture.
      </p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>Stockage temporaire</h2>
        <div class="panel-actions">
          <button class="btn btn-secondary compact" :disabled="store.scanBusy" @click="store.refreshTemporaryStorageStatus()">
            Actualiser
          </button>
          <button class="btn btn-primary compact" :disabled="store.scanBusy" @click="store.clearTemporaryStorage()">
            Nettoyer maintenant
          </button>
        </div>
      </div>
      <div class="runtime-grid">
        <div class="runtime-cell">
          <span>Total</span>
          <strong>{{ formatBytes(store.temporaryStorageStatus?.totalBytes) }}</strong>
        </div>
        <div class="runtime-cell">
          <span>Actif</span>
          <strong>{{ formatBytes(store.temporaryStorageStatus?.activeBytes) }}</strong>
        </div>
        <div class="runtime-cell">
          <span>Orphelins</span>
          <strong>{{ store.temporaryStorageStatus?.orphanFileCount ?? 0 }} · {{ formatBytes(store.temporaryStorageStatus?.orphanBytes) }}</strong>
        </div>
        <div class="runtime-cell">
          <span>Fichiers actifs</span>
          <strong>{{ store.temporaryStorageStatus?.activeFileCount ?? 0 }}</strong>
        </div>
      </div>
      <div class="path-row">
        <span>Dossier temp</span>
        <code>{{ store.temporaryStorageStatus?.tempPath || '-' }}</code>
      </div>
      <div class="path-row">
        <span>Candidats</span>
        <code>{{ formatBytes(store.temporaryStorageStatus?.candidateBytes) }} · {{ store.temporaryStorageStatus?.candidateFileBacked ? 'fichier' : 'RAM' }}</code>
      </div>
      <div class="path-row">
        <span>Undo / snapshot</span>
        <code>{{ formatBytes(store.temporaryStorageStatus?.undoBytes) }} / {{ formatBytes(store.temporaryStorageStatus?.snapshotBytes) }}</code>
      </div>
      <p v-if="store.temporaryStorageCleanupResult" class="status-line">
        {{ store.temporaryStorageCleanupResult.message || (store.temporaryStorageCleanupResult.success ? 'Nettoyage terminé.' : store.temporaryStorageCleanupResult.error) }}
      </p>
      <p v-if="store.temporaryStorageError" class="error">{{ store.temporaryStorageError }}</p>
      <p class="hint">
        Le nettoyage ferme le contexte de scan courant, vide l'undo et le snapshot unknown, puis supprime les fichiers `killengine_candidates_*.kecand` et `killengine_snapshot_*.kesnap` restants.
      </p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>IA locale</h2>
        <div class="panel-actions">
          <span class="status-pill" :class="store.aiModelStatus?.ready ? 'ok' : 'warn'">
            {{ store.aiModelStatus?.ready ? 'IA prête' : 'IA indisponible' }}
          </span>
          <button class="btn btn-secondary compact" :disabled="store.aiModelStatusLoading" @click="store.refreshAiModelStatus()">
            {{ store.aiModelStatusLoading ? 'Vérif...' : 'Vérifier' }}
          </button>
        </div>
      </div>
      <div class="settings-grid">
        <label class="wide">
          <span>Chemin personnalisé GGUF</span>
          <div class="model-path-row">
            <input v-model="store.settingModelPath" class="input" placeholder="Avancé : vide = modèle embarqué dans model\\qwen\\*.gguf" />
            <button class="btn btn-secondary compact" type="button" @click="store.browseForModel()">Parcourir…</button>
          </div>
        </label>
        <label>
          <span>Threads modèle</span>
          <input v-model.number="store.settingModelThreads" class="input" type="number" min="1" max="32" step="1" />
        </label>
      </div>
      <div class="model-status-grid">
        <div class="runtime-cell">
          <span>Backend</span>
          <strong>{{ store.aiModelStatus?.backend || '-' }}</strong>
        </div>
        <div class="runtime-cell">
          <span>Produit</span>
          <strong>IA embarquée requise</strong>
        </div>
        <div class="runtime-cell">
          <span>Agents IA</span>
          <strong>{{ store.aiModelStatus?.embeddedAgentCount ?? visibleEmbeddedAgents.length }}</strong>
        </div>
        <div class="runtime-cell">
          <span>Modèle</span>
          <strong>{{ store.aiModelStatus?.modelFound ? 'trouvé' : 'absent' }}</strong>
        </div>
        <div class="runtime-cell">
          <span>llama-cli</span>
          <strong>{{ store.aiModelStatus?.executableFound ? 'trouvé' : 'absent' }}</strong>
        </div>
        <div class="runtime-cell">
          <span>Threads</span>
          <strong>{{ store.aiModelStatus?.threads ?? store.settingModelThreads }}</strong>
        </div>
      </div>
      <div class="path-row">
        <span>Modèle détecté</span>
        <code>{{ store.aiModelStatus?.modelPath || store.settingModelPath || '-' }}</code>
      </div>
      <div class="path-row">
        <span>Runtime actif</span>
        <code>{{ store.aiModelStatus?.executablePath || '-' }}</code>
      </div>
      <div v-if="visibleEmbeddedAgents.length" class="embedded-agent-list">
        <strong>Agents embarqués</strong>
        <div v-for="agent in visibleEmbeddedAgents" :key="agent.id" class="candidate-path">
          <span :class="agent.modelFound && agent.valid ? 'ok-text' : 'dim-text'">{{ agent.modelFound && agent.valid ? 'OK' : '--' }}</span>
          <code>{{ agent.displayName }} · {{ agent.role }} · {{ agent.modelPath || '-' }}</code>
        </div>
      </div>
      <p v-if="store.aiModelStatus?.message" class="status-line">{{ store.aiModelStatus.message }}</p>
      <p v-if="store.aiModelStatus?.modelError && !store.aiModelStatus?.modelFound" class="warning">{{ store.aiModelStatus.modelError }}</p>
      <p v-if="store.aiModelStatusError" class="error">{{ store.aiModelStatusError }}</p>
      <p class="hint">
        Le produit cherche automatiquement les IA embarquées dans <code>model\&lt;nom_ia&gt;\*.gguf</code> à côté de KillEngine.exe. Le chemin personnalisé sert seulement d'override avancé.
      </p>
      <details class="model-candidates">
        <summary>Chemins inspectés</summary>
        <div class="candidate-columns">
          <div>
            <strong>Modèles GGUF</strong>
            <div v-for="candidate in visibleModelCandidates" :key="String(candidate.path)" class="candidate-path">
              <span :class="candidate.exists ? 'ok-text' : 'dim-text'">{{ candidate.exists ? 'OK' : '--' }}</span>
              <code>{{ candidate.path }}</code>
            </div>
          </div>
          <div>
            <strong>llama-cli</strong>
            <div v-for="candidate in visibleExecutableCandidates" :key="String(candidate.path)" class="candidate-path">
              <span :class="candidate.exists ? 'ok-text' : 'dim-text'">{{ candidate.exists ? 'OK' : '--' }}</span>
              <code>{{ candidate.path }}</code>
            </div>
          </div>
        </div>
      </details>
      <p class="hint">
        Safe autorise seulement les actions sans danger et les écritures confirmées. Expert débloque debugger/patch confirmés. Trainer prépare les actions avancées type hook/injection.
      </p>
    </section>

    <AssistantToolsPanel />

    <section class="panel">
      <div class="panel-title">
        <h2>Workspace IA / Trainer</h2>
      </div>
      <div class="runtime-grid">
        <div class="runtime-cell">
          <span>Investigation active</span>
          <strong>{{ store.activeInvestigation ? store.activeInvestigation.steps.length : 0 }} étape(s)</strong>
        </div>
        <div class="runtime-cell">
          <span>Archives Investigation</span>
          <strong>{{ store.investigationArchive.length }}</strong>
        </div>
        <div class="runtime-cell">
          <span>Features Trainer</span>
          <strong>{{ store.trainerFeatures.length }}</strong>
        </div>
        <div class="runtime-cell">
          <span>Templates Structure</span>
          <strong>{{ store.structureTemplates.length }}</strong>
        </div>
        <div class="runtime-cell">
          <span>Bookmarks</span>
          <strong>{{ store.workspaceBookmarks.length }}</strong>
        </div>
        <div class="runtime-cell">
          <span>Projets locaux</span>
          <strong>{{ store.workspaceProjects.length }}</strong>
        </div>
        <div class="runtime-cell">
          <span>Audit actions</span>
          <strong>{{ store.actionLog.length }}</strong>
        </div>
        <div class="runtime-cell">
          <span>Processus mémoire Auto</span>
          <strong>{{ store.processName || 'global' }}</strong>
        </div>
        <div class="runtime-cell">
          <span>Dernière stratégie</span>
          <strong>{{ learnedAutoProfile.lastSuccessfulAuditEvent || '-' }}</strong>
        </div>
        <div class="runtime-cell">
          <span>Dernière adresse</span>
          <strong>{{ learnedAutoProfile.lastSuccessfulAddress ? `0x${learnedAutoProfile.lastSuccessfulAddress}` : '-' }}</strong>
        </div>
        <div class="runtime-cell">
          <span>Dernier type</span>
          <strong>{{ learnedAutoProfile.lastSuccessfulValueType || '-' }}</strong>
        </div>
        <div class="runtime-cell">
          <span>Stratégies gagnantes</span>
          <strong>{{ Object.keys(strategyWins).length }}</strong>
        </div>
      </div>
      <div class="path-row">
        <span>Pattern AOB appris</span>
        <code>{{ learnedAutoProfile.lastSuccessfulAobPattern || '-' }}</code>
      </div>
      <div v-if="store.rememberedPatterns.length" class="remembered-patterns">
        <div class="source-list-title">
          <strong>Motifs mémorisés pour ce jeu</strong>
          <span>{{ store.rememberedPatterns.length }} entrée(s)</span>
        </div>
        <div v-for="pattern in store.rememberedPatterns" :key="`${pattern.module}:${pattern.moduleOffset}`" class="remembered-pattern-row">
          <div class="remembered-pattern-label">
            <strong v-if="pattern.queryLabel" :title="String(pattern.queryLabel)">{{ pattern.queryLabel }}</strong>
            <code>{{ pattern.module }}+0x{{ pattern.moduleOffset }}</code>
          </div>
          <span>{{ pattern.valueType || '-' }}</span>
          <span>{{ pattern.confirmCount }}×</span>
          <span :class="pattern.resolved ? 'hint' : 'error'">
            {{ pattern.resolved ? `0x${pattern.liveAddress}` : 'module absent (pas attaché ou jeu différent)' }}
          </span>
          <button
            v-if="pattern.resolved"
            class="btn btn-secondary compact"
            type="button"
            @click="store.previewRememberedPattern(pattern)"
          >
            Prévisualiser
          </button>
        </div>
      </div>
      <div v-if="store.writeHistorySequence.length" class="remembered-patterns">
        <div class="source-list-title">
          <strong>Historique d'écritures (replay)</strong>
          <span>{{ store.writeHistorySequence.length }} entrée(s)</span>
        </div>
        <div v-for="(entry, index) in store.writeHistorySequence" :key="`${entry.module}:${entry.moduleOffset}:${entry.writtenAt}:${index}`" class="remembered-pattern-row">
          <code>{{ entry.module }}+0x{{ entry.moduleOffset }}</code>
          <span>{{ entry.valueType || '-' }} = {{ entry.value }}</span>
          <span :class="entry.resolved ? 'hint' : 'error'">
            {{ entry.resolved ? `0x${entry.liveAddress}` : 'module absent (pas attaché ou jeu différent)' }}
          </span>
        </div>
        <div class="panel-actions">
          <button class="btn btn-primary compact" type="button" @click="store.replayWriteHistorySequence()">
            Rejouer la séquence
          </button>
          <button class="btn btn-secondary compact" type="button" @click="store.clearWriteHistorySequence()">
            Vider l'historique
          </button>
        </div>
      </div>
      <div class="panel-actions workspace-actions">
        <button class="btn btn-secondary compact" @click="showWorkspaceExport()">Exporter workspace JSON</button>
        <button class="btn btn-secondary compact" @click="showWorkspaceMarkdownExport()">Exporter workspace MD</button>
        <button class="btn btn-secondary compact" @click="showAuditJsonExport()">Exporter audit JSON</button>
        <button class="btn btn-secondary compact" @click="showAuditMarkdownExport()">Exporter audit MD</button>
        <button class="btn btn-secondary compact" @click="store.clearInvestigation()">Vider investigation active</button>
        <button class="btn btn-secondary compact" @click="store.clearInvestigationArchive()">Vider archives</button>
        <button class="btn btn-secondary compact" @click="store.clearTrainerFeatures()">Vider trainer local</button>
        <button class="btn btn-secondary compact" @click="store.clearStructureTemplates()">Vider templates structure</button>
        <button class="btn btn-secondary compact" @click="store.clearWorkspaceBookmarks()">Vider bookmarks</button>
        <button class="btn btn-secondary compact" @click="store.clearWorkspaceProjects()">Vider projets</button>
        <button class="btn btn-secondary compact" @click="store.clearActionLog()">Vider audit</button>
        <button class="btn btn-secondary compact" @click="store.clearAutoResolveMemory(false)">Vider mémoire Auto processus</button>
        <button class="btn btn-secondary compact danger-action" @click="store.clearAutoResolveMemory(true)">Vider mémoire Auto globale</button>
      </div>
      <p class="hint">
        Ces actions suppriment les mémoires locales de pilotage IA et Trainer. Les profils sauvegardés via ProfileStore ne sont pas supprimés ici.
      </p>
      <div class="project-panel">
        <div class="panel-title">
          <h3>Projets Workspace</h3>
          <div class="panel-actions">
            <input v-model="workspaceProjectName" class="input project-name-input" placeholder="Nom du projet" />
            <button class="btn btn-secondary compact" @click="saveWorkspaceProject()">Sauver projet</button>
          </div>
        </div>
        <div v-if="store.workspaceProjects.length === 0" class="empty-line">Aucun projet local.</div>
        <div v-for="project in store.workspaceProjects.slice(0, 12)" :key="project.id" class="project-row">
          <div>
            <strong>{{ project.name }}</strong>
            <span>{{ project.processName || '-' }} · {{ project.trainerFeatureCount }} feature(s) · {{ project.structureTemplateCount }} template(s) · {{ project.bookmarkCount }} bookmark(s) · {{ project.auditCount || 0 }} audit(s)</span>
          </div>
          <div class="panel-actions">
            <button class="btn btn-secondary compact" @click="store.loadWorkspaceProject(project.id)">Charger</button>
            <button class="btn btn-secondary compact danger-action" @click="store.deleteWorkspaceProject(project.id)">Supprimer</button>
          </div>
        </div>
      </div>
      <div v-if="store.workspaceBookmarks.length > 0" class="bookmark-list">
        <div class="panel-title">
          <h3>Bookmarks / Notes</h3>
          <span>{{ store.workspaceBookmarks.length }}</span>
        </div>
        <div class="bookmark-create">
          <input v-model="bookmarkLabel" class="input" placeholder="Label" />
          <input v-model="bookmarkAddress" class="input" placeholder="Adresse hex optionnelle" />
          <select v-model="bookmarkType" class="select">
            <option v-for="type in valueTypes" :key="type" :value="type">{{ type }}</option>
          </select>
          <input v-model="bookmarkValue" class="input" placeholder="Valeur optionnelle" />
          <input v-model="bookmarkNote" class="input bookmark-note-input" placeholder="Note" />
          <button class="btn btn-secondary compact" @click="addManualBookmark()">Ajouter</button>
        </div>
        <div v-for="bookmark in store.workspaceBookmarks.slice(0, 20)" :key="bookmark.id" class="bookmark-row">
          <div>
            <strong>{{ bookmark.label }}</strong>
            <span>{{ bookmark.kind }} · {{ bookmark.address ? `0x${bookmark.address}` : '-' }} · {{ bookmark.type || '-' }} · {{ bookmark.note || '-' }}</span>
          </div>
          <div class="panel-actions">
            <button class="btn btn-secondary compact" :disabled="!bookmark.address" @click="bookmarkToWrite(bookmark)">Write</button>
            <button class="btn btn-secondary compact" :disabled="!bookmark.address" @click="bookmarkToTrainer(bookmark, 'write')">Trainer</button>
            <button class="btn btn-secondary compact" :disabled="!bookmark.address" @click="bookmarkToTrainer(bookmark, 'freeze_polling')">Freeze</button>
            <button class="btn btn-secondary compact danger-action" @click="store.deleteWorkspaceBookmark(bookmark.id)">Supprimer</button>
          </div>
        </div>
      </div>
      <div v-else class="bookmark-list">
        <div class="panel-title">
          <h3>Bookmarks / Notes</h3>
          <span>0</span>
        </div>
        <div class="bookmark-create">
          <input v-model="bookmarkLabel" class="input" placeholder="Label" />
          <input v-model="bookmarkAddress" class="input" placeholder="Adresse hex optionnelle" />
          <select v-model="bookmarkType" class="select">
            <option v-for="type in valueTypes" :key="type" :value="type">{{ type }}</option>
          </select>
          <input v-model="bookmarkValue" class="input" placeholder="Valeur optionnelle" />
          <input v-model="bookmarkNote" class="input bookmark-note-input" placeholder="Note" />
          <button class="btn btn-secondary compact" @click="addManualBookmark()">Ajouter</button>
        </div>
      </div>
      <div class="workspace-import">
        <div class="panel-title">
          <h3>Importer workspace JSON</h3>
          <div class="panel-actions">
            <button class="btn btn-secondary compact" :disabled="!workspaceImportText.trim()" @click="previewWorkspaceImport()">Aperçu</button>
            <button class="btn btn-secondary compact danger-action" :disabled="workspaceImportPreview?.success !== true" @click="importWorkspace()">Importer</button>
          </div>
        </div>
        <textarea v-model="workspaceImportText" class="workspace-import-input" placeholder="Coller un export Workspace JSON ici" />
        <p v-if="workspaceImportStatus" class="status-line">{{ workspaceImportStatus }}</p>
        <div v-if="workspaceImportPreview?.success === true" class="import-preview">
          <span>Investigation active: {{ workspaceImportPreview.activeInvestigation }}</span>
          <span>Archives: {{ workspaceImportPreview.archiveCount }}</span>
          <span>Features: {{ workspaceImportPreview.trainerFeatureCount }}</span>
          <span>Templates: {{ workspaceImportPreview.structureTemplateCount }}</span>
          <span>Bookmarks: {{ workspaceImportPreview.bookmarkCount }}</span>
          <span>Audit: {{ workspaceImportPreview.auditCount || 0 }}</span>
          <span>Preset: {{ workspaceImportPreview.lastPresetId || '-' }}</span>
          <span>Settings: {{ workspaceImportPreview.hasSettings ? 'oui' : 'non' }}</span>
        </div>
      </div>
      <div v-if="store.structureTemplates.length > 0" class="template-list">
        <div class="panel-title">
          <h3>Templates Structure</h3>
          <span>{{ store.structureTemplates.length }}</span>
        </div>
        <div v-for="template in store.structureTemplates.slice(0, 12)" :key="template.id" class="template-row">
          <div>
            <strong>{{ template.name }}</strong>
            <span>0x{{ template.baseAddress }} · {{ template.fieldCount }} champ(s) · {{ template.processName || '-' }}</span>
          </div>
          <div class="panel-actions">
            <button class="btn btn-secondary compact" @click="selectedStructureTemplateId = template.id">Détails</button>
            <button class="btn btn-secondary compact danger-action" @click="store.deleteStructureTemplate(template.id)">Supprimer</button>
          </div>
        </div>
        <div v-if="selectedStructureTemplate" class="template-detail">
          <div class="panel-title">
            <h3>{{ selectedStructureTemplate.name }}</h3>
            <button class="btn btn-secondary compact" @click="selectedStructureTemplateId = null">Fermer</button>
          </div>
          <div
            v-for="field in selectedStructureTemplate.fields.slice(0, 80)"
            :key="`${selectedStructureTemplate.id}:${field.offset}:${field.type}`"
            class="template-field-row"
          >
            <code>{{ field.offset >= 0 ? '+' : '' }}{{ field.offset }}</code>
            <strong>{{ field.type }}</strong>
            <span>{{ field.label || '-' }}</span>
            <span>{{ field.sampleValue || '-' }}</span>
            <span>{{ field.note || '-' }}</span>
          </div>
        </div>
      </div>
      <div v-if="workspaceExportText" class="workspace-export">
        <div class="panel-title">
          <h3>Export workspace</h3>
          <div class="panel-actions">
            <button class="btn btn-secondary compact" @click="copyWorkspaceExport()">Copier</button>
            <button class="btn btn-secondary compact" @click="workspaceExportText = ''">Fermer</button>
          </div>
        </div>
        <p v-if="workspaceExportStatus" class="status-line">{{ workspaceExportStatus }}</p>
        <pre>{{ workspaceExportText }}</pre>
      </div>
      <div class="audit-panel">
        <div class="panel-title">
          <h3>Audit actions</h3>
          <span>{{ store.actionLog.length }}</span>
        </div>
        <div v-if="store.actionLog.length === 0" class="empty-line">Aucune action auditée.</div>
        <div v-for="entry in store.actionLog.slice(0, 20)" :key="entry.id" class="audit-row">
          <div>
            <strong>{{ entry.title }}</strong>
            <span>{{ entry.time }} · {{ entry.kind }} · {{ entry.status }}{{ entry.detail ? ` · ${entry.detail}` : '' }}</span>
          </div>
        </div>
      </div>
      <div v-if="auditExportText" class="workspace-export">
        <div class="panel-title">
          <h3>Export audit</h3>
          <div class="panel-actions">
            <button class="btn btn-secondary compact" @click="copyAuditExport()">Copier</button>
            <button class="btn btn-secondary compact" @click="auditExportText = ''">Fermer</button>
          </div>
        </div>
        <p v-if="auditExportStatus" class="status-line">{{ auditExportStatus }}</p>
        <pre>{{ auditExportText }}</pre>
      </div>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>État</h2>
      </div>
      <div class="runtime-grid">
        <div v-for="row in runtimeRows" :key="row.label" class="runtime-cell">
          <span>{{ row.label }}</span>
          <strong>{{ row.value }}</strong>
        </div>
      </div>
      <div class="ping-line">
        <button class="btn btn-secondary" @click="store.doPing()">
          {{ $t('actions.ping') }}
        </button>
        <code>{{ store.pingResult || '-' }}</code>
      </div>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>Diagnostic</h2>
        <div class="panel-actions">
          <button class="btn btn-secondary compact" @click="store.refreshLogTail()">
            Logs
          </button>
          <button class="btn btn-secondary compact" @click="store.exportDiagnostics()">
            Exporter
          </button>
        </div>
      </div>
      <div class="path-row">
        <span>Log</span>
        <code>{{ store.logFilePath || '-' }}</code>
      </div>
      <div class="path-row">
        <span>Smart Search JSON</span>
        <code>{{ store.smartSearchDebugFilePath || '-' }}</code>
      </div>
      <div class="path-row">
        <span>Scan telemetry JSON</span>
        <code>{{ store.scanTelemetryFilePath || '-' }}</code>
      </div>
      <div class="setting-row inline-setting">
        <div>
          <strong>Debug Smart Search</strong>
          <span>{{ store.settingSmartSearchDebugEnabled ? 'activé' : 'désactivé' }}</span>
        </div>
        <label class="toggle-row">
          <input v-model="store.settingSmartSearchDebugEnabled" type="checkbox" />
          <span>Écrire le JSONL</span>
        </label>
      </div>
      <div class="setting-row inline-setting">
        <div>
          <strong>Événements affichés</strong>
          <span>{{ store.settingSmartSearchDebugMaxEvents }}</span>
        </div>
        <input v-model.number="store.settingSmartSearchDebugMaxEvents" class="input short-input" type="number" min="5" max="200" step="5" />
      </div>
      <p v-if="store.diagnosticExportPath" class="status-line">
        Diagnostic exporté : <code>{{ store.diagnosticExportPath }}</code>
      </p>
      <p v-if="store.diagnosticExportError" class="error">{{ store.diagnosticExportError }}</p>
      <p v-if="store.diagnosticOpenFolderError" class="warning">{{ store.diagnosticOpenFolderError }}</p>
      <p v-if="store.logError" class="error">{{ store.logError }}</p>
      <p v-if="store.smartSearchDebugError" class="error">{{ store.smartSearchDebugError }}</p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>Compatibilité antivirus</h2>
      </div>
      <p class="hint">
        Certains antivirus/EDR (ex. Microsoft Defender for Endpoint) bloquent parfois l'injection utilisée par
        le breakpoint in-process ou le freeze avancé, en confondant cet usage légitime de débogage avec une
        technique d'injection malveillante. Le bouton ci-dessous ouvre une invite d'élévation Windows (UAC)
        pour ajouter une exclusion — rien ne se passe sans ta confirmation explicite dans cette invite.
      </p>
      <div class="panel-actions">
        <button
          class="btn btn-secondary compact"
          :disabled="store.defenderExclusionBusy"
          @click="store.requestWindowsDefenderExclusion()"
        >
          {{ store.defenderExclusionBusy ? 'En cours…' : 'Ajouter une exclusion Windows Defender' }}
        </button>
      </div>
      <p v-if="store.defenderExclusionResult?.success" class="status-line">
        Exclusion ajoutée avec succès.
      </p>
      <p v-else-if="store.defenderExclusionResult?.cancelled" class="warning">
        Invite d'élévation refusée — aucune modification effectuée.
      </p>
      <p v-else-if="store.defenderExclusionResult?.error" class="error">
        {{ store.defenderExclusionResult.error }}
      </p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>Driver kernel</h2>
        <span>{{ store.kernelDriverStatus?.status ?? 'inconnu' }}</span>
      </div>
      <p class="hint">
        Statut du driver optionnel `KillEngineKernel.sys`. Si "Accès mémoire kernel" est actif ci-dessous, la lecture
        et l'écriture mémoire via le driver noyau sont disponibles (contourne les protections mémoire usermode
        normales — VirtualProtect/PAGE_GUARD — via un accès ring 0, pas juste un probe de santé).
      </p>
      <div class="kernel-learning">
        <div class="kernel-learning-head">
          <strong>Parcours kernel</strong>
          <span>{{ kernelCapabilityLabel }}</span>
        </div>
        <div class="kernel-learning-steps">
          <div v-for="step in kernelLearningSteps" :key="step.label" class="kernel-learning-step">
            <strong>{{ step.label }}</strong>
            <span>{{ step.detail }}</span>
          </div>
        </div>
      </div>
      <div class="panel-actions">
        <button
          class="btn btn-secondary compact"
          :disabled="store.kernelDriverStatusLoading"
          @click="store.refreshKernelDriverStatus()"
        >
          {{ store.kernelDriverStatusLoading ? 'Probe…' : 'Tester le driver' }}
        </button>
        <button
          class="btn btn-secondary compact"
          :disabled="store.kernelDriverStartLoading || store.kernelDriverStatusLoading"
          title="Démarre le service Windows KillEngineKernel s'il est déjà installé mais arrêté. Nécessite KillEngine lancé administrateur."
          @click="store.startKernelDriver()"
        >
          {{ store.kernelDriverStartLoading ? 'Démarrage…' : 'Relancer le driver' }}
        </button>
      </div>
      <div v-if="store.kernelDriverStatus" class="settings-grid compact-grid">
        <div>
          <strong>Device</strong>
          <span>{{ store.kernelDriverStatus.devicePath }}</span>
        </div>
        <div>
          <strong>Message</strong>
          <span>{{ store.kernelDriverStatus.message }}</span>
        </div>
        <div>
          <strong>Protocole</strong>
          <span>{{ store.kernelDriverStatus.capabilities.protocolVersion }}</span>
        </div>
        <div>
          <strong>Health probe</strong>
          <span>{{ store.kernelDriverStatus.capabilities.healthProbe ? 'oui' : 'non' }}</span>
        </div>
        <div>
          <strong>Accès mémoire kernel</strong>
          <span>{{ store.kernelDriverStatus.capabilities.processMemoryAccess ? 'oui' : 'non' }}</span>
        </div>
        <div>
          <strong>Instrumentation privilégiée</strong>
          <span>{{ store.kernelDriverStatus.capabilities.privilegedInstrumentation ? 'oui' : 'non' }}</span>
        </div>
      </div>
      <p v-if="store.kernelDriverStatusError" class="error">
        {{ store.kernelDriverStatusError }}
      </p>

      <template v-if="store.kernelDriverStatus?.capabilities.processMemoryAccess">
        <h3>Lecture mémoire (kernel)</h3>
        <p class="hint">
          Lit sur le processus attaché ({{ store.isAttached ? store.processName : 'aucun' }}) via
          <code>KeStackAttachProcess</code> côté driver — chemin distinct de la lecture usermode habituelle.
        </p>
        <div class="panel-actions">
          <input v-model="kernelReadAddress" class="input" placeholder="Adresse hex, ex: 7FF6ABCD1000" />
          <input v-model.number="kernelReadSize" class="input short-input" type="number" min="1" max="4096" step="1" />
          <button
            class="btn btn-secondary compact"
            :disabled="store.kernelMemoryReadBusy || !store.isAttached"
            @click="store.readMemoryKernel(kernelReadAddress, kernelReadSize)"
          >
            {{ store.kernelMemoryReadBusy ? 'Lecture…' : 'Lire (kernel)' }}
          </button>
        </div>
        <p v-if="store.kernelMemoryReadResult?.success" class="status-line">
          {{ store.kernelMemoryReadResult.bytesRead }} octet(s) : {{ store.kernelMemoryReadResult.hex }}
        </p>
        <p v-else-if="store.kernelMemoryReadResult?.error" class="error">
          {{ store.kernelMemoryReadResult.error }}
        </p>

        <h3>Écriture mémoire (kernel)</h3>
        <p class="hint">
          Écrit directement depuis le ring 0, sans passer par les protections mémoire usermode normales — action à
          risque équivalente à une injection, soumise à la même confirmation. Pour apprendre proprement : lis l'adresse,
          écris une valeur témoin, relis, puis vérifie l'effet dans la cible.
        </p>
        <div class="panel-actions">
          <input v-model="kernelWriteAddress" class="input" placeholder="Adresse hex, ex: 7FF6ABCD1000" />
          <input v-model="kernelWriteBytes" class="input" placeholder="Octets hex, ex: 90 90 90" />
          <button
            class="btn btn-secondary compact"
            :disabled="store.kernelMemoryWriteBusy || !store.isAttached"
            @click="store.writeMemoryKernel(kernelWriteAddress, kernelWriteBytes)"
          >
            {{ store.kernelMemoryWriteBusy ? 'Écriture…' : 'Écrire (kernel)' }}
          </button>
        </div>
        <p v-if="store.kernelMemoryWriteResult?.success" class="status-line">
          {{ store.kernelMemoryWriteResult.bytesWritten }} octet(s) écrits.
        </p>
        <p v-else-if="store.kernelMemoryWriteResult?.error" class="error">
          {{ store.kernelMemoryWriteResult.error }}
        </p>
      </template>
      <p v-else class="hint">
        Lecture/écriture mémoire via le driver noyau indisponibles : le driver doit être connecté avec la capacité
        "Accès mémoire kernel" active (voir ci-dessus, "Tester le driver").
      </p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>Mode Automation (avancé)</h2>
        <span>{{ store.automationPipeStatus?.running ? 'Actif' : 'Inactif' }}</span>
      </div>
      <p class="hint">
        Ouvre le pipe d'automatisation local de KillEngine (utilisé par le scripting Lua <code>ke.call(...)</code> et
        par tout outil/agent externe sur cette machine) sans avoir à lancer KillEngine avec la variable
        d'environnement <code>KILLENGINE_AUTOMATION_PIPE=1</code>. Réservé aux utilisateurs avancés : une fois actif,
        les actions déclenchées via ce pipe s'exécutent immédiatement, sans confirmation par action — voir
        <code>docs/AUTOMATION_API.md</code> pour le protocole complet et un exemple de branchement d'un agent IA.
      </p>
      <div class="panel-actions">
        <button
          v-if="!store.automationPipeStatus?.running"
          class="btn btn-secondary compact"
          @click="store.enableAutomationMode()"
        >
          Activer le mode Automation
        </button>
        <button
          v-else
          class="btn btn-secondary compact"
          @click="store.disableAutomationMode()"
        >
          Désactiver le mode Automation
        </button>
        <button class="btn btn-secondary compact" @click="store.refreshAutomationPipeStatus()">
          Rafraîchir le statut
        </button>
      </div>
      <div v-if="store.automationPipeStatus" class="settings-grid compact-grid">
        <div>
          <strong>Pipe</strong>
          <span>{{ store.automationPipeStatus.pipeName }}</span>
        </div>
        <div>
          <strong>Appels reçus</strong>
          <span>{{ store.automationPipeStatus.callCount ?? 0 }}</span>
        </div>
        <div>
          <strong>Dernier appel</strong>
          <span>{{ store.automationPipeStatus.lastMethod || '—' }}</span>
        </div>
        <div>
          <strong>À</strong>
          <span>{{ store.automationPipeStatus.lastCallAt || '—' }}</span>
        </div>
      </div>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>Débogage CDP WebView2 (avancé)</h2>
        <span>{{ store.webView2CdpDebugFlagStatus?.enabled ? 'Actif' : 'Inactif' }}</span>
      </div>
      <p class="hint">
        Force <code>--remote-debugging-port=9333</code> sur <strong>tous les hôtes WebView2 du user Windows
        courant</strong> à leur prochain lancement — nécessaire pour que KillEngine inspecte l'état JavaScript d'une
        app WebView2/Electron/CEF non packagée (Store/UWP passe par une autre voie, voir le diagnostic ci-dessous).
        Portée large et persistante tant que non désactivé.
      </p>
      <div class="panel-actions">
        <button
          v-if="!store.webView2CdpDebugFlagStatus?.enabled"
          class="btn btn-secondary compact"
          :disabled="store.webView2CdpDebugFlagBusy"
          @click="store.enableWebView2CdpDebugFlag()"
        >
          Activer le débogage CDP
        </button>
        <button
          v-else
          class="btn btn-secondary compact"
          :disabled="store.webView2CdpDebugFlagBusy"
          @click="store.disableWebView2CdpDebugFlag()"
        >
          Désactiver le débogage CDP
        </button>
        <button
          class="btn btn-secondary compact"
          :disabled="store.webView2CdpDebugFlagBusy"
          @click="store.refreshWebView2CdpDebugFlagStatus()"
        >
          Rafraîchir le statut
        </button>
      </div>
      <p v-if="store.webView2CdpDebugFlagStatus?.value" class="status-line">
        Variable posée : <code>{{ store.webView2CdpDebugFlagStatus.value }}</code>
      </p>
      <p v-if="store.webView2CdpDebugFlagStatus?.error" class="error">
        {{ store.webView2CdpDebugFlagStatus.error }}
      </p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>Préparer l'inspection WebView2 (apps Store/UWP)</h2>
        <span>{{ store.webView2SystemPrepStatus?.capabilityInstalled ? 'Prêt' : 'À préparer' }}</span>
      </div>
      <p class="hint">
        Diagnostic pour la chaîne Windows Device Portal (nécessaire pour inspecter le CDP d'apps WebView2
        packagées Store/UWP, ex: apps du Microsoft Store) : Mode développeur Windows et capability optionnelle
        <code>Tools.DeveloperMode.Core</code>. Ne débloque PAS le port CDP direct de ces apps (restriction
        AppContainer séparée, toujours présente) — uniquement un prérequis pour le Portail d'appareil.
      </p>
      <div class="panel-actions">
        <button
          class="btn btn-secondary compact"
          :disabled="store.webView2SystemPrepBusy"
          @click="store.refreshWebView2SystemPrepStatus()"
        >
          {{ store.webView2SystemPrepBusy ? 'Diagnostic…' : 'Lancer le diagnostic' }}
        </button>
        <button
          class="btn btn-secondary compact"
          :disabled="store.webView2SystemPrepBusy || store.webView2SystemPrepStatus?.capabilityInstalled"
          @click="store.installWebView2DeveloperModeCapability()"
        >
          Installer la capability (invite UAC)
        </button>
      </div>
      <div v-if="store.webView2SystemPrepStatus" class="settings-grid compact-grid">
        <div>
          <strong>Mode développeur</strong>
          <span>{{ store.webView2SystemPrepStatus.developerModeEnabled ? 'Activé' : 'Désactivé' }}</span>
        </div>
        <div>
          <strong>Capability Tools.DeveloperMode.Core</strong>
          <span>{{ store.webView2SystemPrepStatus.capabilityState || '—' }}</span>
        </div>
      </div>
      <p v-if="store.webView2CapabilityInstallResult?.message" class="status-line">
        {{ store.webView2CapabilityInstallResult.message }}
      </p>
      <p v-else-if="store.webView2CapabilityInstallResult?.error" class="error">
        {{ store.webView2CapabilityInstallResult.error }}
      </p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>Log principal</h2>
        <span>{{ store.logLines.length }}</span>
      </div>
      <div v-if="store.logLines.length === 0" class="empty-line">
        Aucun log chargé.
      </div>
      <pre v-else class="log-viewer">{{ store.logLines.join('\n') }}</pre>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>Événements Smart Search</h2>
        <div class="panel-actions">
          <span>{{ debugEvents.length }}</span>
          <button class="btn btn-secondary compact" :disabled="debugEvents.length === 0" @click="store.clearSmartSearchDebug()">
            Vider
          </button>
        </div>
      </div>
      <div v-if="debugEvents.length === 0" class="empty-line">
        Aucun événement debug enregistré.
      </div>
      <div v-else class="debug-list">
        <div v-for="(event, index) in debugEvents" :key="index" class="debug-row">
          <div class="debug-head">
            <strong>{{ String(event.event ?? '-') }}</strong>
            <span>{{ String(event.timestamp ?? '') }}</span>
          </div>
          <code v-if="event.query">{{ event.query }}</code>
          <p v-if="eventSummary(event)">{{ eventSummary(event) }}</p>
          <p v-if="event.intentRationale">{{ event.intentRationale }}</p>
          <p v-if="event.message">{{ event.message }}</p>
          <p v-if="event.error" class="error">{{ event.error }}</p>
        </div>
      </div>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>Session</h2>
      </div>
      <div class="actions-row">
        <button class="btn btn-primary" :disabled="store.settingsSaving" @click="saveAll">
          {{ store.settingsSaving ? 'Sauvegarde...' : 'Sauvegarder les paramètres' }}
        </button>
        <button class="btn btn-secondary" @click="store.resetWorkflow()">
          Réinitialiser le workflow
        </button>
        <button class="btn btn-secondary" :disabled="!store.isAttached" @click="store.detach()">
          {{ $t('process.detach') }}
        </button>
      </div>
      <p v-if="store.settingsStatus" class="status-line">{{ store.settingsStatus }}</p>
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
  color: var(--text-dim);
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
  color: var(--text-dim);
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
  color: var(--text-dim);
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
  color: var(--text-dim);
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
  color: var(--text-dim);
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
  color: var(--text-dim);
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
  color: var(--text-dim);
  font-size: 12px;
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

.input::placeholder {
  color: var(--text-dim);
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
  color: var(--text-dim);
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
  color: var(--text-dim);
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
  color: var(--text-dim);
  font-size: 12px;
}

.embedded-agent-list {
  margin-top: 10px;
  color: var(--text-dim);
  font-size: 12px;
}

.embedded-agent-list > strong {
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
  color: var(--text-dim);
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
  color: var(--text-dim);
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
  color: var(--text-dim);
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
</style>
