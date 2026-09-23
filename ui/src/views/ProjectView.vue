<script setup lang="ts">
// UX-PRODUIT-8A (docs/PHASE_TRACKER.md, 22/09/2026) : extrait de SettingsView.vue
// -- projets sauvegardés, notes/bookmarks, modèles de structure, import/export
// workspace, mémoire Auto/motifs mémorisés et journal d'actions, qui mêlaient
// travail courant et préférences dans une seule page de 2200+ lignes. Mêmes
// stores/actions/clés de persistance que Paramètres, aucun nouveau stockage.
import { computed, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { useAppStore, type WorkspaceBookmark, type WorkspaceProject, type StructureTemplate } from '@/stores/app'
import PanelIntro from '@/components/common/PanelIntro.vue'
import PersistenceErrorBanner from '@/components/PersistenceErrorBanner.vue'
import { usePaginatedFilter } from '@/composables/usePaginatedFilter'
import { valueTypeOptions } from '@/utils/valueTypes'

const store = useAppStore()
const { t } = useI18n()

const historyDetailsOpen = ref(false)

// UX-PIPE-9 (docs/PHASE_TRACKER.md, 18/09/2026) : les projets/notes/modèles
// de structure conservés au-delà de la tranche fixe précédemment rendue
// (12/20/12) étaient toujours en stockage mais totalement inaccessibles
// (pas de suivant, pas de recherche) -- voir usePaginatedFilter pour le
// contrat détaillé. Les trois listes ont un état indépendant.
const workspaceProjectsList = computed(() => store.workspaceProjects as WorkspaceProject[])
const workspaceProjectsFilter = usePaginatedFilter(workspaceProjectsList, 12, (project) => `${project.name} ${project.processName}`)
const workspaceBookmarksList = computed(() => store.workspaceBookmarks as WorkspaceBookmark[])
const workspaceBookmarksFilter = usePaginatedFilter(workspaceBookmarksList, 20, (bookmark) => `${bookmark.label} ${bookmark.note} ${bookmark.kind} ${bookmark.address ?? ''}`)
const structureTemplatesList = computed(() => store.structureTemplates as StructureTemplate[])
const structureTemplatesFilter = usePaginatedFilter(structureTemplatesList, 12, (template) => `${template.name} ${template.processName}`)
const learnedAutoProfile = computed(() => store.autoResolveReport?.learnedProfile ?? {})
const strategyWins = computed(() => {
  const wins = learnedAutoProfile.value.strategyWins
  return wins && typeof wins === 'object' ? wins as Record<string, unknown> : {}
})
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
const selectedStructureTemplate = computed(() =>
  store.structureTemplates.find((template) => template.id === selectedStructureTemplateId.value) ?? null,
)
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
  workspaceExportStatus.value = t('settings.exportCopied')
}

async function copyAuditExport() {
  if (!auditExportText.value) return
  await navigator.clipboard?.writeText(auditExportText.value)
  auditExportStatus.value = t('settings.auditCopied')
}

function previewWorkspaceImport() {
  workspaceImportPreview.value = store.previewWorkspaceImport(workspaceImportText.value)
  workspaceImportStatus.value = workspaceImportPreview.value.success === true ? t('settings.previewReady') : String(workspaceImportPreview.value.error ?? t('settings.invalidImport'))
}

// Vidé par importWorkspace() après un succès : ce vidage déclenche le même
// watcher que la saisie utilisateur ci-dessous (Vue ne distingue pas la
// source d'une mutation reactive) -- ce drapeau évite que le message de
// succès qu'on vient d'afficher soit aussitôt écrasé par "aperçu périmé".
let suppressNextImportTextInvalidation = false

async function importWorkspace() {
  const result = await store.importWorkspaceJson(workspaceImportText.value)
  workspaceImportPreview.value = result
  workspaceImportStatus.value = result.success === true
    ? (result.trainerImportBlockReason ? `${t('settings.workspaceImported')} ${result.trainerImportBlockReason}` : t('settings.workspaceImported'))
    : String(result.error ?? t('settings.importRefused'))
  if (result.success === true) {
    suppressNextImportTextInvalidation = true
    workspaceImportText.value = ''
  }
}

// UX-PIPE-7 (docs/PHASE_TRACKER.md, 18/09/2026) : un aperçu réussi ne doit
// plus autoriser Importer une fois le texte modifié -- sans ça, "Importer"
// restait cliquable sur la foi d'un aperçu qui ne correspondait plus au
// texte réellement collé. Invalide (ne recalcule pas automatiquement, pour
// éviter de revalider à chaque frappe) ; l'utilisateur relance "Aperçu"
// explicitement, cohérent avec le bouton déjà désactivé tant qu'aucun aperçu
// valide n'existe pour le texte courant.
watch(workspaceImportText, () => {
  if (suppressNextImportTextInvalidation) {
    suppressNextImportTextInvalidation = false
    return
  }
  if (workspaceImportPreview.value !== null) {
    workspaceImportPreview.value = null
    workspaceImportStatus.value = t('settings.previewStale')
  }
})

function saveWorkspaceProject() {
  const project = store.saveCurrentWorkspaceProject(workspaceProjectName.value)
  if (project) workspaceProjectName.value = ''
}

function addManualBookmark() {
  const bookmark = store.addWorkspaceBookmark({
    kind: bookmarkAddress.value.trim() ? 'address' : 'note',
    label: bookmarkLabel.value || bookmarkAddress.value || t('settings.workspaceNoteFallback'),
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
  <div class="project-view">
    <header class="header">
      <div>
        <h1>{{ $t('project.title') }}</h1>
        <p>{{ $t('project.subtitle') }}</p>
      </div>
      <div v-if="store.isAttached" class="current-target">
        {{ $t('investigation.process') }}: <strong>{{ store.processName }}</strong>
      </div>
    </header>

    <PanelIntro
      :what="$t('project.intro.what')"
      :purpose="$t('project.intro.purpose')"
      :how="$t('project.intro.how')"
    />

    <nav class="project-toc" aria-label="Sommaire">
      <a href="#project-projects">{{ $t('project.sommaire.projects') }}</a>
      <a href="#project-notes">{{ $t('project.sommaire.notes') }}</a>
      <a href="#project-templates">{{ $t('project.sommaire.templates') }}</a>
      <a href="#project-import-export">{{ $t('project.sommaire.importExport') }}</a>
      <a href="#project-history">{{ $t('project.sommaire.history') }}</a>
    </nav>

    <section id="project-projects" class="panel">
      <PersistenceErrorBanner
        :error="store.workspaceProjectsPersistenceError"
        :retry="() => store.saveWorkspaceProjects()"
        :export-data="() => JSON.stringify(store.workspaceProjects, null, 2)"
      />
      <div class="project-panel">
        <div class="panel-title">
          <h3>{{ $t('settings.workspaceProjectsTitle') }}</h3>
          <span>{{ store.workspaceProjects.length }}</span>
          <div class="panel-actions">
            <input v-model="workspaceProjectName" class="input project-name-input" :placeholder="$t('settings.projectNamePlaceholder')" />
            <button class="btn btn-secondary compact" @click="saveWorkspaceProject()">{{ $t('settings.saveProject') }}</button>
          </div>
        </div>
        <div v-if="store.workspaceProjects.length === 0" class="empty-line">{{ $t('settings.noLocalProject') }}</div>
        <template v-else>
          <input
            v-if="store.workspaceProjects.length > 12"
            v-model="workspaceProjectsFilter.filterText.value"
            class="input list-filter-input"
            :placeholder="$t('settings.filterProjectsPlaceholder')"
          />
          <div v-if="workspaceProjectsFilter.filteredCount.value === 0" class="empty-line">{{ $t('settings.noListMatch') }}</div>
          <div v-for="project in workspaceProjectsFilter.visibleItems.value" :key="project.id" class="project-row">
          <div>
            <strong>{{ project.name }}</strong>
            <span>{{ $t('settings.projectSummary', { process: project.processName || '-', features: project.trainerFeatureCount, templates: project.structureTemplateCount, bookmarks: project.bookmarkCount, audits: project.auditCount || 0 }) }}</span>
          </div>
          <div class="panel-actions">
            <button class="btn btn-secondary compact" @click="store.loadWorkspaceProject(project.id)">{{ $t('settings.load') }}</button>
            <button class="btn btn-secondary compact danger-action" @click="store.deleteWorkspaceProject(project.id)">{{ $t('settings.delete') }}</button>
          </div>
          </div>
          <button v-if="workspaceProjectsFilter.hasMore.value" class="btn btn-secondary compact list-show-more" @click="workspaceProjectsFilter.showMore()">
            {{ $t('settings.showMoreItems', { count: Math.min(12, workspaceProjectsFilter.filteredCount.value - workspaceProjectsFilter.visibleItems.value.length) }) }}
          </button>
        </template>
      </div>
    </section>

    <section id="project-notes" class="panel">
      <PersistenceErrorBanner
        :error="store.workspaceBookmarksPersistenceError"
        :retry="() => store.saveWorkspaceBookmarks()"
        :export-data="() => JSON.stringify(store.workspaceBookmarks, null, 2)"
      />
      <div v-if="store.workspaceBookmarks.length > 0" class="bookmark-list">
        <div class="panel-title">
          <h3>{{ $t('settings.bookmarksNotesTitle') }}</h3>
          <span>{{ store.workspaceBookmarks.length }}</span>
        </div>
        <div class="bookmark-create">
          <input v-model="bookmarkLabel" class="input" :placeholder="$t('settings.labelPlaceholder')" />
          <input v-model="bookmarkAddress" class="input" :placeholder="$t('settings.addressHexOptional')" />
          <select v-model="bookmarkType" class="input select">
            <option v-for="type in valueTypeOptions" :key="type" :value="type">{{ type }}</option>
          </select>
          <input v-model="bookmarkValue" class="input" :placeholder="$t('settings.valueOptional')" />
          <input v-model="bookmarkNote" class="input bookmark-note-input" :placeholder="$t('settings.notePlaceholder')" />
          <button class="btn btn-secondary compact" @click="addManualBookmark()">{{ $t('settings.add') }}</button>
        </div>
        <input
          v-if="store.workspaceBookmarks.length > 20"
          v-model="workspaceBookmarksFilter.filterText.value"
          class="input list-filter-input"
          :placeholder="$t('settings.filterBookmarksPlaceholder')"
        />
        <div v-if="workspaceBookmarksFilter.filteredCount.value === 0" class="empty-line">{{ $t('settings.noListMatch') }}</div>
        <div v-for="bookmark in workspaceBookmarksFilter.visibleItems.value" :key="bookmark.id" class="bookmark-row">
          <div>
            <strong>{{ bookmark.label }}</strong>
            <span>{{ bookmark.kind }} · {{ bookmark.address ? `0x${bookmark.address}` : '-' }} · {{ bookmark.type || '-' }} · {{ bookmark.note || '-' }}</span>
          </div>
          <div class="panel-actions">
            <button class="btn btn-secondary compact" :disabled="!bookmark.address" @click="bookmarkToWrite(bookmark)">{{ $t('settings.write') }}</button>
            <button class="btn btn-secondary compact" :disabled="!bookmark.address" @click="bookmarkToTrainer(bookmark, 'write')">{{ $t('settings.trainer') }}</button>
            <button class="btn btn-secondary compact" :disabled="!bookmark.address" @click="bookmarkToTrainer(bookmark, 'freeze_polling')">{{ $t('settings.freeze') }}</button>
            <button class="btn btn-secondary compact danger-action" @click="store.deleteWorkspaceBookmark(bookmark.id)">{{ $t('settings.delete') }}</button>
          </div>
        </div>
        <button v-if="workspaceBookmarksFilter.hasMore.value" class="btn btn-secondary compact list-show-more" @click="workspaceBookmarksFilter.showMore()">
          {{ $t('settings.showMoreItems', { count: Math.min(20, workspaceBookmarksFilter.filteredCount.value - workspaceBookmarksFilter.visibleItems.value.length) }) }}
        </button>
      </div>
      <div v-else class="bookmark-list">
        <div class="panel-title">
          <h3>{{ $t('settings.bookmarksNotesTitle') }}</h3>
          <span>0</span>
        </div>
        <div class="bookmark-create">
          <input v-model="bookmarkLabel" class="input" :placeholder="$t('settings.labelPlaceholder')" />
          <input v-model="bookmarkAddress" class="input" :placeholder="$t('settings.addressHexOptional')" />
          <select v-model="bookmarkType" class="input select">
            <option v-for="type in valueTypeOptions" :key="type" :value="type">{{ type }}</option>
          </select>
          <input v-model="bookmarkValue" class="input" :placeholder="$t('settings.valueOptional')" />
          <input v-model="bookmarkNote" class="input bookmark-note-input" :placeholder="$t('settings.notePlaceholder')" />
          <button class="btn btn-secondary compact" @click="addManualBookmark()">{{ $t('settings.add') }}</button>
        </div>
      </div>
    </section>

    <section id="project-templates" class="panel">
      <PersistenceErrorBanner
        :error="store.structureTemplatesPersistenceError"
        :retry="() => store.saveStructureTemplates()"
        :export-data="() => JSON.stringify(store.structureTemplates, null, 2)"
      />
      <div v-if="store.structureTemplates.length > 0" class="template-list">
        <div class="panel-title">
          <h3>{{ $t('settings.structureTemplates') }}</h3>
          <span>{{ store.structureTemplates.length }}</span>
        </div>
        <input
          v-if="store.structureTemplates.length > 12"
          v-model="structureTemplatesFilter.filterText.value"
          class="input list-filter-input"
          :placeholder="$t('settings.filterStructuresPlaceholder')"
        />
        <div v-if="structureTemplatesFilter.filteredCount.value === 0" class="empty-line">{{ $t('settings.noListMatch') }}</div>
        <div v-for="template in structureTemplatesFilter.visibleItems.value" :key="template.id" class="template-row">
          <div>
            <strong>{{ template.name }}</strong>
            <span>0x{{ template.baseAddress }} · {{ $t('settings.fieldCount', { count: template.fieldCount }) }} · {{ template.processName || '-' }}</span>
          </div>
          <div class="panel-actions">
            <button class="btn btn-secondary compact" @click="selectedStructureTemplateId = template.id">{{ $t('settings.details') }}</button>
            <button class="btn btn-secondary compact danger-action" @click="store.deleteStructureTemplate(template.id)">{{ $t('settings.delete') }}</button>
          </div>
        </div>
        <button v-if="structureTemplatesFilter.hasMore.value" class="btn btn-secondary compact list-show-more" @click="structureTemplatesFilter.showMore()">
          {{ $t('settings.showMoreItems', { count: Math.min(12, structureTemplatesFilter.filteredCount.value - structureTemplatesFilter.visibleItems.value.length) }) }}
        </button>
        <div v-if="selectedStructureTemplate" class="template-detail">
          <div class="panel-title">
            <h3>{{ selectedStructureTemplate.name }}</h3>
            <button class="btn btn-secondary compact" @click="selectedStructureTemplateId = null">{{ $t('settings.close') }}</button>
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
    </section>

    <section id="project-import-export" class="panel">
      <div class="panel-title">
        <h2>{{ $t('project.sommaire.importExport') }}</h2>
        <div class="panel-actions">
          <button class="btn btn-secondary compact" @click="showWorkspaceExport()">{{ $t('settings.exportWorkspaceJson') }}</button>
          <button class="btn btn-secondary compact" @click="showWorkspaceMarkdownExport()">{{ $t('settings.exportWorkspaceMd') }}</button>
        </div>
      </div>
      <div class="workspace-import">
        <div class="panel-title">
          <h3>{{ $t('settings.importWorkspaceJsonTitle') }}</h3>
          <div class="panel-actions">
            <button class="btn btn-secondary compact" :disabled="!workspaceImportText.trim()" @click="previewWorkspaceImport()">{{ $t('settings.importPreviewAction') }}</button>
            <button class="btn btn-secondary compact danger-action" :disabled="workspaceImportPreview?.success !== true" @click="importWorkspace()">{{ $t('settings.importAction') }}</button>
          </div>
        </div>
        <textarea v-model="workspaceImportText" class="workspace-import-input" :placeholder="$t('settings.pasteWorkspaceExportPlaceholder')" />
        <p v-if="workspaceImportStatus" class="status-line">{{ workspaceImportStatus }}</p>
        <div v-if="workspaceImportPreview?.success === true" class="import-preview">
          <span>{{ $t('settings.importPreviewActiveInvestigation', { value: workspaceImportPreview.activeInvestigation }) }}</span>
          <span>{{ $t('settings.importPreviewArchives', { value: workspaceImportPreview.archiveCount }) }}</span>
          <span>{{ $t('settings.importPreviewFeatures', { value: workspaceImportPreview.trainerFeatureCount }) }}</span>
          <span>{{ $t('settings.importPreviewTemplates', { value: workspaceImportPreview.structureTemplateCount }) }}</span>
          <span>{{ $t('settings.importPreviewBookmarks', { value: workspaceImportPreview.bookmarkCount }) }}</span>
          <span>{{ $t('settings.importPreviewAudit', { value: workspaceImportPreview.auditCount || 0 }) }}</span>
          <span>{{ $t('settings.importPreviewPreset', { value: workspaceImportPreview.lastPresetId || '-' }) }}</span>
          <span>{{ $t('settings.importPreviewSettings', { value: workspaceImportPreview.hasSettings ? $t('settings.yes') : $t('settings.no') }) }}</span>
        </div>
      </div>
      <div v-if="workspaceExportText" class="workspace-export">
        <div class="panel-title">
          <h3>{{ $t('settings.exportWorkspaceTitle') }}</h3>
          <div class="panel-actions">
            <button class="btn btn-secondary compact" @click="copyWorkspaceExport()">{{ $t('settings.copy') }}</button>
            <button class="btn btn-secondary compact" @click="workspaceExportText = ''">{{ $t('settings.close') }}</button>
          </div>
        </div>
        <p v-if="workspaceExportStatus" class="status-line">{{ workspaceExportStatus }}</p>
        <pre>{{ workspaceExportText }}</pre>
      </div>
    </section>

    <section id="project-history" class="panel">
      <div class="panel-title">
        <h2>{{ $t('project.sommaire.history') }}</h2>
      </div>
      <PersistenceErrorBanner
        :error="store.actionLogPersistenceError"
        :retry="() => store.saveActionLog()"
        :export-data="() => JSON.stringify(store.actionLog, null, 2)"
      />
      <div class="audit-panel">
        <div class="panel-title">
          <h3>{{ $t('settings.actionAudit') }}</h3>
          <span>{{ store.actionLog.length }}</span>
          <div class="panel-actions">
            <button class="btn btn-secondary compact" @click="showAuditJsonExport()">{{ $t('settings.exportAuditJson') }}</button>
            <button class="btn btn-secondary compact" @click="showAuditMarkdownExport()">{{ $t('settings.exportAuditMd') }}</button>
          </div>
        </div>
        <div v-if="store.actionLog.length === 0" class="empty-line">{{ $t('settings.noAuditedAction') }}</div>
        <div v-for="entry in store.actionLog.slice(0, 20)" :key="entry.id" class="audit-row">
          <div>
            <strong>{{ entry.title }}</strong>
            <span>{{ entry.time }} · {{ entry.kind }} · {{ entry.status }}{{ entry.detail ? ` · ${entry.detail}` : '' }}</span>
          </div>
        </div>
      </div>
      <div v-if="auditExportText" class="workspace-export">
        <div class="panel-title">
          <h3>{{ $t('settings.exportAuditTitle') }}</h3>
          <div class="panel-actions">
            <button class="btn btn-secondary compact" @click="copyAuditExport()">{{ $t('settings.copy') }}</button>
            <button class="btn btn-secondary compact" @click="auditExportText = ''">{{ $t('settings.close') }}</button>
          </div>
        </div>
        <p v-if="auditExportStatus" class="status-line">{{ auditExportStatus }}</p>
        <pre>{{ auditExportText }}</pre>
      </div>
      <details class="history-details" :open="historyDetailsOpen">
        <summary>{{ $t('project.autoMemoryDetailsTitle') }}</summary>
      <div class="runtime-grid">
        <div class="runtime-cell">
          <span>{{ $t('settings.activeInvestigation') }}</span>
          <strong>{{ $t('settings.stepCount', { count: store.activeInvestigation ? store.activeInvestigation.steps.length : 0 }) }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.investigationArchives') }}</span>
          <strong>{{ store.investigationArchive.length }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.trainerFeatures') }}</span>
          <strong>{{ store.trainerFeatures.length }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.structureTemplates') }}</span>
          <strong>{{ store.structureTemplates.length }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.bookmarks') }}</span>
          <strong>{{ store.workspaceBookmarks.length }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.localProjects') }}</span>
          <strong>{{ store.workspaceProjects.length }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.actionAudit') }}</span>
          <strong>{{ store.actionLog.length }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.autoMemoryProcess') }}</span>
          <strong>{{ store.processName || $t('settings.globalFallback') }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.lastStrategy') }}</span>
          <strong>{{ learnedAutoProfile.lastSuccessfulAuditEvent || '-' }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.lastAddress') }}</span>
          <strong>{{ learnedAutoProfile.lastSuccessfulAddress ? `0x${learnedAutoProfile.lastSuccessfulAddress}` : '-' }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.lastType') }}</span>
          <strong>{{ learnedAutoProfile.lastSuccessfulValueType || '-' }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.winningStrategies') }}</span>
          <strong>{{ Object.keys(strategyWins).length }}</strong>
        </div>
      </div>
      <div class="path-row">
        <span>{{ $t('settings.learnedAobPattern') }}</span>
        <code>{{ learnedAutoProfile.lastSuccessfulAobPattern || '-' }}</code>
      </div>
      <div v-if="store.rememberedPatterns.length" class="remembered-patterns">
        <div class="source-list-title">
          <strong>{{ $t('settings.rememberedPatternsTitle') }}</strong>
          <span>{{ $t('settings.entryCount', { count: store.rememberedPatterns.length }) }}</span>
        </div>
        <div v-for="pattern in store.rememberedPatterns" :key="`${pattern.module}:${pattern.moduleOffset}`" class="remembered-pattern-row">
          <div class="remembered-pattern-label">
            <strong v-if="pattern.queryLabel" :title="String(pattern.queryLabel)">{{ pattern.queryLabel }}</strong>
            <code>{{ pattern.module }}+0x{{ pattern.moduleOffset }}</code>
          </div>
          <span>{{ pattern.valueType || '-' }}</span>
          <span>{{ pattern.confirmCount }}×</span>
          <span :class="pattern.resolved ? 'hint' : 'error'">
            {{ pattern.resolved ? `0x${pattern.liveAddress}` : $t('settings.moduleAbsent') }}
          </span>
          <button
            v-if="pattern.resolved"
            class="btn btn-secondary compact"
            type="button"
            @click="store.previewRememberedPattern(pattern)"
          >
            {{ $t('settings.preview') }}
          </button>
        </div>
      </div>
      <div v-if="store.writeHistorySequence.length" class="remembered-patterns">
        <div class="source-list-title">
          <strong>{{ $t('settings.writeHistoryTitle') }}</strong>
          <span>{{ $t('settings.entryCount', { count: store.writeHistorySequence.length }) }}</span>
        </div>
        <div v-for="(entry, index) in store.writeHistorySequence" :key="`${entry.module}:${entry.moduleOffset}:${entry.writtenAt}:${index}`" class="remembered-pattern-row">
          <code>{{ entry.module }}+0x{{ entry.moduleOffset }}</code>
          <span>{{ entry.valueType || '-' }} = {{ entry.value }}</span>
          <span :class="entry.resolved ? 'hint' : 'error'">
            {{ entry.resolved ? `0x${entry.liveAddress}` : $t('settings.moduleAbsent') }}
          </span>
        </div>
        <div class="panel-actions">
          <button class="btn btn-primary compact" type="button" @click="store.replayWriteHistorySequence()">
            {{ $t('settings.replaySequence') }}
          </button>
          <button class="btn btn-secondary compact" type="button" @click="store.clearWriteHistorySequence()">
            {{ $t('settings.clearHistory') }}
          </button>
        </div>
      </div>
      <div class="panel-actions workspace-actions">
        <button class="btn btn-secondary compact" @click="store.clearInvestigation()">{{ $t('settings.clearActiveInvestigation') }}</button>
        <button class="btn btn-secondary compact" @click="store.clearInvestigationArchive()">{{ $t('settings.clearArchives') }}</button>
        <button class="btn btn-secondary compact" @click="store.clearTrainerFeatures()">{{ $t('settings.clearLocalTrainer') }}</button>
        <button class="btn btn-secondary compact" @click="store.clearStructureTemplates()">{{ $t('settings.clearStructureTemplates') }}</button>
        <button class="btn btn-secondary compact" @click="store.clearWorkspaceBookmarks()">{{ $t('settings.clearBookmarks') }}</button>
        <button class="btn btn-secondary compact" @click="store.clearWorkspaceProjects()">{{ $t('settings.clearProjects') }}</button>
        <button class="btn btn-secondary compact" @click="store.clearActionLog()">{{ $t('settings.clearAudit') }}</button>
        <button class="btn btn-secondary compact" @click="store.clearAutoResolveMemory(false)">{{ $t('settings.clearAutoMemoryProcess') }}</button>
        <button class="btn btn-secondary compact danger-action" @click="store.clearAutoResolveMemory(true)">{{ $t('settings.clearAutoMemoryGlobal') }}</button>
      </div>
      <p class="hint">
        {{ $t('settings.workspaceActionsHint') }}
      </p>
      </details>
    </section>
  </div>
</template>

<style scoped>
.project-view {
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

.embedded-agent-list {
  margin-top: 10px;
  color: var(--text-muted);
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

.stealth-analysis {
  margin-top: 12px;
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.stealth-risk-line {
  display: flex;
  align-items: center;
  gap: 10px;
}

.risk-badge {
  padding: 3px 10px;
  border-radius: 999px;
  font-size: 12px;
  font-weight: 600;
  text-transform: uppercase;
  border: 1px solid var(--border);
}

.risk-badge.risk-low {
  color: var(--success);
  border-color: color-mix(in srgb, var(--success) 50%, var(--border));
}

.risk-badge.risk-medium {
  color: var(--warning);
  border-color: color-mix(in srgb, var(--warning) 50%, var(--border));
}

.risk-badge.risk-high {
  color: var(--error);
  border-color: color-mix(in srgb, var(--error) 50%, var(--border));
}

.stealth-threat-list,
.stealth-recommendation-list {
  margin: 0;
  padding-left: 18px;
  font-size: 13px;
  color: var(--text-secondary);
}

.stealth-threat-list li,
.stealth-recommendation-list li {
  margin-bottom: 4px;
}

/* UX-PRODUIT-8A : nouveaux éléments propres à cette vue (le reste du bloc
   ci-dessus est copié tel quel depuis SettingsView.vue pour ne rien casser
   dans les styles partagés -- une partie de ces règles n'est donc plus
   utilisée ici, sans risque puisque scoped). */
.header {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: 8px;
}

.current-target {
  color: var(--text-muted);
  font-size: 12px;
}

.current-target strong {
  color: var(--text-primary);
}

.project-toc {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-bottom: 14px;
}

.project-toc a {
  padding: 5px 10px;
  border: 1px solid var(--border);
  border-radius: 999px;
  color: var(--text-muted);
  font-size: 12px;
  text-decoration: none;
}

.project-toc a:hover {
  color: var(--text-primary);
  border-color: rgba(122, 162, 247, 0.6);
}

.history-details summary {
  cursor: pointer;
  padding: 8px 0;
  color: var(--text-primary);
  font-size: 13px;
  font-weight: 600;
}

.history-details[open] summary {
  margin-bottom: 8px;
}
</style>
