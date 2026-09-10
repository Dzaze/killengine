/**
 * KillEngine — store Workspace Session (extrait de app.ts, candidat S9b de
 * docs/REFACTOR_ROADMAP.md, PHASE 234, 30/08/2026).
 *
 * S9b regroupe le reste de S9 après S9a (`workspaceItems.ts`, CRUD pur
 * templates/bookmarks) : projets workspace (snapshot JSON complet,
 * save/load/delete/clear), export/import JSON+Markdown de l'espace de
 * travail, et le panneau Pointer Chain Watch (`PointerChainWatchPanel.vue`/
 * `useExpertPointerChain.ts`). Annoté couplage "Haut" dans le roadmap
 * (bidirectionnel avec S7/S8) — vérifié en pratique une fois S1/S7/S8/S9a
 * déjà extraits en feuilles indépendantes (`clrInspector.ts`/`writeFreeze.ts`/
 * `trainer.ts`/`workspaceItems.ts`) : `exportWorkspaceJson`/
 * `importWorkspaceJson` lisent/écrivent presque tous les domaines déjà
 * extraits (import direct, sans dépendance vers `./app`), plus
 * `actionLog.ts`/`investigation.ts`/`settings.ts` (fondations déjà feuilles).
 * Seuls des bouts de state encore possédés par `app.ts` (Process/Assistant/
 * Diagnostics, pas encore de candidat dédié) et le dispatch mode-kernel
 * (Memory) restent transversaux : injectés UNE SEULE FOIS via
 * `configureWorkspaceSessionContext`, même patron que `writeFreeze.ts`/
 * `trainer.ts`. Le pont Session→Trainer (`promoteSessionEntryToTrainer`...)
 * et le dispatch chat/IA restent hors périmètre (Session/S10), non touchés.
 */
import { defineStore, storeToRefs } from 'pinia'
import { ref, type Ref } from 'vue'
import { i18n } from '@/i18n'
import {
  backend,
  type AppSettings,
  type AutoResolveReportResult,
  type MemoryReadPreview,
} from '@/services/backend'
import { useActionLogStore, type UserActionLogEntry } from './actionLog'
import { useInvestigationStore, type InvestigationRun } from './investigation'
import { useTrainerStore, type TrainerFeature } from './trainer'
import {
  useWorkspaceItemsStore,
  type StructureTemplate,
  type WorkspaceBookmark,
} from './workspaceItems'
import { useSettingsStore } from './settings'

const { t } = i18n.global

export interface WorkspaceProject {
  id: number
  name: string
  processName: string
  snapshotJson: string
  investigationCount: number
  trainerFeatureCount: number
  structureTemplateCount: number
  bookmarkCount: number
  auditCount?: number
  createdAt: string
  updatedAt: string
}

export interface WatchedPointerChain {
  id: number
  label: string
  chain: { module: string, baseOffset: string, offsets: string[] }
  type: string
  finalAddress: string
  value: string
  previousValue: string
  changed: boolean
  error: string
  updatedAt: string
}

export interface WorkflowPresetSnapshot {
  id: string
  title: string
  mode: string
  risk: string
  nextStep: string
}

export interface WorkspaceSessionExternalDeps {
  version: Ref<string>
  processName: Ref<string>
  isAttached: Ref<boolean>
  workflowStatus: Ref<string>
  lastWorkflowPresetId: Ref<string>
  workflowPresets: Ref<WorkflowPresetSnapshot[]>
  autoResolveReport: Ref<AutoResolveReportResult | null>
  logFilePath: Ref<string>
  smartSearchDebugFilePath: Ref<string>
  scanTelemetryFilePath: Ref<string>
  readMemoryPreviewByMode: (addressHex: string, size: number) => Promise<MemoryReadPreview>
  valueTypeReadSize: (type: string) => number
  decodeTypedPreviewValue: (preview: MemoryReadPreview, type: string) => string
}

export const useWorkspaceSessionStore = defineStore('workspaceSession', () => {
  const actionLogStore = useActionLogStore()
  const { addActionLog } = actionLogStore
  const { actionLog, actionLogIdCounter } = storeToRefs(actionLogStore)
  const { saveActionLog } = actionLogStore

  const investigationStore = useInvestigationStore()
  const { addInvestigationStep, saveInvestigations } = investigationStore
  const {
    activeInvestigation,
    investigationArchive,
    investigationStepIdCounter,
    investigationRunIdCounter,
  } = storeToRefs(investigationStore)

  const trainerStore = useTrainerStore()
  const { saveTrainerFeatures, refreshTrainerOverlay } = trainerStore
  const { trainerFeatures, trainerFeatureIdCounter } = storeToRefs(trainerStore)

  const workspaceItemsStore = useWorkspaceItemsStore()
  const { saveStructureTemplates, saveWorkspaceBookmarks } = workspaceItemsStore
  const {
    structureTemplates,
    structureTemplateIdCounter,
    workspaceBookmarks,
    workspaceBookmarkIdCounter,
  } = storeToRefs(workspaceItemsStore)

  const settingsStore = useSettingsStore()
  const {
    appLanguage,
    settingDefaultValueType,
    settingPerformanceMode,
    settingModelEnabled,
    settingModelPath,
    settingModelThreads,
    settingScanMaxResults,
    settingUnknownSnapshotMaxMb,
    aiModelStatus,
  } = storeToRefs(settingsStore)

  let externalDeps: WorkspaceSessionExternalDeps | null = null
  function configureWorkspaceSessionContext(newDeps: WorkspaceSessionExternalDeps) {
    externalDeps = newDeps
  }
  function deps(): WorkspaceSessionExternalDeps {
    if (!externalDeps) throw new Error('workspaceSession store used before configureWorkspaceSessionContext')
    return externalDeps
  }

  const workspaceProjectStorageKey = 'killengine.workspace.projects.v1'
  const workspaceProjects = ref<WorkspaceProject[]>([])
  const workspaceProjectIdCounter = ref(0)

  function saveWorkspaceProjects() {
    try {
      window.localStorage.setItem(workspaceProjectStorageKey, JSON.stringify({
        projects: workspaceProjects.value,
        id: workspaceProjectIdCounter.value,
      }))
    } catch {
      // Best-effort persistence.
    }
  }

  function loadWorkspaceProjects() {
    try {
      const raw = window.localStorage.getItem(workspaceProjectStorageKey)
      if (!raw) return
      const parsed = JSON.parse(raw) as { projects?: WorkspaceProject[], id?: number }
      workspaceProjects.value = Array.isArray(parsed.projects) ? parsed.projects.slice(0, 50) : []
      workspaceProjectIdCounter.value = Number(parsed.id ?? 0)
    } catch {
      workspaceProjects.value = []
      workspaceProjectIdCounter.value = 0
    }
  }

  function exportWorkspaceJson(): string {
    const d = deps()
    return JSON.stringify({
      version: 1,
      exportedAt: new Date().toISOString(),
      app: {
        version: d.version.value,
        processName: d.processName.value,
        attached: d.isAttached.value,
        workflowStatus: d.workflowStatus.value,
      },
      settings: {
        language: appLanguage.value,
        defaultValueType: settingDefaultValueType.value,
        performanceMode: settingPerformanceMode.value,
        modelEnabled: settingModelEnabled.value,
        modelPath: settingModelPath.value,
        modelThreads: settingModelThreads.value,
        scanMaxResults: settingScanMaxResults.value,
        unknownSnapshotMaxMb: settingUnknownSnapshotMaxMb.value,
      },
      workflowPresets: {
        lastPresetId: d.lastWorkflowPresetId.value,
        available: d.workflowPresets.value.map((preset) => ({
          id: preset.id,
          title: preset.title,
          mode: preset.mode,
          risk: preset.risk,
          nextStep: preset.nextStep,
        })),
      },
      investigation: {
        active: activeInvestigation.value,
        archive: investigationArchive.value,
      },
      trainer: {
        features: trainerFeatures.value,
      },
      structures: {
        templates: structureTemplates.value,
      },
      bookmarks: {
        items: workspaceBookmarks.value,
      },
      audit: {
        entries: actionLog.value.slice(0, 200),
      },
      autoResolve: {
        report: d.autoResolveReport.value,
      },
      aiModel: {
        status: aiModelStatus.value,
      },
      diagnostics: {
        logFilePath: d.logFilePath.value,
        smartSearchDebugFilePath: d.smartSearchDebugFilePath.value,
        scanTelemetryFilePath: d.scanTelemetryFilePath.value,
      },
    }, null, 2)
  }

  function workspaceBookmarkMarkdownLine(bookmark: WorkspaceBookmark): string {
    const payload = bookmark.payload ?? {}
    const details = [
      bookmark.address ? `0x${bookmark.address}` : '',
      bookmark.type ? `type ${bookmark.type}` : '',
      bookmark.value !== undefined ? t('workspaceSessionStore.markdown.value', { value: bookmark.value }) : '',
      payload.confidenceLabel ? String(payload.confidenceLabel) : '',
      Number(payload.confidenceScore ?? 0) > 0 ? `score ${String(payload.confidenceScore)}/100` : '',
      payload.requiresConfirmation === true ? t('workspaceSessionStore.markdown.confirmationRequired') : '',
      payload.aobPattern ? `AOB ${String(payload.aobPattern).slice(0, 80)}` : '',
      payload.patchBytes ? `patch ${String(payload.patchBytes).slice(0, 40)}` : '',
      payload.signatureLevel ? t('workspaceSessionStore.markdown.quality', { level: String(payload.signatureLevel), score: payload.signatureScore ? ` ${String(payload.signatureScore)}/100` : '' }) : '',
      payload.signatureMatches !== undefined ? `${String(payload.signatureMatches)} match(es)` : '',
    ].filter(Boolean)
    return `- ${bookmark.kind} ${bookmark.label}${details.length > 0 ? ` - ${details.join(' · ')}` : ''}${bookmark.note ? ` - ${bookmark.note}` : ''}`
  }

  function exportWorkspaceMarkdown(): string {
    const d = deps()
    const report = d.autoResolveReport.value
    const lines = [
      '# KillEngine Workspace',
      '',
      t('workspaceSessionStore.markdown.export', { value: new Date().toISOString() }),
      t('workspaceSessionStore.markdown.version', { value: d.version.value }),
      t('workspaceSessionStore.markdown.process', { value: d.processName.value || t('workspaceSessionStore.markdown.notAttached') }),
      t('workspaceSessionStore.markdown.workflow', { value: d.workflowStatus.value }),
      t('workspaceSessionStore.markdown.preset', { value: d.lastWorkflowPresetId.value || t('workspaceSessionStore.markdown.none') }),
      t('workspaceSessionStore.markdown.localAi', { value: aiModelStatus.value?.ready ? 'llama.cpp' : t('workspaceSessionStore.markdown.unavailable') }),
      '',
      `## ${t('workspaceSessionStore.markdown.availablePresetsTitle')}`,
      '',
      ...d.workflowPresets.value.map((preset) => `- ${preset.title}: ${preset.mode} / ${preset.risk} - ${preset.nextStep}`),
      '',
      '## Investigation',
      '',
      t('workspaceSessionStore.markdown.active', { value: activeInvestigation.value ? activeInvestigation.value.objective : t('workspaceSessionStore.markdown.none') }),
      t('workspaceSessionStore.markdown.activeSteps', { value: activeInvestigation.value?.steps.length ?? 0 }),
      t('workspaceSessionStore.markdown.archives', { value: investigationArchive.value.length }),
      '',
      '## Trainer',
      '',
      t('workspaceSessionStore.markdown.features', { value: trainerFeatures.value.length }),
      ...trainerFeatures.value.slice(0, 12).map((feature) => `- ${feature.name}: ${feature.action} 0x${feature.address} (${feature.status})`),
      '',
      `## ${t('workspaceSessionStore.markdown.structuresTitle')}`,
      '',
      t('workspaceSessionStore.markdown.templates', { value: structureTemplates.value.length }),
      ...structureTemplates.value.slice(0, 12).map((template) => t('workspaceSessionStore.markdown.templateLine', { name: template.name, count: template.fieldCount, address: template.baseAddress })),
      ...structureTemplates.value.slice(0, 5).flatMap((template) => [
        '',
        `### ${template.name}`,
        ...template.fields.slice(0, 20).map((field) =>
          `- ${field.offset >= 0 ? '+' : ''}${field.offset} ${field.type} ${field.label || '-'} = ${field.sampleValue || '-'}${field.note ? ` (${field.note})` : ''}`,
        ),
      ]),
      '',
      '## Bookmarks',
      '',
      t('workspaceSessionStore.markdown.bookmarks', { value: workspaceBookmarks.value.length }),
      ...workspaceBookmarks.value.slice(0, 20).map((bookmark) => workspaceBookmarkMarkdownLine(bookmark)),
      '',
      `## ${t('workspaceSessionStore.markdown.auditTitle')}`,
      '',
      t('workspaceSessionStore.markdown.entries', { value: actionLog.value.length }),
      ...actionLog.value.slice(0, 30).map((entry) => `- ${entry.time} [${entry.status}] ${entry.kind} - ${entry.title}${entry.detail ? `: ${entry.detail}` : ''}`),
      '',
      `## ${t('workspaceSessionStore.markdown.autoReportTitle')}`,
      '',
      report
        ? t('workspaceSessionStore.markdown.strategy', { value: String(report.preferredStrategy?.label ?? t('workspaceSessionStore.markdown.notDetermined')) })
        : t('workspaceSessionStore.markdown.noAutoReportLoaded'),
      report?.summary ? t('workspaceSessionStore.markdown.summary', { value: report.summary }) : '',
      '',
      `## ${t('workspaceSessionStore.markdown.diagnosticsTitle')}`,
      '',
      `Log: ${d.logFilePath.value || '-'}`,
      `Smart Search JSONL: ${d.smartSearchDebugFilePath.value || '-'}`,
      `Telemetry JSONL: ${d.scanTelemetryFilePath.value || '-'}`,
      '',
    ].filter((line) => line !== '')
    return lines.join('\n')
  }

  function previewWorkspaceImport(raw: string) {
    try {
      const parsed = JSON.parse(raw) as Record<string, unknown>
      const investigation = parsed.investigation as Record<string, unknown> | undefined
      const trainer = parsed.trainer as Record<string, unknown> | undefined
      const structures = parsed.structures as Record<string, unknown> | undefined
      const bookmarksRoot = parsed.bookmarks as Record<string, unknown> | undefined
      const auditRoot = parsed.audit as Record<string, unknown> | undefined
      const settings = parsed.settings as Record<string, unknown> | undefined
      const presetsRoot = parsed.workflowPresets as Record<string, unknown> | undefined
      const active = investigation?.active && typeof investigation.active === 'object' ? 1 : 0
      const archive = Array.isArray(investigation?.archive) ? investigation.archive.length : 0
      const features = Array.isArray(trainer?.features) ? trainer.features.length : 0
      const templates = Array.isArray(structures?.templates) ? structures.templates.length : 0
      const bookmarks = Array.isArray(bookmarksRoot?.items) ? bookmarksRoot.items.length : 0
      const audit = Array.isArray(auditRoot?.entries) ? auditRoot.entries.length : 0
      const presetId = String(presetsRoot?.lastPresetId ?? '')
      return {
        success: true,
        version: Number(parsed.version ?? 0),
        exportedAt: String(parsed.exportedAt ?? ''),
        activeInvestigation: active,
        archiveCount: archive,
        trainerFeatureCount: features,
        structureTemplateCount: templates,
        bookmarkCount: bookmarks,
        auditCount: audit,
        lastPresetId: presetId,
        hasSettings: Boolean(settings),
      }
    } catch (e) {
      return {
        success: false,
        error: String(e),
      }
    }
  }

  function importWorkspaceJson(raw: string) {
    const d = deps()
    const preview = previewWorkspaceImport(raw)
    if (preview.success !== true) return preview

    const parsed = JSON.parse(raw) as Record<string, unknown>
    const investigation = parsed.investigation as Record<string, unknown> | undefined
    const trainer = parsed.trainer as Record<string, unknown> | undefined
    const structures = parsed.structures as Record<string, unknown> | undefined
    const bookmarksRoot = parsed.bookmarks as Record<string, unknown> | undefined
    const auditRoot = parsed.audit as Record<string, unknown> | undefined
    const settings = parsed.settings as Record<string, unknown> | undefined
    const presetsRoot = parsed.workflowPresets as Record<string, unknown> | undefined

    if (investigation) {
      activeInvestigation.value =
        investigation.active && typeof investigation.active === 'object'
          ? investigation.active as InvestigationRun
          : null
      investigationArchive.value = Array.isArray(investigation.archive)
        ? (investigation.archive as InvestigationRun[]).slice(0, 20)
        : []
      investigationStepIdCounter.value = Math.max(
        investigationStepIdCounter.value,
        activeInvestigation.value?.steps.reduce((max, step) => Math.max(max, Number(step.id) || 0), 0) ?? 0,
        ...investigationArchive.value.map((run) => run.steps.reduce((max, step) => Math.max(max, Number(step.id) || 0), 0)),
      )
      investigationRunIdCounter.value = Math.max(
        investigationRunIdCounter.value,
        Number(activeInvestigation.value?.id ?? 0),
        ...investigationArchive.value.map((run) => Number(run.id) || 0),
      )
      saveInvestigations()
    }

    if (trainer && Array.isArray(trainer.features)) {
      trainerFeatures.value = (trainer.features as TrainerFeature[]).slice(0, 200)
      trainerFeatureIdCounter.value = Math.max(0, ...trainerFeatures.value.map((feature) => Number(feature.id) || 0))
      saveTrainerFeatures()
      void refreshTrainerOverlay()
    }

    if (structures && Array.isArray(structures.templates)) {
      structureTemplates.value = (structures.templates as StructureTemplate[]).slice(0, 100)
      structureTemplateIdCounter.value = Math.max(0, ...structureTemplates.value.map((template) => Number(template.id) || 0))
      saveStructureTemplates()
    }

    if (bookmarksRoot && Array.isArray(bookmarksRoot.items)) {
      workspaceBookmarks.value = (bookmarksRoot.items as WorkspaceBookmark[]).slice(0, 500)
      workspaceBookmarkIdCounter.value = Math.max(0, ...workspaceBookmarks.value.map((bookmark) => Number(bookmark.id) || 0))
      saveWorkspaceBookmarks()
    }

    if (auditRoot && Array.isArray(auditRoot.entries)) {
      actionLog.value = (auditRoot.entries as UserActionLogEntry[]).slice(0, 200)
      actionLogIdCounter.value = Math.max(0, ...actionLog.value.map((entry) => Number(entry.id) || 0))
      saveActionLog()
    }

    if (settings) {
      if (settings.language === 'fr' || settings.language === 'en') appLanguage.value = settings.language
      if (typeof settings.defaultValueType === 'string') settingDefaultValueType.value = settings.defaultValueType
      if (['Auto', 'Eco', 'Normal', 'Performance', 'Max'].includes(String(settings.performanceMode))) {
        settingPerformanceMode.value = settings.performanceMode as AppSettings['performanceMode']
      }
      if (typeof settings.modelEnabled === 'boolean') settingModelEnabled.value = settings.modelEnabled
      if (typeof settings.modelPath === 'string') settingModelPath.value = settings.modelPath
      if (Number.isFinite(Number(settings.modelThreads))) settingModelThreads.value = Number(settings.modelThreads)
      if (Number.isFinite(Number(settings.scanMaxResults))) settingScanMaxResults.value = Number(settings.scanMaxResults)
      if (Number.isFinite(Number(settings.unknownSnapshotMaxMb))) settingUnknownSnapshotMaxMb.value = Number(settings.unknownSnapshotMaxMb)
    }

    const presetId = String(presetsRoot?.lastPresetId ?? '')
    if (presetId && d.workflowPresets.value.some((preset) => preset.id === presetId)) {
      d.lastWorkflowPresetId.value = presetId
    }

    addActionLog(
      'workspace',
      t('workspaceSessionStore.workspaceImported'),
      t('workspaceSessionStore.importedCounts', { features: preview.trainerFeatureCount, templates: preview.structureTemplateCount, bookmarks: preview.bookmarkCount, archives: preview.archiveCount, audits: preview.auditCount ?? 0 }),
      'success',
    )
    addInvestigationStep({
      title: t('workspaceSessionStore.workspaceImported'),
      detail: t('workspaceSessionStore.importedCountsDetailed', { features: preview.trainerFeatureCount, templates: preview.structureTemplateCount, bookmarks: preview.bookmarkCount, archives: preview.archiveCount, audits: preview.auditCount ?? 0, hasSettings: preview.hasSettings ? t('workspaceSessionStore.yes') : t('workspaceSessionStore.no'), preset: String(preview.lastPresetId || '-') }),
      status: 'success',
      tool: 'importWorkspaceJson',
      risk: 'safe',
      payload: preview,
    })
    return {
      ...preview,
      imported: true,
    }
  }

  function saveCurrentWorkspaceProject(name?: string) {
    const d = deps()
    workspaceProjectIdCounter.value += 1
    const now = new Date().toISOString()
    const fallbackName = d.processName.value
      ? `${d.processName.value.replace(/\.[^.]+$/, '')} workspace`
      : 'KillEngine workspace'
    const snapshotJson = exportWorkspaceJson()
    const project: WorkspaceProject = {
      id: workspaceProjectIdCounter.value,
      name: String(name ?? fallbackName).trim() || fallbackName,
      processName: d.processName.value,
      snapshotJson,
      investigationCount: (activeInvestigation.value ? 1 : 0) + investigationArchive.value.length,
      trainerFeatureCount: trainerFeatures.value.length,
      structureTemplateCount: structureTemplates.value.length,
      bookmarkCount: workspaceBookmarks.value.length,
      auditCount: actionLog.value.length,
      createdAt: now,
      updatedAt: now,
    }
    workspaceProjects.value.unshift(project)
    workspaceProjects.value = workspaceProjects.value.slice(0, 50)
    saveWorkspaceProjects()
    addActionLog('workspace', t('workspaceSessionStore.projectSaved', { name: project.name }), t('workspaceSessionStore.projectCounts', { features: project.trainerFeatureCount, templates: project.structureTemplateCount }), 'success')
    return project
  }

  function loadWorkspaceProject(id: number) {
    const project = workspaceProjects.value.find((item) => item.id === id)
    if (!project) return { success: false, error: t('workspaceSessionStore.projectNotFound') }
    const result = importWorkspaceJson(project.snapshotJson)
    if (result.success === true) {
      addActionLog('workspace', t('workspaceSessionStore.projectLoaded', { name: project.name }), project.processName || '-', 'success')
      addInvestigationStep({
        title: t('workspaceSessionStore.workspaceProjectLoaded'),
        detail: t('workspaceSessionStore.projectLoadedDetail', { name: project.name, features: project.trainerFeatureCount, templates: project.structureTemplateCount, bookmarks: project.bookmarkCount, audits: project.auditCount ?? 0 }),
        status: 'success',
        tool: 'loadWorkspaceProject',
        risk: 'safe',
        payload: { projectId: project.id, projectName: project.name },
      })
    }
    return result
  }

  function deleteWorkspaceProject(id: number) {
    const before = workspaceProjects.value.length
    workspaceProjects.value = workspaceProjects.value.filter((item) => item.id !== id)
    if (workspaceProjects.value.length !== before) {
      saveWorkspaceProjects()
      addActionLog('workspace', t('workspaceSessionStore.projectDeleted'), `id=${id}`, 'warning')
    }
  }

  function clearWorkspaceProjects() {
    workspaceProjects.value = []
    saveWorkspaceProjects()
    addActionLog('workspace', t('workspaceSessionStore.projectsCleared'), t('workspaceSessionStore.allLocalProjectsRemoved'), 'warning')
  }

  // ---- Watch pointer chain (P1) : suit une chaine de pointeurs en live ----
  const watchedPointerChains = ref<WatchedPointerChain[]>([])
  let nextWatchedChainId = 1
  const watchedPointerChainsLiveEnabled = ref(false)
  let watchedPointerChainsLiveTimer: ReturnType<typeof setInterval> | null = null

  async function addWatchedPointerChain(chain: { module: string, baseOffset: string, offsets: string[] }, type = 'Int32', label = '') {
    const controller = backend.getController()
    if (!controller.resolvePointerChain) return null
    try {
      const resolve = await controller.resolvePointerChain(chain)
      if (!resolve.success || !resolve.finalAddress) {
        addActionLog('watch', t('workspaceSessionStore.chainNotResolved'), resolve.error ?? t('workspaceSessionStore.resolutionImpossible'), 'warning')
        return null
      }
      const normalized = resolve.finalAddress.replace(/^0x/i, '')
      const entry: WatchedPointerChain = {
        id: nextWatchedChainId++,
        label: label || t('workspaceSessionStore.defaultChainLabel', { id: nextWatchedChainId - 1 }),
        chain: { ...chain },
        type,
        finalAddress: normalized,
        value: '',
        previousValue: '',
        changed: false,
        error: '',
        updatedAt: new Date().toLocaleTimeString(),
      }
      watchedPointerChains.value.push(entry)
      await refreshWatchedPointerChain(entry.id)
      return entry
    } catch (e) {
      addActionLog('watch', t('workspaceSessionStore.addChainError'), String(e), 'error')
      return null
    }
  }

  async function refreshWatchedPointerChain(id: number): Promise<WatchedPointerChain | null> {
    const entry = watchedPointerChains.value.find((item) => item.id === id)
    if (!entry) return null
    const controller = backend.getController()
    let updated = entry
    try {
      if (controller.resolvePointerChain) {
        const resolve = await controller.resolvePointerChain(entry.chain)
        if (resolve.success && resolve.finalAddress) {
          entry.finalAddress = resolve.finalAddress.replace(/^0x/i, '')
        } else if (!resolve.success) {
          entry.error = resolve.error ?? t('workspaceSessionStore.resolutionImpossible')
        }
      }
      const d = deps()
      const preview = await d.readMemoryPreviewByMode(entry.finalAddress, d.valueTypeReadSize(entry.type))
      const value = d.decodeTypedPreviewValue(preview, entry.type)
      updated = {
        ...entry,
        previousValue: entry.value,
        value,
        changed: entry.value !== '' && value !== entry.value,
        error: preview.success ? '' : preview.error,
        updatedAt: new Date().toLocaleTimeString(),
      }
    } catch (e) {
      updated = { ...entry, error: String(e), updatedAt: new Date().toLocaleTimeString() }
    }
    watchedPointerChains.value = watchedPointerChains.value.map((item) => (item.id === id ? updated : item))
    return updated
  }

  async function refreshWatchedPointerChains() {
    for (const entry of watchedPointerChains.value.slice(0, 20)) {
      await refreshWatchedPointerChain(entry.id)
    }
  }

  function removeWatchedPointerChain(id: number) {
    watchedPointerChains.value = watchedPointerChains.value.filter((item) => item.id !== id)
  }

  function clearWatchedPointerChains() {
    watchedPointerChains.value = []
  }

  function setWatchedPointerChainsLiveEnabled(enabled: boolean) {
    watchedPointerChainsLiveEnabled.value = enabled
    if (watchedPointerChainsLiveTimer) {
      clearInterval(watchedPointerChainsLiveTimer)
      watchedPointerChainsLiveTimer = null
    }
    if (enabled) {
      void refreshWatchedPointerChains()
      watchedPointerChainsLiveTimer = setInterval(() => {
        void refreshWatchedPointerChains()
      }, 1000)
    }
    addActionLog(
      'watch',
      enabled ? t('workspaceSessionStore.liveWatchEnabled') : t('workspaceSessionStore.liveWatchStopped'),
      t('workspaceSessionStore.chainCount', { count: watchedPointerChains.value.length }),
      enabled ? 'success' : 'info',
    )
  }

  return {
    configureWorkspaceSessionContext,
    workspaceProjects,
    workspaceProjectIdCounter,
    saveWorkspaceProjects,
    loadWorkspaceProjects,
    exportWorkspaceJson,
    exportWorkspaceMarkdown,
    previewWorkspaceImport,
    importWorkspaceJson,
    saveCurrentWorkspaceProject,
    loadWorkspaceProject,
    deleteWorkspaceProject,
    clearWorkspaceProjects,
    watchedPointerChains,
    watchedPointerChainsLiveEnabled,
    addWatchedPointerChain,
    refreshWatchedPointerChain,
    refreshWatchedPointerChains,
    removeWatchedPointerChain,
    clearWatchedPointerChains,
    setWatchedPointerChainsLiveEnabled,
  }
})
