/**
 * KillEngine — store Workspace Items (extrait de app.ts, sous-ensemble
 * isolé du candidat S9 de docs/REFACTOR_ROADMAP.md, 29/08/2026).
 *
 * S9 complet ("Profils / Workspace") est à couplage haut, bidirectionnel
 * avec S7/S8 (voir roadmap) — mais les templates de structure et les
 * bookmarks workspace sont un CRUD localStorage pur, sans dépendance vers
 * ces autres domaines : seul le nom du process est lu (paramètre explicite,
 * même patron que `exportActionLogJson`), et `addActionLog` est importé
 * directement (fondation sûre, cf. `actionLog.ts`). Les fonctions qui
 * traversent vers d'autres domaines (`useWorkspaceBookmarkAsWriteTarget`,
 * `createTrainerFeatureFromBookmark`, `createWorkspaceBookmarkFromCheckpoint`)
 * restent dans `app.ts`, qui lit `workspaceBookmarks`/appelle
 * `addWorkspaceBookmark` via les wrappers de ce store.
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'
import { useActionLogStore } from './actionLog'

export interface StructureTemplateField {
  offset: number
  type: string
  label: string
  note: string
  sampleValue: string
  rawHex?: string
}

export interface StructureTemplate {
  id: number
  name: string
  processName: string
  baseAddress: string
  size: number
  fieldCount: number
  fields: StructureTemplateField[]
  createdAt: string
  updatedAt: string
}

export interface WorkspaceBookmark {
  id: number
  kind: 'address' | 'structure_field' | 'aob' | 'pointer' | 'note'
  label: string
  processName: string
  address?: string
  type?: string
  value?: string
  note: string
  payload?: Record<string, unknown>
  createdAt: string
  updatedAt: string
}

const structureTemplateStorageKey = 'killengine.structure.templates.v1'
const workspaceBookmarkStorageKey = 'killengine.workspace.bookmarks.v1'

export const useWorkspaceItemsStore = defineStore('workspaceItems', () => {
  const actionLogStore = useActionLogStore()

  const structureTemplates = ref<StructureTemplate[]>([])
  const structureTemplateIdCounter = ref(0)
  const workspaceBookmarks = ref<WorkspaceBookmark[]>([])
  const workspaceBookmarkIdCounter = ref(0)

  function saveStructureTemplates() {
    try {
      window.localStorage.setItem(structureTemplateStorageKey, JSON.stringify({
        templates: structureTemplates.value,
        id: structureTemplateIdCounter.value,
      }))
    } catch {
      // Best-effort persistence.
    }
  }

  function loadStructureTemplates() {
    try {
      const raw = window.localStorage.getItem(structureTemplateStorageKey)
      if (!raw) return
      const parsed = JSON.parse(raw) as { templates?: StructureTemplate[], id?: number }
      structureTemplates.value = Array.isArray(parsed.templates) ? parsed.templates.slice(0, 100) : []
      structureTemplateIdCounter.value = Number(parsed.id ?? 0)
    } catch {
      structureTemplates.value = []
      structureTemplateIdCounter.value = 0
    }
  }

  function saveStructureTemplate(input: {
    name?: string
    baseAddress: string
    size: number
    fields: StructureTemplateField[]
  }, processNameValue = '') {
    const fields = input.fields
      .filter((field) => Number.isFinite(field.offset) && field.type.trim())
      .slice(0, 256)
    if (fields.length === 0) {
      actionLogStore.addActionLog('structure', 'Template refusé', 'Aucun champ typé exploitable.', 'warning')
      return null
    }

    structureTemplateIdCounter.value += 1
    const now = new Date().toISOString()
    const template: StructureTemplate = {
      id: structureTemplateIdCounter.value,
      name: String(input.name ?? `Structure 0x${input.baseAddress}`).trim() || `Structure 0x${input.baseAddress}`,
      processName: processNameValue,
      baseAddress: input.baseAddress.replace(/^0x/i, '').toUpperCase(),
      size: Math.max(0, Math.round(input.size)),
      fieldCount: fields.length,
      fields,
      createdAt: now,
      updatedAt: now,
    }
    structureTemplates.value.unshift(template)
    structureTemplates.value = structureTemplates.value.slice(0, 100)
    saveStructureTemplates()
    actionLogStore.addActionLog('structure', `Template sauvegardé: ${template.name}`, `${template.fieldCount} champ(s).`, 'success')
    return template
  }

  function deleteStructureTemplate(id: number) {
    const before = structureTemplates.value.length
    structureTemplates.value = structureTemplates.value.filter((item) => item.id !== id)
    if (structureTemplates.value.length !== before) {
      saveStructureTemplates()
      actionLogStore.addActionLog('structure', 'Template supprimé', `id=${id}`, 'warning')
    }
  }

  function clearStructureTemplates() {
    structureTemplates.value = []
    saveStructureTemplates()
    actionLogStore.addActionLog('structure', 'Templates vidés', 'Tous les templates locaux ont été supprimés.', 'warning')
  }

  function saveWorkspaceBookmarks() {
    try {
      window.localStorage.setItem(workspaceBookmarkStorageKey, JSON.stringify({
        bookmarks: workspaceBookmarks.value,
        id: workspaceBookmarkIdCounter.value,
      }))
    } catch {
      // Best-effort persistence.
    }
  }

  function loadWorkspaceBookmarks() {
    try {
      const raw = window.localStorage.getItem(workspaceBookmarkStorageKey)
      if (!raw) return
      const parsed = JSON.parse(raw) as { bookmarks?: WorkspaceBookmark[], id?: number }
      workspaceBookmarks.value = Array.isArray(parsed.bookmarks) ? parsed.bookmarks.slice(0, 500) : []
      workspaceBookmarkIdCounter.value = Number(parsed.id ?? 0)
    } catch {
      workspaceBookmarks.value = []
      workspaceBookmarkIdCounter.value = 0
    }
  }

  function addWorkspaceBookmark(input: Partial<WorkspaceBookmark>, processNameValue = '') {
    workspaceBookmarkIdCounter.value += 1
    const now = new Date().toISOString()
    const bookmark: WorkspaceBookmark = {
      id: workspaceBookmarkIdCounter.value,
      kind: input.kind ?? 'address',
      label: String(input.label ?? input.address ?? 'Bookmark').trim() || 'Bookmark',
      processName: String(input.processName ?? processNameValue),
      address: input.address ? String(input.address).replace(/^0x/i, '').toUpperCase() : undefined,
      type: input.type ? String(input.type) : undefined,
      value: input.value ? String(input.value) : undefined,
      note: String(input.note ?? ''),
      payload: input.payload,
      createdAt: now,
      updatedAt: now,
    }
    workspaceBookmarks.value.unshift(bookmark)
    workspaceBookmarks.value = workspaceBookmarks.value.slice(0, 500)
    saveWorkspaceBookmarks()
    actionLogStore.addActionLog('workspace', `Bookmark ajouté: ${bookmark.label}`, bookmark.address ? `0x${bookmark.address}` : bookmark.note, 'success')
    return bookmark
  }

  function updateWorkspaceBookmark(id: number, input: Partial<WorkspaceBookmark>) {
    const bookmark = workspaceBookmarks.value.find((item) => item.id === id)
    if (!bookmark) return null
    bookmark.kind = input.kind ?? bookmark.kind
    bookmark.label = input.label !== undefined ? String(input.label).trim() || bookmark.label : bookmark.label
    bookmark.processName = input.processName !== undefined ? String(input.processName) : bookmark.processName
    bookmark.address = input.address !== undefined
      ? String(input.address).replace(/^0x/i, '').trim().toUpperCase() || undefined
      : bookmark.address
    bookmark.type = input.type !== undefined ? String(input.type).trim() || undefined : bookmark.type
    bookmark.value = input.value !== undefined ? String(input.value).trim() || undefined : bookmark.value
    bookmark.note = input.note !== undefined ? String(input.note) : bookmark.note
    bookmark.payload = input.payload !== undefined ? input.payload : bookmark.payload
    bookmark.updatedAt = new Date().toISOString()
    saveWorkspaceBookmarks()
    actionLogStore.addActionLog('workspace', `Bookmark modifié: ${bookmark.label}`, bookmark.address ? `0x${bookmark.address}` : bookmark.note, 'success')
    return bookmark
  }

  function deleteWorkspaceBookmark(id: number) {
    const before = workspaceBookmarks.value.length
    workspaceBookmarks.value = workspaceBookmarks.value.filter((item) => item.id !== id)
    if (workspaceBookmarks.value.length !== before) {
      saveWorkspaceBookmarks()
      actionLogStore.addActionLog('workspace', 'Bookmark supprimé', `id=${id}`, 'warning')
    }
  }

  function clearWorkspaceBookmarks() {
    workspaceBookmarks.value = []
    saveWorkspaceBookmarks()
    actionLogStore.addActionLog('workspace', 'Bookmarks vidés', 'Tous les bookmarks locaux ont été supprimés.', 'warning')
  }

  return {
    structureTemplates,
    structureTemplateIdCounter,
    workspaceBookmarks,
    workspaceBookmarkIdCounter,
    saveStructureTemplates,
    loadStructureTemplates,
    saveStructureTemplate,
    deleteStructureTemplate,
    clearStructureTemplates,
    saveWorkspaceBookmarks,
    loadWorkspaceBookmarks,
    addWorkspaceBookmark,
    updateWorkspaceBookmark,
    deleteWorkspaceBookmark,
    clearWorkspaceBookmarks,
  }
})
