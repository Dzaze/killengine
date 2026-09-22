/**
 * KillEngine — validation pure de l'import workspace (UX-PIPE-7,
 * docs/PHASE_TRACKER.md, 18/09/2026).
 *
 * Corrige deux défauts démontrés en direct dans `workspaceSession.ts` :
 * (1) `previewWorkspaceImport` acceptait un objet `investigation.active`
 *     syntaxiquement présent mais structurellement incomplet (ex. sans
 *     `steps`), annonçant « Aperçu prêt » ; (2) `importWorkspaceJson`
 *     assignait `activeInvestigation.value` AVANT de lire `.steps.reduce(...)`,
 *     donc un import invalide plantait après avoir déjà corrompu l'état en
 *     mémoire (`Cannot read properties of undefined (reading 'reduce')`).
 *
 * Ce module ne fait AUCUNE mutation, n'importe ni Pinia ni Vue ni i18n --
 * seuls des `import type` (effacés à la compilation, aucun import runtime)
 * réutilisent les vraies formes de données plutôt que de les dupliquer.
 * Zéro dépendance runtime : testable en isolation via esbuild+node, même
 * patron que `scripts/test-trainer-dependencies.ps1`/`.mjs` pour
 * `trainerDependencies.ts`.
 *
 * Politique d'import partiel (à conserver) : une section ABSENTE du JSON
 * n'est jamais mutée (laissée telle quelle) ; une section PRÉSENTE mais
 * structurellement invalide fait échouer tout l'import (aucune section n'est
 * appliquée, y compris celles déjà validées) -- ni fusion partielle, ni
 * remplacement partiel. Une investigation présente mais incomplète est un
 * rejet, jamais assimilée à une section absente.
 */
import type { InvestigationRun, InvestigationStep } from './investigation'
import type { TrainerFeature } from './trainer'
import type { StructureTemplate, StructureTemplateField, WorkspaceBookmark } from './workspaceItems'
import type { UserActionLogEntry } from './actionLog'
import type { AppSettings } from '@/services/backend'

/** Versions de format acceptées par ce validateur -- `exportWorkspaceJson` n'écrit aujourd'hui que la version 1. */
export const WORKSPACE_IMPORT_SUPPORTED_VERSIONS: readonly number[] = [1]

/** Code d'erreur machine + section fautive ; le texte localisé FR/EN est construit par l'appelant (store/vue), pas ici. */
export interface WorkspaceImportValidationError {
  section: 'format' | 'version' | 'investigation' | 'trainer' | 'structures' | 'bookmarks' | 'audit' | 'settings'
  reason: string
  detail?: string
}

export interface ValidatedWorkspaceSettings {
  language?: 'fr' | 'en'
  defaultValueType?: string
  performanceMode?: AppSettings['performanceMode']
  modelEnabled?: boolean
  modelPath?: string
  modelThreads?: number
  scanMaxResults?: number
  unknownSnapshotMaxMb?: number
}

export interface ValidatedInvestigationSection { active: InvestigationRun | null, archive: InvestigationRun[] }
export interface ValidatedTrainerSection { features: TrainerFeature[] }
export interface ValidatedStructuresSection { templates: StructureTemplate[] }
export interface ValidatedBookmarksSection { items: WorkspaceBookmark[] }
export interface ValidatedAuditSection { entries: UserActionLogEntry[] }

/** Uniquement les sections réellement présentes dans le JSON source -- une clé absente ici signifie "ne pas toucher ce domaine". */
export interface ValidatedWorkspaceImport {
  version: number
  exportedAt: string
  lastPresetId: string
  investigation?: ValidatedInvestigationSection
  trainer?: ValidatedTrainerSection
  structures?: ValidatedStructuresSection
  bookmarks?: ValidatedBookmarksSection
  audit?: ValidatedAuditSection
  settings?: ValidatedWorkspaceSettings
}

export type WorkspaceImportValidationResult =
  | { success: true, data: ValidatedWorkspaceImport }
  | { success: false, error: WorkspaceImportValidationError }

function isPlainObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
}

function isNonEmptyString(value: unknown): value is string {
  return typeof value === 'string'
}

function isOneOf<T extends string>(value: unknown, allowed: readonly T[]): value is T {
  return typeof value === 'string' && (allowed as readonly string[]).includes(value)
}

// --- Investigation ----------------------------------------------------------

const INVESTIGATION_STATUSES = ['active', 'checkpoint', 'done', 'blocked'] as const
const INVESTIGATION_STEP_STATUSES = ['planned', 'running', 'success', 'warning', 'error', 'checkpoint'] as const

function isValidInvestigationStep(value: unknown): value is InvestigationStep {
  if (!isPlainObject(value)) return false
  return typeof value.id === 'number'
    && isNonEmptyString(value.time)
    && isNonEmptyString(value.title)
    && isNonEmptyString(value.detail)
    && isOneOf(value.status, INVESTIGATION_STEP_STATUSES)
}

function isValidInvestigationRun(value: unknown): value is InvestigationRun {
  if (!isPlainObject(value)) return false
  return typeof value.id === 'number'
    && isNonEmptyString(value.title)
    && isNonEmptyString(value.objective)
    && isNonEmptyString(value.processName)
    && isNonEmptyString(value.startedAt)
    && isNonEmptyString(value.updatedAt)
    && isOneOf(value.status, INVESTIGATION_STATUSES)
    && isNonEmptyString(value.summary)
    && Array.isArray(value.steps) && value.steps.every(isValidInvestigationStep)
    && Array.isArray(value.hypotheses)
    && Array.isArray(value.checkpoints)
}

function validateInvestigationSection(root: Record<string, unknown>): ValidatedInvestigationSection | WorkspaceImportValidationError {
  const section = root.investigation
  if (!isPlainObject(section)) {
    return { section: 'investigation', reason: 'section_not_object' }
  }
  let active: InvestigationRun | null = null
  if (section.active !== null && section.active !== undefined) {
    if (!isValidInvestigationRun(section.active)) {
      return { section: 'investigation', reason: 'invalid_active_investigation' }
    }
    active = section.active
  }
  let archive: InvestigationRun[] = []
  if (section.archive !== undefined) {
    if (!Array.isArray(section.archive) || !section.archive.every(isValidInvestigationRun)) {
      return { section: 'investigation', reason: 'invalid_archive_entry' }
    }
    archive = section.archive
  }
  return { active, archive }
}

// --- Trainer ------------------------------------------------------------

const TRAINER_ACTIONS = ['write', 'freeze_polling', 'freeze_breakpoint', 'patch', 'clr_write'] as const
const TRAINER_LOCATOR_KINDS = ['absolute', 'aob', 'pointer_chain', 'clr_field'] as const
const TRAINER_STATUSES = ['idle', 'active', 'error', 'ambiguous'] as const

function isValidTrainerFeature(value: unknown): value is TrainerFeature {
  if (!isPlainObject(value)) return false
  return typeof value.id === 'number'
    && isNonEmptyString(value.name)
    && isNonEmptyString(value.processName)
    && isOneOf(value.action, TRAINER_ACTIONS)
    && isOneOf(value.locatorKind, TRAINER_LOCATOR_KINDS)
    && isNonEmptyString(value.address)
    && isNonEmptyString(value.valueType)
    && isNonEmptyString(value.value)
    && typeof value.enabled === 'boolean'
    && isOneOf(value.status, TRAINER_STATUSES)
    && typeof value.lastError === 'string'
    && isNonEmptyString(value.createdAt)
    && isNonEmptyString(value.updatedAt)
}

function validateTrainerSection(root: Record<string, unknown>): ValidatedTrainerSection | WorkspaceImportValidationError {
  const section = root.trainer
  if (!isPlainObject(section)) {
    return { section: 'trainer', reason: 'section_not_object' }
  }
  if (!Array.isArray(section.features) || !section.features.every(isValidTrainerFeature)) {
    return { section: 'trainer', reason: 'invalid_feature_entry' }
  }
  return { features: section.features }
}

// --- Structures -----------------------------------------------------------

function isValidStructureField(value: unknown): value is StructureTemplateField {
  if (!isPlainObject(value)) return false
  return typeof value.offset === 'number'
    && isNonEmptyString(value.type)
    && typeof value.label === 'string'
    && typeof value.note === 'string'
    && typeof value.sampleValue === 'string'
}

function isValidStructureTemplate(value: unknown): value is StructureTemplate {
  if (!isPlainObject(value)) return false
  return typeof value.id === 'number'
    && isNonEmptyString(value.name)
    && isNonEmptyString(value.processName)
    && isNonEmptyString(value.baseAddress)
    && typeof value.size === 'number'
    && typeof value.fieldCount === 'number'
    && Array.isArray(value.fields) && value.fields.every(isValidStructureField)
    && isNonEmptyString(value.createdAt)
    && isNonEmptyString(value.updatedAt)
}

function validateStructuresSection(root: Record<string, unknown>): ValidatedStructuresSection | WorkspaceImportValidationError {
  const section = root.structures
  if (!isPlainObject(section)) {
    return { section: 'structures', reason: 'section_not_object' }
  }
  if (!Array.isArray(section.templates) || !section.templates.every(isValidStructureTemplate)) {
    return { section: 'structures', reason: 'invalid_template_entry' }
  }
  return { templates: section.templates }
}

// --- Bookmarks --------------------------------------------------------------

const BOOKMARK_KINDS = ['address', 'structure_field', 'aob', 'pointer', 'note'] as const

function isValidWorkspaceBookmark(value: unknown): value is WorkspaceBookmark {
  if (!isPlainObject(value)) return false
  return typeof value.id === 'number'
    && isOneOf(value.kind, BOOKMARK_KINDS)
    && isNonEmptyString(value.label)
    && isNonEmptyString(value.processName)
    && typeof value.note === 'string'
    && isNonEmptyString(value.createdAt)
    && isNonEmptyString(value.updatedAt)
}

function validateBookmarksSection(root: Record<string, unknown>): ValidatedBookmarksSection | WorkspaceImportValidationError {
  const section = root.bookmarks
  if (!isPlainObject(section)) {
    return { section: 'bookmarks', reason: 'section_not_object' }
  }
  if (!Array.isArray(section.items) || !section.items.every(isValidWorkspaceBookmark)) {
    return { section: 'bookmarks', reason: 'invalid_bookmark_entry' }
  }
  return { items: section.items }
}

// --- Audit ------------------------------------------------------------------

const AUDIT_STATUSES = ['info', 'success', 'warning', 'error'] as const

function isValidActionLogEntry(value: unknown): value is UserActionLogEntry {
  if (!isPlainObject(value)) return false
  return typeof value.id === 'number'
    && isNonEmptyString(value.time)
    && isNonEmptyString(value.kind)
    && isNonEmptyString(value.title)
    && typeof value.detail === 'string'
    && isOneOf(value.status, AUDIT_STATUSES)
}

function validateAuditSection(root: Record<string, unknown>): ValidatedAuditSection | WorkspaceImportValidationError {
  const section = root.audit
  if (!isPlainObject(section)) {
    return { section: 'audit', reason: 'section_not_object' }
  }
  if (!Array.isArray(section.entries) || !section.entries.every(isValidActionLogEntry)) {
    return { section: 'audit', reason: 'invalid_entry' }
  }
  return { entries: section.entries }
}

// --- Settings -----------------------------------------------------------

const PERFORMANCE_MODES = ['Auto', 'Eco', 'Normal', 'Performance', 'Max'] as const

function validateSettingsSection(root: Record<string, unknown>): ValidatedWorkspaceSettings | WorkspaceImportValidationError {
  const section = root.settings
  if (!isPlainObject(section)) {
    return { section: 'settings', reason: 'section_not_object' }
  }
  const result: ValidatedWorkspaceSettings = {}
  if (section.language !== undefined) {
    if (section.language !== 'fr' && section.language !== 'en') {
      return { section: 'settings', reason: 'invalid_field', detail: 'language' }
    }
    result.language = section.language
  }
  if (section.defaultValueType !== undefined) {
    if (typeof section.defaultValueType !== 'string') {
      return { section: 'settings', reason: 'invalid_field', detail: 'defaultValueType' }
    }
    result.defaultValueType = section.defaultValueType
  }
  if (section.performanceMode !== undefined) {
    if (!isOneOf(section.performanceMode, PERFORMANCE_MODES)) {
      return { section: 'settings', reason: 'invalid_field', detail: 'performanceMode' }
    }
    result.performanceMode = section.performanceMode
  }
  if (section.modelEnabled !== undefined) {
    if (typeof section.modelEnabled !== 'boolean') {
      return { section: 'settings', reason: 'invalid_field', detail: 'modelEnabled' }
    }
    result.modelEnabled = section.modelEnabled
  }
  if (section.modelPath !== undefined) {
    if (typeof section.modelPath !== 'string') {
      return { section: 'settings', reason: 'invalid_field', detail: 'modelPath' }
    }
    result.modelPath = section.modelPath
  }
  if (section.modelThreads !== undefined) {
    if (!Number.isFinite(Number(section.modelThreads))) {
      return { section: 'settings', reason: 'invalid_field', detail: 'modelThreads' }
    }
    result.modelThreads = Number(section.modelThreads)
  }
  if (section.scanMaxResults !== undefined) {
    if (!Number.isFinite(Number(section.scanMaxResults))) {
      return { section: 'settings', reason: 'invalid_field', detail: 'scanMaxResults' }
    }
    result.scanMaxResults = Number(section.scanMaxResults)
  }
  if (section.unknownSnapshotMaxMb !== undefined) {
    if (!Number.isFinite(Number(section.unknownSnapshotMaxMb))) {
      return { section: 'settings', reason: 'invalid_field', detail: 'unknownSnapshotMaxMb' }
    }
    result.unknownSnapshotMaxMb = Number(section.unknownSnapshotMaxMb)
  }
  return result
}

// --- Entry point --------------------------------------------------------

/**
 * Valide intégralement un export workspace JSON avant toute mutation de
 * store. Chaque section PRÉSENTE dans `root` est validée dans son
 * intégralité ; toute section échouant sa validation fait échouer tout
 * l'import (aucune mutation partielle n'est jamais produite par ce module,
 * qui ne mute d'ailleurs rien lui-même). Une section absente de `root`
 * n'apparaît pas dans `data` -- l'appelant ne doit alors toucher à rien pour
 * ce domaine.
 */
export function validateWorkspaceImport(raw: string): WorkspaceImportValidationResult {
  let parsed: unknown
  try {
    parsed = JSON.parse(raw)
  } catch (e) {
    return { success: false, error: { section: 'format', reason: 'json_syntax', detail: String(e) } }
  }
  if (!isPlainObject(parsed)) {
    return { success: false, error: { section: 'format', reason: 'not_object' } }
  }
  const root = parsed

  const version = Number(root.version)
  if (!WORKSPACE_IMPORT_SUPPORTED_VERSIONS.includes(version)) {
    return { success: false, error: { section: 'version', reason: 'unsupported_version', detail: String(root.version) } }
  }

  const data: ValidatedWorkspaceImport = {
    version,
    exportedAt: typeof root.exportedAt === 'string' ? root.exportedAt : '',
    lastPresetId: isPlainObject(root.workflowPresets) && typeof root.workflowPresets.lastPresetId === 'string'
      ? root.workflowPresets.lastPresetId
      : '',
  }

  if ('investigation' in root) {
    const result = validateInvestigationSection(root)
    if ('section' in result) return { success: false, error: result }
    data.investigation = result
  }
  if ('trainer' in root) {
    const result = validateTrainerSection(root)
    if ('section' in result) return { success: false, error: result }
    data.trainer = result
  }
  if ('structures' in root) {
    const result = validateStructuresSection(root)
    if ('section' in result) return { success: false, error: result }
    data.structures = result
  }
  if ('bookmarks' in root) {
    const result = validateBookmarksSection(root)
    if ('section' in result) return { success: false, error: result }
    data.bookmarks = result
  }
  if ('audit' in root) {
    const result = validateAuditSection(root)
    if ('section' in result) return { success: false, error: result }
    data.audit = result
  }
  if ('settings' in root) {
    const result = validateSettingsSection(root)
    if ('section' in result) return { success: false, error: result }
    data.settings = result
  }

  return { success: true, data }
}
