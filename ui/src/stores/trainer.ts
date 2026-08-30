/**
 * KillEngine — store Trainer Features (extrait de app.ts, candidat S8 de
 * docs/REFACTOR_ROADMAP.md, PHASE 230, 30/08/2026).
 *
 * S8 était annoté couplage "Moyen-haut" avec S1 (CLR)/S9 (Profils/Session)
 * dans le roadmap — vérifié en pratique, une fois S1 (`clrInspector.ts`) et
 * S9a (`workspaceItems.ts`) déjà extraits en feuilles indépendantes, le
 * couplage réel restant est bien plus léger que redouté : ce store importe
 * directement `clrInspector.ts`/`workspaceItems.ts`/`scanning.ts`/
 * `writeFreeze.ts` (toutes des feuilles sans dépendance vers `./app`), et
 * seules 4 dépendances transversales vers des domaines pas encore extraits
 * restent : `processName` (Process), `confirmRiskAction` (RiskGate + audit),
 * `kernelMemoryModeActive`/`writeMemoryValueByMode` (mode kernel, Memory).
 * Injectées UNE SEULE FOIS via `configureTrainerContext`, même patron
 * "réglé une fois" que `configureWriteFreezeContext` (`writeFreeze.ts`,
 * PHASE 229) et `configureCandidateWatchNotifier` (`scanning.ts`, PHASE 224).
 *
 * Restent volontairement dans `app.ts` (pas ce candidat) : le pont
 * Session→Trainer (`sessionTrainerAction`/`sessionTrainerValue`/
 * `resolveSessionTrainerLocator`/`promoteSessionEntryToTrainer`/
 * `promoteSessionGroupToTrainer`, territoire Session — appellent
 * `createTrainerFeature` de ce store, dépendance à sens unique donc pas
 * circulaire), l'export/import Workspace complet (S9b, lit `trainerFeatures`
 * via ce store) et le dispatch chat/IA (`trainer_list_features`/
 * `trainer_create_write`/`trainer_delete_feature`, territoire S10).
 */
import { defineStore, storeToRefs } from 'pinia'
import { ref } from 'vue'
import {
  backend,
  type AobPatternQuality,
  type PointerChainInfo,
} from '@/services/backend'
import {
  resolveTrainerFeatureOrder,
  collectTrainerFeatureDependents,
  cleanupDependsOnAfterDelete,
  isToggleableTrainerAction,
} from './trainerDependencies'
import { useActionLogStore } from './actionLog'
import { useInvestigationStore } from './investigation'
import { useClrInspectorStore } from './clrInspector'
import { useScanningStore } from './scanning'
import { useWriteFreezeStore } from './writeFreeze'
import { useWorkspaceItemsStore } from './workspaceItems'

export interface TrainerFeature {
  id: number
  name: string
  processName: string
  action: 'write' | 'freeze_polling' | 'freeze_breakpoint' | 'patch' | 'clr_write'
  locatorKind: 'absolute' | 'aob' | 'pointer_chain' | 'clr_field'
  address: string
  valueType: string
  value: string
  patchBytes?: string
  aobPattern?: string
  /** PHASE 162 : base statique + offsets, re-resolue a chaque activation via resolvePointerChain — utile pour une adresse dans un objet alloue dynamiquement (reallouee a chaque partie), contrairement a 'absolute'. Voir docs/PHASE_TRACKER.md PHASE 162. */
  pointerChain?: PointerChainInfo
  clrTypeSubstring?: string
  clrIdentityField?: string
  clrIdentityValue?: string
  clrFieldName?: string
  signatureQuality?: AobPatternQuality
  signatureScore?: number
  signatureLevel?: string
  signatureWarning?: string
  signatureFixedBytes?: number
  signatureWildcardBytes?: number
  signatureUniqueFixedBytes?: number
  signatureFixedRatio?: number
  trainerSafe?: boolean
  signatureMatches?: number
  hotkey?: string
  hotkeyId?: number
  /** Ids d'autres TrainerFeature qui doivent être actives avant celle-ci (ex: "God Mode" dépend de "Infinite HP" + "Infinite Mana"). */
  dependsOn?: number[]
  enabled: boolean
  status: 'idle' | 'active' | 'error' | 'ambiguous'
  lastError: string
  history?: Array<{
    time: string
    action: string
    status: 'success' | 'warning' | 'error' | 'info'
    detail: string
  }>
  createdAt: string
  updatedAt: string
}

type RiskLevel = 'safe' | 'write' | 'debug' | 'patch' | 'injection'

export interface TrainerExternalDeps {
  processName: { readonly value: string }
  confirmRiskAction: (risk: RiskLevel, title: string, detail: string) => Promise<boolean>
  kernelMemoryModeActive: { readonly value: boolean }
  writeMemoryValueByMode: (address: string, type: string, value: string) => Promise<{ success: boolean, error?: string }>
}

export const useTrainerStore = defineStore('trainer', () => {
  const actionLogStore = useActionLogStore()
  const { addActionLog } = actionLogStore
  const investigationStore = useInvestigationStore()
  const { addInvestigationStep } = investigationStore
  const clrInspectorStore = useClrInspectorStore()
  const { clrFieldLocatorResult } = storeToRefs(clrInspectorStore)
  const scanningStore = useScanningStore()
  const { selectedCandidateAddress, exactScanType } = storeToRefs(scanningStore)
  const writeFreezeStore = useWriteFreezeStore()
  const { writeValue } = storeToRefs(writeFreezeStore)
  const workspaceItemsStore = useWorkspaceItemsStore()
  const { workspaceBookmarks } = storeToRefs(workspaceItemsStore)

  let externalDeps: TrainerExternalDeps | null = null
  function configureTrainerContext(newDeps: TrainerExternalDeps) {
    externalDeps = newDeps
  }
  function deps(): TrainerExternalDeps {
    if (!externalDeps) throw new Error('trainer store used before configureTrainerContext')
    return externalDeps
  }

  const trainerFeatures = ref<TrainerFeature[]>([])
  const trainerFeatureIdCounter = ref(0)
  const trainerBusy = ref(false)
  const trainerHotkeyStatus = ref('')
  const trainerOverlayVisible = ref(false)
  const trainerOverlayStatus = ref('')
  const trainerOverlayHotkey = ref('')
  const trainerOverlayHotkeyId = ref<number | undefined>(undefined)

  const trainerStorageKey = 'killengine.trainer.features.v1'
  const overlayHotkeyStorageKey = 'killengine.trainer.overlayHotkey.v1'

  function saveTrainerFeatures() {
    try {
      window.localStorage.setItem(trainerStorageKey, JSON.stringify({
        features: trainerFeatures.value,
        id: trainerFeatureIdCounter.value,
      }))
    } catch {
      // Best-effort persistence.
    }
  }

  function loadTrainerFeatures() {
    try {
      const raw = window.localStorage.getItem(trainerStorageKey)
      if (!raw) return
      const parsed = JSON.parse(raw) as { features?: TrainerFeature[], id?: number }
      trainerFeatures.value = Array.isArray(parsed.features) ? parsed.features : []
      trainerFeatureIdCounter.value = Number(parsed.id ?? 0)
    } catch {
      trainerFeatures.value = []
    }
  }

  function saveOverlayHotkey() {
    try {
      window.localStorage.setItem(overlayHotkeyStorageKey, trainerOverlayHotkey.value)
    } catch {
      // Best-effort persistence.
    }
  }

  function loadOverlayHotkey() {
    try {
      trainerOverlayHotkey.value = window.localStorage.getItem(overlayHotkeyStorageKey) ?? ''
    } catch {
      trainerOverlayHotkey.value = ''
    }
  }

  function createTrainerFeatureFromBookmark(id: number, action: TrainerFeature['action'] = 'write') {
    const bookmark = workspaceBookmarks.value.find((item) => item.id === id)
    if (!bookmark?.address) {
      addActionLog('trainer', 'Feature refusée', 'Bookmark sans adresse.', 'warning')
      return null
    }
    const patchBytes = String(bookmark.payload?.patchBytes ?? '').trim()
    const aobPattern = String(bookmark.payload?.aobPattern ?? '').trim()
    const signatureQuality = bookmark.payload?.signatureQuality as AobPatternQuality | undefined
    const inferredAction = action === 'patch' || patchBytes ? 'patch' : action
    const feature = createTrainerFeature({
      name: bookmark.label,
      processName: bookmark.processName || deps().processName.value,
      action: inferredAction,
      locatorKind: bookmark.kind === 'aob' || aobPattern ? 'aob' : 'absolute',
      address: bookmark.address,
      valueType: bookmark.type || exactScanType.value,
      value: bookmark.value ?? writeValue.value,
      patchBytes: patchBytes || undefined,
      aobPattern: aobPattern || undefined,
      signatureQuality,
      signatureScore: Number(bookmark.payload?.signatureScore ?? signatureQuality?.score ?? 0) || undefined,
      signatureLevel: String(bookmark.payload?.signatureLevel ?? signatureQuality?.level ?? ''),
      signatureWarning: String(bookmark.payload?.signatureWarning ?? signatureQuality?.warning ?? ''),
      signatureFixedBytes: Number(bookmark.payload?.signatureFixedBytes ?? signatureQuality?.fixedBytes ?? 0) || undefined,
      signatureWildcardBytes: Number(bookmark.payload?.signatureWildcardBytes ?? signatureQuality?.wildcardBytes ?? 0) || undefined,
      signatureUniqueFixedBytes: Number(bookmark.payload?.signatureUniqueFixedBytes ?? signatureQuality?.uniqueFixedBytes ?? 0) || undefined,
      signatureFixedRatio: Number(bookmark.payload?.signatureFixedRatio ?? signatureQuality?.fixedRatio ?? 0) || undefined,
      trainerSafe: Boolean(bookmark.payload?.trainerSafe ?? signatureQuality?.trainerSafe ?? false) || undefined,
      signatureMatches: Number(bookmark.payload?.signatureMatches ?? 0) || undefined,
    })
    if (feature) {
      addInvestigationStep({
        title: 'Feature Trainer créée depuis bookmark',
        detail: `${feature.name} · ${feature.action} · 0x${feature.address}`,
        status: 'success',
        tool: 'createTrainerFeatureFromBookmark',
        risk: feature.action === 'patch' ? 'patch' : 'safe',
        payload: { bookmarkId: bookmark.id, featureId: feature.id },
      })
    }
    return feature
  }

  async function registerTrainerFeatureHotkey(id: number, combo: string) {
    const feature = trainerFeatures.value.find((item) => item.id === id)
    const trimmed = combo.trim()
    if (!feature || !trimmed) return
    const controller = backend.getController()
    if (!controller.registerGlobalHotkey) {
      trainerHotkeyStatus.value = 'Hotkeys globales non exposées par ce backend.'
      addActionLog('hotkey', 'Hotkey indisponible', trainerHotkeyStatus.value, 'warning')
      return
    }
    if (feature.hotkeyId && controller.unregisterGlobalHotkey) {
      await controller.unregisterGlobalHotkey(feature.hotkeyId)
    }
    const type = feature.action === 'patch' ? 'toggle_patch' : feature.action.startsWith('freeze') ? 'toggle_freeze' : 'write_value'
    const result = await controller.registerGlobalHotkey(trimmed, {
      type,
      targetId: String(feature.id),
      label: feature.name,
      payload: { featureId: feature.id },
    })
    if (result.success === true) {
      feature.hotkey = String(result.combo ?? trimmed)
      feature.hotkeyId = Number(result.id)
      feature.updatedAt = new Date().toISOString()
      addTrainerFeatureHistory(feature, 'hotkey_register', 'success', feature.hotkey)
      trainerHotkeyStatus.value = `Hotkey enregistrée: ${feature.hotkey}`
      saveTrainerFeatures()
      addActionLog('hotkey', 'Hotkey Trainer enregistrée', `${feature.hotkey} -> ${feature.name}`, 'success')
    } else {
      trainerHotkeyStatus.value = String(result.error ?? 'Hotkey refusée.')
      addTrainerFeatureHistory(feature, 'hotkey_register', 'warning', trainerHotkeyStatus.value)
      saveTrainerFeatures()
      addActionLog('hotkey', 'Hotkey Trainer refusée', trainerHotkeyStatus.value, 'warning')
    }
  }

  async function unregisterTrainerFeatureHotkey(id: number) {
    const feature = trainerFeatures.value.find((item) => item.id === id)
    if (!feature?.hotkeyId) return
    const controller = backend.getController()
    if (controller.unregisterGlobalHotkey) {
      await controller.unregisterGlobalHotkey(feature.hotkeyId)
    }
    feature.hotkey = ''
    feature.hotkeyId = undefined
    feature.updatedAt = new Date().toISOString()
    addTrainerFeatureHistory(feature, 'hotkey_unregister', 'success', feature.name)
    saveTrainerFeatures()
    addActionLog('hotkey', 'Hotkey Trainer supprimée', feature.name, 'success')
  }

  async function handleGlobalHotkey(event: Record<string, unknown>) {
    if (event.type === 'toggle_overlay') {
      addActionLog('hotkey', 'Hotkey: Overlay', String(event.type ?? ''), 'info')
      await setTrainerOverlay(!trainerOverlayVisible.value)
      return
    }
    const featureId = Number(event.targetId ?? (event.payload as Record<string, unknown> | undefined)?.featureId)
    const feature = trainerFeatures.value.find((item) => item.id === featureId)
    if (!feature) return
    addActionLog('hotkey', `Hotkey: ${feature.name}`, String(event.type ?? ''), 'info')
    if (feature.enabled) {
      await restoreTrainerFeature(feature.id)
    } else {
      await applyTrainerFeature(feature.id)
    }
  }

  async function registerOverlayHotkey(combo: string) {
    const trimmed = combo.trim()
    if (!trimmed) return
    const controller = backend.getController()
    if (!controller.registerGlobalHotkey) {
      trainerOverlayStatus.value = 'Hotkeys globales non exposées par ce backend.'
      addActionLog('hotkey', 'Hotkey overlay indisponible', trainerOverlayStatus.value, 'warning')
      return
    }
    if (trainerOverlayHotkeyId.value && controller.unregisterGlobalHotkey) {
      await controller.unregisterGlobalHotkey(trainerOverlayHotkeyId.value)
    }
    const result = await controller.registerGlobalHotkey(trimmed, {
      type: 'toggle_overlay',
      label: 'Overlay Trainer',
    })
    if (result.success === true) {
      trainerOverlayHotkey.value = String(result.combo ?? trimmed)
      trainerOverlayHotkeyId.value = Number(result.id)
      trainerOverlayStatus.value = `Hotkey overlay enregistrée: ${trainerOverlayHotkey.value}`
      saveOverlayHotkey()
      addActionLog('hotkey', 'Hotkey overlay enregistrée', trainerOverlayStatus.value, 'success')
    } else {
      trainerOverlayStatus.value = String(result.error ?? 'Hotkey overlay refusée.')
      addActionLog('hotkey', 'Hotkey overlay refusée', trainerOverlayStatus.value, 'warning')
    }
  }

  async function unregisterOverlayHotkey() {
    const controller = backend.getController()
    if (trainerOverlayHotkeyId.value && controller.unregisterGlobalHotkey) {
      await controller.unregisterGlobalHotkey(trainerOverlayHotkeyId.value)
    }
    trainerOverlayHotkey.value = ''
    trainerOverlayHotkeyId.value = undefined
    saveOverlayHotkey()
    addActionLog('hotkey', 'Hotkey overlay supprimée', '', 'success')
  }

  async function reregisterPersistedHotkeys() {
    // Le gestionnaire de hotkeys cote backend (GlobalHotkeyManager) repart a
    // zero a chaque lancement de KillEngine — les hotkeyId persistes en
    // localStorage ne correspondent plus a rien. Sans ce re-enregistrement,
    // une hotkey configuree lors d'une session precedente semble toujours la
    // (visible dans l'UI) mais ne declenche plus rien tant que l'utilisateur
    // ne la reconfigure pas manuellement.
    for (const feature of trainerFeatures.value) {
      if (feature.hotkey) {
        await registerTrainerFeatureHotkey(feature.id, feature.hotkey)
      }
    }
    if (trainerOverlayHotkey.value) {
      await registerOverlayHotkey(trainerOverlayHotkey.value)
    }
  }

  async function refreshTrainerOverlay() {
    const controller = backend.getController()
    if (!trainerOverlayVisible.value || !controller.updateTrainerOverlay) return
    const result = await controller.updateTrainerOverlay({
      title: deps().processName.value ? `KillEngine Trainer - ${deps().processName.value}` : 'KillEngine Trainer',
      features: trainerFeatures.value.map((feature) => ({
        name: feature.name,
        action: feature.action,
        enabled: feature.enabled,
        status: feature.status,
        hotkey: feature.hotkey,
      })),
    })
    trainerOverlayStatus.value = result.success === true ? 'Overlay mis à jour.' : String(result.error ?? 'Overlay non mis à jour.')
  }

  async function setTrainerOverlay(visible: boolean) {
    const controller = backend.getController()
    if (!controller.setTrainerOverlayVisible) {
      trainerOverlayStatus.value = 'Overlay Trainer non exposé par ce backend.'
      addActionLog('overlay', 'Overlay indisponible', trainerOverlayStatus.value, 'warning')
      return
    }
    const result = await controller.setTrainerOverlayVisible(visible, { x: 24, y: 24, width: 340, height: 180 })
    trainerOverlayVisible.value = result.success === true ? visible : trainerOverlayVisible.value
    trainerOverlayStatus.value = result.success === true ? (visible ? 'Overlay affiché.' : 'Overlay masqué.') : String(result.error ?? 'Overlay refusé.')
    addActionLog('overlay', visible ? 'Overlay Trainer affiché' : 'Overlay Trainer masqué', trainerOverlayStatus.value, result.success === true ? 'success' : 'warning')
    if (visible) await refreshTrainerOverlay()
  }

  function addTrainerFeatureHistory(
    feature: TrainerFeature,
    action: string,
    status: NonNullable<TrainerFeature['history']>[number]['status'],
    detail = '',
  ) {
    feature.history = [
      {
        time: new Date().toISOString(),
        action,
        status,
        detail,
      },
      ...(feature.history ?? []),
    ].slice(0, 30)
  }

  function createTrainerFeature(input: Partial<TrainerFeature>) {
    const address = String(input.address ?? selectedCandidateAddress.value ?? '').replace(/^0x/i, '').trim()
    const isClrFeature = input.action === 'clr_write' || input.locatorKind === 'clr_field'
    if (!address && !isClrFeature) {
      addActionLog('trainer', 'Feature refusée', 'Adresse manquante.', 'warning')
      return null
    }
    if (isClrFeature && (!String(input.clrTypeSubstring ?? '').trim() || !String(input.clrIdentityField ?? '').trim() || !String(input.clrIdentityValue ?? '').trim() || !String(input.clrFieldName ?? '').trim())) {
      addActionLog('trainer', 'Feature CLR refusée', 'Locator CLR incomplet.', 'warning')
      return null
    }
    trainerFeatureIdCounter.value += 1
    const now = new Date().toISOString()
    const signatureQuality = input.signatureQuality
    const signatureScore = Number(signatureQuality?.score ?? input.signatureScore ?? 0)
    const signatureFixedBytes = Number(signatureQuality?.fixedBytes ?? input.signatureFixedBytes ?? 0)
    const signatureWildcardBytes = Number(signatureQuality?.wildcardBytes ?? input.signatureWildcardBytes ?? 0)
    const signatureUniqueFixedBytes = Number(signatureQuality?.uniqueFixedBytes ?? input.signatureUniqueFixedBytes ?? 0)
    const signatureFixedRatio = Number(signatureQuality?.fixedRatio ?? input.signatureFixedRatio ?? 0)
    const feature: TrainerFeature = {
      id: trainerFeatureIdCounter.value,
      name: String(input.name ?? (isClrFeature ? `CLR ${input.clrFieldName ?? 'field'}` : `Feature 0x${address}`)).trim() || (isClrFeature ? `CLR ${input.clrFieldName ?? 'field'}` : `Feature 0x${address}`),
      processName: String(input.processName ?? deps().processName.value),
      action: input.action ?? 'write',
      locatorKind: input.locatorKind ?? (isClrFeature ? 'clr_field' : 'absolute'),
      address,
      valueType: String(input.valueType ?? exactScanType.value ?? 'Int32'),
      value: String(input.value ?? writeValue.value ?? ''),
      patchBytes: input.patchBytes,
      aobPattern: input.aobPattern,
      // PHASE 163 : oubli de PHASE 162 corrige -- sans cette ligne, un appel
      // createTrainerFeature({locatorKind:'pointer_chain', pointerChain: {...}})
      // (ex. depuis le nouveau chemin Assistant trainer_create_write) perdait
      // silencieusement la chaine, laissant resolveTrainerFeatureAddress
      // echouer avec "Chaine de pointeurs manquante."
      pointerChain: input.pointerChain,
      clrTypeSubstring: input.clrTypeSubstring,
      clrIdentityField: input.clrIdentityField,
      clrIdentityValue: input.clrIdentityValue,
      clrFieldName: input.clrFieldName,
      signatureQuality,
      signatureScore: Number.isFinite(signatureScore) && signatureScore > 0 ? signatureScore : undefined,
      signatureLevel: signatureQuality?.level ?? input.signatureLevel,
      signatureWarning: signatureQuality?.warning ?? input.signatureWarning,
      signatureFixedBytes: Number.isFinite(signatureFixedBytes) && signatureFixedBytes > 0 ? signatureFixedBytes : undefined,
      signatureWildcardBytes: Number.isFinite(signatureWildcardBytes) && signatureWildcardBytes >= 0 ? signatureWildcardBytes : undefined,
      signatureUniqueFixedBytes: Number.isFinite(signatureUniqueFixedBytes) && signatureUniqueFixedBytes > 0 ? signatureUniqueFixedBytes : undefined,
      signatureFixedRatio: Number.isFinite(signatureFixedRatio) && signatureFixedRatio > 0 ? signatureFixedRatio : undefined,
      trainerSafe: signatureQuality?.trainerSafe ?? input.trainerSafe,
      signatureMatches: input.signatureMatches,
      hotkey: input.hotkey,
      hotkeyId: input.hotkeyId,
      dependsOn: Array.isArray(input.dependsOn) ? input.dependsOn.filter((id) => Number.isFinite(id)) : undefined,
      enabled: false,
      status: 'idle',
      lastError: '',
      history: [],
      createdAt: now,
      updatedAt: now,
    }
    const locationText = feature.locatorKind === 'clr_field'
      ? `${feature.clrTypeSubstring}.${feature.clrFieldName} via ${feature.clrIdentityField}=${feature.clrIdentityValue}`
      : feature.locatorKind === 'pointer_chain' && feature.pointerChain
        ? `${feature.pointerChain.module}+${feature.pointerChain.baseOffset}`
      : `0x${feature.address}`
    addTrainerFeatureHistory(feature, 'created', 'success', `${feature.action} ${locationText}`)
    trainerFeatures.value.unshift(feature)
    saveTrainerFeatures()
    void refreshTrainerOverlay()
    addActionLog('trainer', `Feature créée: ${feature.name}`, `${feature.action} ${locationText}.`, 'success')
    return feature
  }

  function createTrainerClrFieldFeature(input: {
    name?: string
    typeSubstring: string
    identityField: string
    identityValue: string
    targetField: string
    valueType: string
    value: string
    address?: string
  }) {
    return createTrainerFeature({
      name: input.name || `CLR ${input.targetField}`,
      action: 'clr_write',
      locatorKind: 'clr_field',
      address: String(input.address ?? '').replace(/^0x/i, '').trim(),
      valueType: input.valueType,
      value: input.value,
      clrTypeSubstring: input.typeSubstring,
      clrIdentityField: input.identityField,
      clrIdentityValue: input.identityValue,
      clrFieldName: input.targetField,
    })
  }

  function createTrainerFeatureFromCheckpoint(checkpoint: Record<string, unknown>) {
    const patchBytes = String(checkpoint.patchBytes ?? '').trim()
    const payload = (checkpoint.payload && typeof checkpoint.payload === 'object')
      ? checkpoint.payload as Record<string, unknown>
      : {}
    const signatureQuality = (checkpoint.signatureQuality ?? payload.signatureQuality) as AobPatternQuality | undefined
    return createTrainerFeature({
      name: String(checkpoint.label ?? checkpoint.name ?? checkpoint.address ?? 'Feature checkpoint'),
      action: patchBytes ? 'patch' : 'write',
      locatorKind: checkpoint.aobPattern ? 'aob' : 'absolute',
      address: String(checkpoint.address ?? ''),
      valueType: String(checkpoint.type ?? exactScanType.value),
      value: String(checkpoint.value ?? writeValue.value),
      patchBytes: patchBytes || undefined,
      aobPattern: String(checkpoint.aobPattern ?? '').trim() || undefined,
      signatureQuality,
      signatureScore: Number(checkpoint.signatureScore ?? payload.signatureScore ?? 0) || undefined,
      signatureLevel: String(checkpoint.signatureLevel ?? payload.signatureLevel ?? ''),
      signatureWarning: String(checkpoint.signatureWarning ?? payload.signatureWarning ?? ''),
      signatureFixedBytes: Number(checkpoint.signatureFixedBytes ?? payload.signatureFixedBytes ?? 0) || undefined,
      signatureWildcardBytes: Number(checkpoint.signatureWildcardBytes ?? payload.signatureWildcardBytes ?? 0) || undefined,
      signatureUniqueFixedBytes: Number(checkpoint.signatureUniqueFixedBytes ?? payload.signatureUniqueFixedBytes ?? 0) || undefined,
      signatureFixedRatio: Number(checkpoint.signatureFixedRatio ?? payload.signatureFixedRatio ?? 0) || undefined,
      trainerSafe: Boolean(checkpoint.trainerSafe ?? payload.trainerSafe ?? false) || undefined,
      signatureMatches: Number(checkpoint.signatureMatches ?? payload.signatureMatches ?? 0) || undefined,
    })
  }

  function trainerFeatureSignatureQuality(feature: TrainerFeature): AobPatternQuality | null {
    if (feature.signatureQuality) return feature.signatureQuality
    if (feature.signatureScore === undefined && !feature.signatureLevel) return null
    return {
      score: feature.signatureScore ?? 0,
      level: feature.signatureLevel || 'unknown',
      warning: feature.signatureWarning || '',
      fixedBytes: feature.signatureFixedBytes ?? 0,
      wildcardBytes: feature.signatureWildcardBytes ?? 0,
      uniqueFixedBytes: feature.signatureUniqueFixedBytes ?? 0,
      fixedRatio: feature.signatureFixedRatio ?? 0,
      trainerSafe: feature.trainerSafe ?? false,
    }
  }

  // PHASE 160 : ce garde-fou couvrait seulement 'patch' a l'origine ; generalise a
  // toute feature avec locatorKind 'aob' (freeze_polling/freeze_breakpoint/write inclus),
  // car resolveTrainerFeatureAddress ci-dessous re-resout desormais l'AOB pour ces actions
  // aussi, pas seulement pour patch.
  function trainerFeaturePatchBlockReason(feature: TrainerFeature): string {
    if (feature.action === 'patch' && !feature.patchBytes?.trim()) return 'Patch incomplet : bytes manquants.'
    if (feature.locatorKind === 'pointer_chain') {
      if (!feature.pointerChain || feature.pointerChain.offsets.length === 0) {
        return 'Chaîne de pointeurs manquante : génère-la avant d\'activer.'
      }
      return ''
    }
    if (feature.locatorKind !== 'aob') return ''
    if (!feature.aobPattern?.trim()) return 'AOB manquant : sauvegarde une signature stable avant activation.'
    const quality = trainerFeatureSignatureQuality(feature)
    if (!quality) return ''
    const score = Number(quality.score ?? 0)
    const fixedBytes = Number(quality.fixedBytes ?? 0)
    if (fixedBytes < 3 || score < 35) {
      return `AOB trop faible (${score}/100, ${fixedBytes} octet(s) fixe(s)).`
    }
    return ''
  }

  // PHASE 160 : renommee depuis resolveTrainerPatchAddress — re-resout desormais l'adresse
  // AOB pour n'importe quelle action Trainer (write/freeze_polling/freeze_breakpoint/patch),
  // pas seulement patch. Avant ce correctif, freeze_polling/freeze_breakpoint utilisaient
  // feature.address tel quel sans jamais re-scanner, donc un locator 'aob' ne servait a rien
  // pour ces actions (voir docs/PHASE_TRACKER.md PHASE 160).
  // PHASE 162 : resolution pointer chain — reutilise resolvePointerChain deja expose et
  // deja utilise par le panneau lecture-seule "Pointer Chain Watch" (addWatchedPointerChain).
  // Contrairement a l'AOB (signe du code/data statique), c'est la bonne technique pour une
  // adresse logee dans un objet alloue dynamiquement : une base stable (module + offset) est
  // retraversee via la chaine d'offsets a chaque activation, donc resiliente a la reallocation
  // d'une partie a l'autre (voir docs/PHASE_TRACKER.md PHASE 162, memoire vampire_survivors_health_freeze).
  async function resolveTrainerFeaturePointerChain(feature: TrainerFeature): Promise<{ address: string, error: string }> {
    if (!feature.pointerChain || feature.pointerChain.offsets.length === 0) {
      return { address: '', error: 'Chaîne de pointeurs manquante.' }
    }
    const controller = backend.getController()
    if (!controller.resolvePointerChain) {
      return { address: '', error: 'Résolution pointer chain non exposée par ce backend.' }
    }
    const resolve = await controller.resolvePointerChain(feature.pointerChain)
    if (!resolve.success || !resolve.finalAddress) {
      return { address: '', error: resolve.error || 'Résolution de la chaîne de pointeurs impossible.' }
    }
    return { address: String(resolve.finalAddress).replace(/^0x/i, '').toUpperCase(), error: '' }
  }

  async function resolveTrainerFeatureAddress(feature: TrainerFeature): Promise<{ address: string, error: string }> {
    if (feature.locatorKind === 'pointer_chain') {
      return resolveTrainerFeaturePointerChain(feature)
    }
    if (feature.locatorKind !== 'aob' || !feature.aobPattern?.trim()) {
      return { address: feature.address, error: '' }
    }
    const controller = backend.getController()
    if (!controller.scanAobPattern) {
      return { address: '', error: 'Scan AOB non expose par ce backend.' }
    }
    // PHASE 160 : executableOnly reste vrai pour 'patch' (cible forcement du code),
    // mais doit etre desactive pour write/freeze_polling/freeze_breakpoint — une donnee
    // (ex. un global statique comme g_health) vit en .data/.bss, pas dans une page
    // executable, et serait sinon filtree a tort par scanAobPattern (voir region.executable
    // dans ApplicationController). imageOnly reste vrai dans tous les cas : le locator AOB
    // suppose une adresse statique dans l'image du module, executable ou non.
    const scan = await controller.scanAobPattern(feature.aobPattern, {
      executableOnly: feature.action === 'patch',
      imageOnly: true,
      maxResults: 2,
    })
    if (scan.signatureQuality) {
      feature.signatureQuality = scan.signatureQuality
      feature.signatureScore = scan.signatureQuality.score
      feature.signatureLevel = scan.signatureQuality.level
      feature.signatureWarning = scan.signatureQuality.warning
      feature.signatureFixedBytes = scan.signatureQuality.fixedBytes
      feature.signatureWildcardBytes = scan.signatureQuality.wildcardBytes
      feature.signatureUniqueFixedBytes = scan.signatureQuality.uniqueFixedBytes
      feature.signatureFixedRatio = scan.signatureQuality.fixedRatio
      feature.trainerSafe = scan.signatureQuality.trainerSafe
    }
    feature.signatureMatches = Number(scan.matchesFound ?? scan.matches?.length ?? 0)
    if (scan.success !== true) {
      return { address: '', error: scan.error || 'Resolution AOB impossible.' }
    }
    if (feature.signatureMatches !== 1 || !scan.matches?.[0]?.address) {
      return { address: '', error: `AOB non unique (${feature.signatureMatches} match(es)). Regénère une signature plus spécifique.` }
    }
    return { address: String(scan.matches[0].address).replace(/^0x/i, '').toUpperCase(), error: '' }
  }

  async function doApplyTrainerFeature(id: number): Promise<boolean> {
    const feature = trainerFeatures.value.find((item) => item.id === id)
    if (!feature) return false
    if (feature.enabled) return true
    const blocked = trainerFeaturePatchBlockReason(feature)
    if (blocked) {
      feature.status = 'error'
      feature.lastError = blocked
      feature.updatedAt = new Date().toISOString()
      addTrainerFeatureHistory(feature, 'apply_blocked', 'warning', blocked)
      addActionLog('trainer', `Feature bloquée: ${feature.name}`, blocked, 'warning')
      return false
    }
    // PHASE 160 : re-resoudre l'adresse AVANT confirmRiskAction (pas seulement pour 'patch'
    // comme avant), pour que le dialogue de confirmation montre l'adresse reellement ciblee
    // et que freeze_polling/freeze_breakpoint/write beneficient aussi d'un locator 'aob' a jour.
    // clr_write garde sa propre resolution (findClrObjectsByFieldValue), non touchee ici.
    let resolvedAddress = feature.address
    if (feature.action !== 'clr_write') {
      const resolved = await resolveTrainerFeatureAddress(feature)
      if (resolved.error) {
        feature.status = 'error'
        feature.lastError = resolved.error
        feature.updatedAt = new Date().toISOString()
        addTrainerFeatureHistory(feature, 'apply_blocked', 'error', resolved.error)
        addActionLog('trainer', `Feature bloquée: ${feature.name}`, resolved.error, 'error')
        return false
      }
      resolvedAddress = resolved.address
      feature.address = resolvedAddress
    }
    const kernelActive = deps().kernelMemoryModeActive.value === true
    const trainerRisk: RiskLevel = feature.action === 'patch' ? 'patch' : (feature.action === 'write' && kernelActive ? 'injection' : 'write')
    const riskDetail = feature.locatorKind === 'clr_field'
      ? `${feature.action} ${feature.clrTypeSubstring}.${feature.clrFieldName} via ${feature.clrIdentityField}=${feature.clrIdentityValue} -> ${feature.value}`
      : `${feature.action} 0x${resolvedAddress} ${feature.valueType} ${feature.value || feature.patchBytes || ''}${feature.action === 'write' && kernelActive ? ' via driver kernel' : ''}`
    if (!await deps().confirmRiskAction(trainerRisk, `Activer feature Trainer: ${feature.name}`, riskDetail)) return false

    try {
      const controller = backend.getController()
      let ok = false
      let error = ''
      if (feature.action === 'clr_write') {
        if (!feature.clrTypeSubstring || !feature.clrIdentityField || !feature.clrIdentityValue || !feature.clrFieldName) {
          error = 'Locator CLR incomplet.'
        } else {
          const locator = await clrInspectorStore.findClrObjectsByFieldValue(feature.clrTypeSubstring, feature.clrIdentityField, feature.clrIdentityValue, 1)
          const match = clrFieldLocatorResult.value?.matches?.[0]
          if (!locator?.success || !match?.address) {
            error = locator?.error ?? 'Objet CLR introuvable.'
          } else if (!controller.writeClrPrimitiveField) {
            error = 'writePrimitiveField non exposé par ce backend.'
          } else {
            const write = await controller.writeClrPrimitiveField(match.address, feature.clrFieldName, feature.value)
            ok = write?.success === true
            error = write?.error ?? ''
            feature.address = String(match.address).replace(/^0x/i, '')
          }
        }
      } else if (feature.action === 'write') {
        const result = await deps().writeMemoryValueByMode(resolvedAddress, feature.valueType, feature.value)
        ok = result.success === true
        error = result.error ?? ''
      } else if (feature.action === 'freeze_polling') {
        const result = await controller.setFreezeValue(resolvedAddress, feature.valueType, feature.value, true)
        ok = result.success === true
        error = result.error ?? ''
      } else if (feature.action === 'freeze_breakpoint') {
        if (!controller.freezeWithBreakpoint) {
          error = 'Freeze BP non expose par ce backend.'
        } else {
          const result = await controller.freezeWithBreakpoint(resolvedAddress, feature.valueType, feature.value, { mode: 'rewrite' })
          ok = result.success === true
          error = result.error ?? ''
        }
      } else if (feature.action === 'patch') {
        if (!controller.applyCodePatch) {
          error = 'Patch code non expose par ce backend.'
        } else {
          const result = await controller.applyCodePatch(resolvedAddress, feature.patchBytes ?? '', { verify: true })
          ok = result.success === true
          error = result.error ?? ''
        }
      }
      feature.enabled = ok && isToggleableTrainerAction(feature.action)
      feature.status = ok ? (feature.action === 'write' || feature.action === 'clr_write' ? 'idle' : 'active') : 'error'
      feature.lastError = error
      feature.updatedAt = new Date().toISOString()
      const detail = feature.locatorKind === 'clr_field'
        ? (error || `${feature.clrFieldName} @ 0x${feature.address}`)
        : (error || `0x${feature.address}`)
      addTrainerFeatureHistory(feature, 'apply', ok ? 'success' : 'error', detail)
      addActionLog('trainer', ok ? `Feature activée: ${feature.name}` : `Feature échouée: ${feature.name}`, detail, ok ? 'success' : 'error')
      return ok
    } catch (e) {
      feature.status = 'error'
      feature.lastError = String(e)
      feature.updatedAt = new Date().toISOString()
      addTrainerFeatureHistory(feature, 'apply', 'error', String(e))
      addActionLog('trainer', `Feature échouée: ${feature.name}`, String(e), 'error')
      return false
    }
  }

  // PHASE 162 : genere une chaine de pointeurs pour une TrainerFeature a partir de son
  // adresse actuelle (typiquement une adresse absolue confirmee par scan, ex. le cas
  // Vampire Survivors ou l'adresse se realloue a chaque partie — voir memoire
  // vampire_survivors_health_freeze). Reutilise scanPointerChains, deja expose cote backend
  // et deja utilise par le panneau lecture-seule Pointer Chain Watch (addWatchedPointerChain).
  async function generateTrainerFeaturePointerChain(id: number, options?: { maxDepth?: number, maxResults?: number }): Promise<boolean> {
    const feature = trainerFeatures.value.find((item) => item.id === id)
    if (!feature) return false
    const controller = backend.getController()
    if (!controller.scanPointerChains) {
      addActionLog('trainer', `Chaîne de pointeurs indisponible: ${feature.name}`, 'scanPointerChains non exposé par ce backend.', 'warning')
      return false
    }
    const scan = await controller.scanPointerChains(feature.address, {
      maxDepth: options?.maxDepth ?? 3,
      maxResults: options?.maxResults ?? 5,
      onlyModuleBase: true,
    })
    if (!scan.success || !scan.chains || scan.chains.length === 0) {
      addActionLog('trainer', `Chaîne de pointeurs introuvable: ${feature.name}`, scan.error || 'Aucune chaîne stable trouvée pour cette adresse.', 'warning')
      return false
    }
    const best = scan.chains[0]
    feature.pointerChain = { module: best.module, baseOffset: best.baseOffset, offsets: best.offsets }
    feature.locatorKind = 'pointer_chain'
    feature.updatedAt = new Date().toISOString()
    addTrainerFeatureHistory(feature, 'pointer_chain_generated', 'success', `${best.module}+${best.baseOffset} → [${best.offsets.join(', ')}]`)
    addActionLog('trainer', `Chaîne de pointeurs générée: ${feature.name}`, `${best.module}+${best.baseOffset} → [${best.offsets.join(', ')}] (${scan.chains.length} candidate(s))`, 'success')
    saveTrainerFeatures()
    return true
  }

  async function applyTrainerFeature(id: number) {
    const feature = trainerFeatures.value.find((item) => item.id === id)
    if (!feature || trainerBusy.value) return
    const resolved = resolveTrainerFeatureOrder(trainerFeatures.value, [id])
    if (!resolved.success) {
      addActionLog('trainer', 'Activation interrompue', resolved.error ?? 'Ordre de dépendances invalide.', 'error')
      return
    }

    trainerBusy.value = true
    try {
      for (const orderedId of resolved.order) {
        const orderedFeature = trainerFeatures.value.find((item) => item.id === orderedId)
        if (!orderedFeature || orderedFeature.enabled) continue
        const ok = await doApplyTrainerFeature(orderedId)
        if (!ok) break
      }
    } finally {
      trainerBusy.value = false
      saveTrainerFeatures()
      void refreshTrainerOverlay()
    }
  }

  async function doRestoreTrainerFeature(id: number): Promise<boolean> {
    const feature = trainerFeatures.value.find((item) => item.id === id)
    if (!feature) return false
    if (!feature.enabled) return true
    if (!await deps().confirmRiskAction(feature.action === 'patch' ? 'patch' : 'write', `Restaurer feature Trainer: ${feature.name}`, `${feature.action} 0x${feature.address}.`)) return false

    try {
      const controller = backend.getController()
      let ok = true
      let error = ''
      if (feature.action === 'freeze_polling') {
        const result = await controller.setFreezeValue(feature.address, feature.valueType, feature.value, false)
        ok = result.success === true
        error = result.error ?? ''
      } else if (feature.action === 'freeze_breakpoint') {
        if (controller.stopBreakpointFreeze) {
          const result = await controller.stopBreakpointFreeze()
          ok = result.success === true
          error = result.error ?? ''
        }
      } else if (feature.action === 'patch') {
        if (controller.restoreCodePatch) {
          const result = await controller.restoreCodePatch(feature.address)
          ok = result.success === true
          error = result.error ?? ''
        } else {
          ok = false
          error = 'Restore patch non expose par ce backend.'
        }
      }
      feature.enabled = false
      feature.status = ok ? 'idle' : 'error'
      feature.lastError = error
      feature.updatedAt = new Date().toISOString()
      addTrainerFeatureHistory(feature, 'restore', ok ? 'success' : 'warning', error || `0x${feature.address}`)
      addActionLog('trainer', ok ? `Feature restaurée: ${feature.name}` : `Restauration échouée: ${feature.name}`, error || `0x${feature.address}`, ok ? 'success' : 'warning')
      return ok
    } catch (e) {
      feature.status = 'error'
      feature.lastError = String(e)
      feature.updatedAt = new Date().toISOString()
      addTrainerFeatureHistory(feature, 'restore', 'error', String(e))
      addActionLog('trainer', `Restauration échouée: ${feature.name}`, String(e), 'error')
      return false
    }
  }

  async function restoreTrainerFeature(id: number) {
    const feature = trainerFeatures.value.find((item) => item.id === id)
    if (!feature || trainerBusy.value) return
    const restoreIds = collectTrainerFeatureDependents(trainerFeatures.value, [id]).filter((featureId) => trainerFeatures.value.some((item) => item.id === featureId && item.enabled))
    const resolved = resolveTrainerFeatureOrder(trainerFeatures.value, restoreIds)
    if (!resolved.success) {
      addActionLog('trainer', 'Restauration interrompue', resolved.error ?? 'Ordre de dépendances invalide.', 'error')
      return
    }
    const restoreSet = new Set(restoreIds)

    trainerBusy.value = true
    try {
      for (const orderedId of [...resolved.order].reverse()) {
        if (!restoreSet.has(orderedId)) continue
        const orderedFeature = trainerFeatures.value.find((item) => item.id === orderedId)
        if (!orderedFeature || !orderedFeature.enabled) continue
        const ok = await doRestoreTrainerFeature(orderedId)
        if (!ok) break
      }
    } finally {
      trainerBusy.value = false
      saveTrainerFeatures()
      void refreshTrainerOverlay()
    }
  }

  async function saveTrainerFeatureToProfile(id: number) {
    const feature = trainerFeatures.value.find((item) => item.id === id)
    if (!feature) return
    const controller = backend.getController()
    const profileName = (feature.processName || deps().processName.value || 'KillEngineTrainer')
      .replace(/\.[^.]+$/, '')
      .replace(/[^a-z0-9_.-]+/gi, '_')
      .slice(0, 80) || 'KillEngineTrainer'
    try {
      let result: Record<string, unknown>
      if (feature.action === 'clr_write') {
        if (!controller.saveClrFieldProfileTarget) {
          throw new Error('Sauvegarde profil CLR non exposee par ce backend.')
        }
        if (!feature.clrTypeSubstring || !feature.clrIdentityField || !feature.clrIdentityValue || !feature.clrFieldName) {
          throw new Error('Locator CLR incomplet.')
        }
        result = await controller.saveClrFieldProfileTarget(
          profileName,
          feature.name,
          feature.clrTypeSubstring,
          feature.clrIdentityField,
          feature.clrIdentityValue,
          feature.clrFieldName,
          feature.valueType,
          `Trainer CLR ${feature.clrFieldName} = ${feature.value}`,
        )
      } else if (feature.action === 'patch') {
        const blocked = trainerFeaturePatchBlockReason(feature)
        if (blocked) {
          feature.status = 'error'
          feature.lastError = blocked
          feature.updatedAt = new Date().toISOString()
          addTrainerFeatureHistory(feature, 'save_profile_blocked', 'warning', blocked)
          saveTrainerFeatures()
          addActionLog('trainer', `Sauvegarde profil bloquée: ${feature.name}`, blocked, 'warning')
          return
        }
        if (!controller.saveProfileCodePatch) {
          throw new Error('Sauvegarde patch profil non exposee par ce backend.')
        }
        result = await controller.saveProfileCodePatch(
          profileName,
          feature.name,
          feature.address,
          feature.aobPattern ?? '',
          feature.patchBytes ?? '',
          {
            source: 'TrainerView',
            action: feature.action,
            valueType: feature.valueType,
            createdAt: feature.createdAt,
            signatureQuality: feature.signatureQuality,
            signatureScore: feature.signatureScore,
            signatureLevel: feature.signatureLevel,
            signatureWarning: feature.signatureWarning,
            signatureFixedBytes: feature.signatureFixedBytes,
            signatureWildcardBytes: feature.signatureWildcardBytes,
            signatureUniqueFixedBytes: feature.signatureUniqueFixedBytes,
            signatureFixedRatio: feature.signatureFixedRatio,
            trainerSafe: feature.trainerSafe,
            signatureMatches: feature.signatureMatches,
          },
        )
      } else if (feature.locatorKind === 'pointer_chain' && feature.pointerChain) {
        if (!controller.savePointerChainProfileTarget) {
          throw new Error('Sauvegarde pointer chain non exposee par ce backend.')
        }
        result = await controller.savePointerChainProfileTarget(
          profileName,
          feature.name,
          feature.pointerChain,
          feature.valueType,
          `Trainer ${feature.action} = ${feature.value}`,
        )
      } else {
        result = await controller.saveProfileTarget(
          profileName,
          feature.name,
          feature.address,
          feature.valueType,
          `Trainer ${feature.action} = ${feature.value}`,
        )
      }
      if (result.success !== false && feature.action !== 'patch') {
        const dependencyNames = (feature.dependsOn ?? [])
          .map((dependencyId) => trainerFeatures.value.find((item) => item.id === dependencyId)?.name?.trim() ?? '')
          .filter((dependencyName, index, all) => dependencyName.length > 0 && all.indexOf(dependencyName) === index)
        if (!controller.setProfileTargetDependencies) {
          if (dependencyNames.length > 0) {
            throw new Error('Persistance des dépendances Trainer non exposee par ce backend.')
          }
        } else {
          const dependencyResult = await controller.setProfileTargetDependencies(profileName, feature.name, dependencyNames)
          if (dependencyResult.success === false) {
            result = {
              ...result,
              success: false,
              error: `Cible sauvegardee, dependances non sauvegardees : ${String(dependencyResult.error ?? 'erreur inconnue')}`,
            }
          }
        }
      }
      feature.updatedAt = new Date().toISOString()
      feature.lastError = result.success === false ? String(result.error ?? 'Sauvegarde profil echouee.') : ''
      if (result.success === false) feature.status = 'error'
      addTrainerFeatureHistory(feature, 'save_profile', result.success === false ? 'warning' : 'success', `${profileName} · ${feature.lastError || 'OK'}`)
      saveTrainerFeatures()
      addActionLog('trainer', `Profil sauvegarde: ${feature.name}`, `${profileName} · ${feature.lastError || 'OK'}`, result.success === false ? 'warning' : 'success')
    } catch (e) {
      feature.status = 'error'
      feature.lastError = String(e)
      feature.updatedAt = new Date().toISOString()
      addTrainerFeatureHistory(feature, 'save_profile', 'error', String(e))
      saveTrainerFeatures()
      addActionLog('trainer', `Sauvegarde profil echouee: ${feature.name}`, String(e), 'error')
    }
  }

  async function applyAllTrainerFeatures() {
    const targetIds = trainerFeatures.value.filter((feature) => !feature.enabled).map((feature) => feature.id)
    if (targetIds.length === 0) return
    const resolved = resolveTrainerFeatureOrder(trainerFeatures.value, targetIds)
    if (!resolved.success) {
      addActionLog('trainer', 'Apply all interrompu', resolved.error ?? 'Ordre de dépendances invalide.', 'error')
      return
    }
    for (const id of resolved.order) {
      const feature = trainerFeatures.value.find((item) => item.id === id)
      if (feature && !feature.enabled) {
        trainerBusy.value = true
        try {
          const ok = await doApplyTrainerFeature(id)
          if (!ok) break
        } finally {
          trainerBusy.value = false
          saveTrainerFeatures()
          void refreshTrainerOverlay()
        }
      }
    }
  }

  async function restoreAllTrainerFeatures() {
    const targetIds = trainerFeatures.value.filter((feature) => feature.enabled).map((feature) => feature.id)
    if (targetIds.length === 0) return
    const resolved = resolveTrainerFeatureOrder(trainerFeatures.value, targetIds)
    if (!resolved.success) {
      addActionLog('trainer', 'Restore all interrompu', resolved.error ?? 'Ordre de dépendances invalide.', 'error')
      return
    }
    for (const id of [...resolved.order].reverse()) {
      const feature = trainerFeatures.value.find((item) => item.id === id)
      if (feature && feature.enabled) {
        trainerBusy.value = true
        try {
          const ok = await doRestoreTrainerFeature(id)
          if (!ok) break
        } finally {
          trainerBusy.value = false
          saveTrainerFeatures()
          void refreshTrainerOverlay()
        }
      }
    }
  }

  // Lecture seule, sans effet de bord : utilisee par le pont d'automatisation
  // (callVueStoreAction) pour verifier l'etat reel des features apres un
  // apply/restore async dont la valeur de retour n'est pas exploitable par
  // pipe (une fonction async retourne une Promise a la dispatch synchrone).
  function getTrainerFeaturesSnapshot() {
    return trainerFeatures.value.map((feature) => ({
      id: feature.id,
      name: feature.name,
      enabled: feature.enabled,
      status: feature.status,
      lastError: feature.lastError,
      dependsOn: feature.dependsOn ?? [],
    }))
  }

  function deleteTrainerFeature(id: number) {
    const remaining = trainerFeatures.value.filter((item) => item.id !== id)
    const now = new Date().toISOString()
    const touchedIds = new Set(remaining.filter((item) => item.dependsOn?.includes(id)).map((item) => item.id))
    trainerFeatures.value = cleanupDependsOnAfterDelete(remaining, id).map((feature) => (
      touchedIds.has(feature.id) ? { ...feature, updatedAt: now } : feature
    ))
    saveTrainerFeatures()
    void refreshTrainerOverlay()
  }

  function updateTrainerFeatureDependencies(id: number, dependencyIds: number[]) {
    const feature = trainerFeatures.value.find((item) => item.id === id)
    if (!feature) {
      return { success: false, error: 'Feature introuvable.' }
    }
    const cleanDependencies = dependencyIds
      .filter((dependencyId) => Number.isFinite(dependencyId) && dependencyId !== id)
      .filter((dependencyId, index, all) => all.indexOf(dependencyId) === index)
    const existingIds = new Set(trainerFeatures.value.map((item) => item.id))
    const missing = cleanDependencies.filter((dependencyId) => !existingIds.has(dependencyId))
    if (missing.length > 0) {
      return { success: false, error: `Dépendance introuvable : ${missing.join(', ')}.` }
    }

    const candidateFeatures = trainerFeatures.value.map((item) => (
      item.id === id
        ? { ...item, dependsOn: cleanDependencies.length > 0 ? cleanDependencies : undefined }
        : item
    ))
    const orderCheck = resolveTrainerFeatureOrder(candidateFeatures, candidateFeatures.map((item) => item.id))
    if (!orderCheck.success) {
      addActionLog('trainer', `Dépendances refusées: ${feature.name}`, orderCheck.error ?? 'Cycle détecté.', 'warning')
      return { success: false, error: orderCheck.error ?? 'Cycle de dépendances détecté.' }
    }

    feature.dependsOn = cleanDependencies.length > 0 ? cleanDependencies : undefined
    feature.updatedAt = new Date().toISOString()
    const detail = cleanDependencies.length > 0
      ? cleanDependencies.map((dependencyId) => trainerFeatures.value.find((item) => item.id === dependencyId)?.name ?? `#${dependencyId}`).join(', ')
      : 'aucune dépendance'
    addTrainerFeatureHistory(feature, 'dependencies_update', 'info', detail)
    saveTrainerFeatures()
    void refreshTrainerOverlay()
    return { success: true, dependsOn: cleanDependencies }
  }

  function clearTrainerFeatures() {
    trainerFeatures.value = []
    saveTrainerFeatures()
    void refreshTrainerOverlay()
    addActionLog('trainer', 'Trainer vidé', 'Toutes les features locales ont été supprimées.', 'warning')
  }

  function exportTrainerFeaturesJson(): string {
    return JSON.stringify({
      version: 1,
      exportedAt: new Date().toISOString(),
      processName: deps().processName.value,
      features: trainerFeatures.value,
    }, null, 2)
  }

  function exportTrainerFeaturesMarkdown(): string {
    const lines = [
      '# Trainer Features',
      '',
      `Export: ${new Date().toISOString()}`,
      `Processus: ${deps().processName.value || 'non attache'}`,
      `Features: ${trainerFeatures.value.length}`,
      '',
    ]
    if (trainerFeatures.value.length === 0) {
      lines.push('Aucune feature Trainer locale.')
      return lines.join('\n')
    }

    for (const feature of trainerFeatures.value) {
      lines.push(
        `## ${feature.name}`,
        '',
        `- Action: ${feature.action}`,
        `- Statut: ${feature.status}${feature.enabled ? ' / active' : ''}`,
        `- Processus: ${feature.processName || '-'}`,
        `- Locator: ${feature.locatorKind}`,
        `- Adresse: ${feature.address ? `0x${feature.address}` : '-'}`,
        feature.locatorKind === 'clr_field'
          ? `- CLR: ${feature.clrTypeSubstring}.${feature.clrFieldName} via ${feature.clrIdentityField}=${feature.clrIdentityValue}`
          : '',
        feature.locatorKind === 'pointer_chain' && feature.pointerChain
          ? `- Pointer: ${feature.pointerChain.module}+${feature.pointerChain.baseOffset} ${feature.pointerChain.offsets?.join(' -> ') ?? ''}`
          : '',
        `- Type: ${feature.valueType}`,
        `- Valeur/patch: ${feature.value || feature.patchBytes || '-'}`,
        `- Dépend de: ${feature.dependsOn?.length ? feature.dependsOn.map((id) => trainerFeatures.value.find((item) => item.id === id)?.name ?? `#${id}`).join(', ') : '-'}`,
        `- Hotkey: ${feature.hotkey || '-'}`,
        `- AOB qualite: ${feature.signatureLevel || '-'}${feature.signatureScore !== undefined ? ` (${feature.signatureScore}/100)` : ''}`,
        `- Derniere erreur: ${feature.lastError || '-'}`,
        '- Historique:',
        ...(feature.history?.length
          ? feature.history.slice(0, 8).map((item) => `  - ${item.time} [${item.status}] ${item.action}: ${item.detail}`)
          : ['  - aucun']),
        '',
      )
    }
    return lines.join('\n')
  }

  return {
    configureTrainerContext,
    trainerFeatures,
    trainerFeatureIdCounter,
    trainerBusy,
    trainerHotkeyStatus,
    trainerOverlayVisible,
    trainerOverlayStatus,
    trainerOverlayHotkey,
    trainerOverlayHotkeyId,
    saveTrainerFeatures,
    loadTrainerFeatures,
    saveOverlayHotkey,
    loadOverlayHotkey,
    createTrainerFeature,
    createTrainerClrFieldFeature,
    createTrainerFeatureFromCheckpoint,
    createTrainerFeatureFromBookmark,
    trainerFeatureSignatureQuality,
    trainerFeaturePatchBlockReason,
    resolveTrainerFeaturePointerChain,
    resolveTrainerFeatureAddress,
    doApplyTrainerFeature,
    applyTrainerFeature,
    doRestoreTrainerFeature,
    restoreTrainerFeature,
    generateTrainerFeaturePointerChain,
    saveTrainerFeatureToProfile,
    applyAllTrainerFeatures,
    restoreAllTrainerFeatures,
    getTrainerFeaturesSnapshot,
    deleteTrainerFeature,
    updateTrainerFeatureDependencies,
    clearTrainerFeatures,
    exportTrainerFeaturesJson,
    exportTrainerFeaturesMarkdown,
    registerTrainerFeatureHotkey,
    unregisterTrainerFeatureHotkey,
    registerOverlayHotkey,
    unregisterOverlayHotkey,
    reregisterPersistedHotkeys,
    refreshTrainerOverlay,
    setTrainerOverlay,
    handleGlobalHotkey,
  }
})
