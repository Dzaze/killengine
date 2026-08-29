/**
 * KillEngine — store CLR Inspector (extrait de app.ts, candidat S1 de
 * docs/REFACTOR_ROADMAP.md, 29/08/2026).
 *
 * Miroir frontend de l'extraction backend PHASE 209 (Codex,
 * `apps/desktop/clr_inspector_bridge.*`) : les deux moitiés du pont CLR sont
 * désormais chacune dans leur propre fichier.
 *
 * Dépend de `./actionLog` (safe, store-fondation sans dépendance vers `./app`)
 * pour ses propres addActionLog. Ne dépend PAS de `confirmRiskAction` (pas
 * encore extrait de app.ts, ça créerait un import circulaire) : les 7
 * fonctions à risque (write..., callClrInstanceMethod) n'ont volontairement
 * PAS le gate ici -- `app.ts` valide les entrées et appelle confirmRiskAction
 * AVANT de déléguer au store, exactement comme avant l'extraction.
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'
import {
  backend,
  type ClrCallInstanceMethodResult,
  type ClrDisassembleMethodResult,
  type ClrFieldLocatorResult,
  type ClrGcRootPathResult,
  type ClrInspectorStatus,
  type ClrObjectReadResult,
  type ClrObjectReportResult,
  type ClrObjectSummary,
  type ClrPathWriteOperation,
  type ClrRootInfo,
  type ClrRpcResult,
} from '@/services/backend'
import { useActionLogStore } from './actionLog'

export const useClrInspectorStore = defineStore('clrInspector', () => {
  const actionLogStore = useActionLogStore()

  const clrInspectorStatus = ref<ClrInspectorStatus | null>(null)
  const clrInspectorBusy = ref(false)
  const clrInspectorError = ref('')
  const clrTypeFilter = ref('KillEngine.ClrTestTarget')
  const clrObjects = ref<ClrObjectSummary[]>([])
  const clrSelectedObject = ref<ClrObjectReadResult | null>(null)
  const clrRoots = ref<ClrRootInfo[]>([])
  const clrLastResult = ref<ClrRpcResult | null>(null)
  const clrFieldLocatorResult = ref<ClrFieldLocatorResult | null>(null)
  const clrCallMethodResult = ref<ClrCallInstanceMethodResult | null>(null)
  const clrGcRootPathResult = ref<ClrGcRootPathResult | null>(null)
  const clrDisassembleResult = ref<ClrDisassembleMethodResult | null>(null)
  const clrObjectReportResult = ref<ClrObjectReportResult | null>(null)

  async function refreshClrInspectorStatus() {
    const controller = backend.getController()
    if (!controller.getClrInspectorStatus) {
      clrInspectorStatus.value = null
      clrInspectorError.value = 'Inspecteur CLR non exposé par ce backend.'
      return
    }
    try {
      clrInspectorStatus.value = await controller.getClrInspectorStatus()
      clrInspectorError.value = clrInspectorStatus.value.error ?? ''
    } catch (e) {
      clrInspectorStatus.value = null
      clrInspectorError.value = String(e)
    }
  }

  async function attachClrInspector() {
    const controller = backend.getController()
    if (!controller.attachClrInspector) {
      clrLastResult.value = { success: false, error: 'Inspecteur CLR non exposé par ce backend.' }
      return
    }
    clrInspectorBusy.value = true
    clrInspectorError.value = ''
    try {
      const result = await controller.attachClrInspector()
      clrLastResult.value = result
      clrInspectorError.value = result.success ? '' : (result.error ?? 'Attache CLR échouée.')
      actionLogStore.addActionLog(
        'clr_inspector',
        result.success ? 'Inspecteur CLR attaché' : 'Inspecteur CLR refusé',
        result.success ? `PID ${clrInspectorStatus.value?.pid ?? ''}` : clrInspectorError.value,
        result.success ? 'success' : 'warning',
      )
      await refreshClrInspectorStatus()
    } catch (e) {
      clrLastResult.value = { success: false, error: String(e) }
      clrInspectorError.value = String(e)
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function detachClrInspector() {
    const controller = backend.getController()
    if (!controller.detachClrInspector) return
    clrInspectorBusy.value = true
    try {
      clrLastResult.value = await controller.detachClrInspector()
      clrSelectedObject.value = null
      clrObjects.value = []
      clrRoots.value = []
      await refreshClrInspectorStatus()
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function shutdownClrInspector() {
    const controller = backend.getController()
    if (!controller.shutdownClrInspector) return
    clrInspectorBusy.value = true
    try {
      clrLastResult.value = await controller.shutdownClrInspector()
      clrSelectedObject.value = null
      clrObjects.value = []
      clrRoots.value = []
      await refreshClrInspectorStatus()
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function flushClrInspectorCache() {
    const controller = backend.getController()
    if (!controller.flushClrInspectorCache) {
      clrLastResult.value = { success: false, error: 'flushCachedData non exposé par ce backend.' }
      return
    }
    clrInspectorBusy.value = true
    try {
      clrLastResult.value = await controller.flushClrInspectorCache()
      if (!clrLastResult.value.success) clrInspectorError.value = clrLastResult.value.error ?? ''
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function findClrObjects(typeSubstring = clrTypeFilter.value) {
    const controller = backend.getController()
    if (!controller.findClrObjectsByType) {
      clrLastResult.value = { success: false, error: 'findObjectsByType non exposé par ce backend.' }
      return
    }
    clrInspectorBusy.value = true
    clrInspectorError.value = ''
    try {
      const result = await controller.findClrObjectsByType(typeSubstring.trim() || 'KillEngine.ClrTestTarget')
      clrLastResult.value = result
      clrObjects.value = result.success && Array.isArray(result.result) ? result.result : []
      clrInspectorError.value = result.success ? '' : (result.error ?? '')
    } catch (e) {
      clrObjects.value = []
      clrLastResult.value = { success: false, error: String(e) }
      clrInspectorError.value = String(e)
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function findClrObjectsByFieldValue(typeSubstring: string, fieldName: string, expectedValue: string, maxResults = 20) {
    const controller = backend.getController()
    const typeFilter = typeSubstring.trim()
    const field = fieldName.trim()
    const value = expectedValue.trim()
    if (!controller.findClrObjectsByFieldValue) {
      clrLastResult.value = { success: false, error: 'findObjectsByFieldValue non exposé par ce backend.' }
      clrInspectorError.value = clrLastResult.value.error ?? ''
      return clrLastResult.value
    }
    if (!typeFilter || !field || !value) return { success: false, error: 'Locator CLR incomplet.' }

    clrInspectorBusy.value = true
    clrInspectorError.value = ''
    try {
      const bounded = Math.min(200, Math.max(1, Math.round(maxResults)))
      const result = await controller.findClrObjectsByFieldValue(typeFilter, field, value, bounded)
      clrLastResult.value = result
      clrFieldLocatorResult.value = result.success && result.result ? result.result : null
      clrInspectorError.value = result.success ? '' : (result.error ?? '')
      actionLogStore.addActionLog(
        'clr_inspector',
        result.success ? 'Locator CLR exécuté' : 'Locator CLR échoué',
        result.success
          ? `${clrFieldLocatorResult.value?.matchesReturned ?? 0} match(es) pour ${typeFilter}.${field} = ${value}.`
          : (result.error ?? ''),
        result.success ? 'success' : 'warning',
      )
      return result
    } catch (e) {
      clrFieldLocatorResult.value = null
      clrLastResult.value = { success: false, error: String(e) }
      clrInspectorError.value = String(e)
      return clrLastResult.value
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function readClrObject(addressHex: string) {
    const controller = backend.getController()
    if (!controller.readClrObject) {
      clrLastResult.value = { success: false, error: 'readObject non exposé par ce backend.' }
      return
    }
    clrInspectorBusy.value = true
    try {
      const result = await controller.readClrObject(addressHex)
      clrLastResult.value = result
      clrSelectedObject.value = result.success && result.result ? result.result : null
      clrInspectorError.value = result.success ? '' : (result.error ?? '')
    } finally {
      clrInspectorBusy.value = false
    }
  }

  /** Pas de confirmRiskAction ici -- app.ts valide/confirme avant d'appeler. */
  async function writeClrPrimitiveField(objectAddressHex: string, fieldName: string, value: string) {
    const controller = backend.getController()
    const address = objectAddressHex.trim()
    const field = fieldName.trim()
    const text = value.trim()
    if (!controller.writeClrPrimitiveField) {
      clrLastResult.value = { success: false, error: 'writePrimitiveField non exposé par ce backend.' }
      clrInspectorError.value = clrLastResult.value.error ?? ''
      return
    }
    if (!address || !field || !text) return

    clrInspectorBusy.value = true
    try {
      const result = await controller.writeClrPrimitiveField(address, field, text)
      clrLastResult.value = result
      clrInspectorError.value = result.success ? '' : (result.error ?? '')
      const writeResult = result.result as Record<string, unknown> | undefined
      actionLogStore.addActionLog(
        'clr_inspector',
        result.success ? 'Champ CLR écrit' : 'Écriture champ CLR échouée',
        result.success
          ? `${field} @ ${String(writeResult?.fieldAddress ?? address)} = ${String(writeResult?.value ?? text)}`
          : (result.error ?? ''),
        result.success ? 'success' : 'error',
      )
      if (result.success) {
        await readClrObject(address)
      }
      return result
    } catch (e) {
      clrLastResult.value = { success: false, error: String(e) }
      clrInspectorError.value = String(e)
      actionLogStore.addActionLog('clr_inspector', 'Écriture champ CLR échouée', String(e), 'error')
      return clrLastResult.value
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function writeClrPrimitivePath(objectAddressHex: string, path: string, value: string) {
    const controller = backend.getController()
    const address = objectAddressHex.trim()
    const pathText = path.trim()
    const text = value.trim()
    if (!controller.writeClrPrimitivePath) {
      clrLastResult.value = { success: false, error: 'writePrimitivePath non exposé par ce backend.' }
      clrInspectorError.value = clrLastResult.value.error ?? ''
      return
    }
    if (!address || !pathText || !text) return

    clrInspectorBusy.value = true
    try {
      const result = await controller.writeClrPrimitivePath(address, pathText, text)
      clrLastResult.value = result
      clrInspectorError.value = result.success ? '' : (result.error ?? '')
      const writeResult = result.result as Record<string, unknown> | undefined
      actionLogStore.addActionLog(
        'clr_inspector',
        result.success ? 'Chemin CLR écrit' : 'Écriture chemin CLR échouée',
        result.success
          ? `${pathText} @ ${String(writeResult?.fieldAddress ?? address)} = ${String(writeResult?.value ?? text)}`
          : (result.error ?? ''),
        result.success ? 'success' : 'error',
      )
      if (result.success) {
        await readClrObject(address)
      }
      return result
    } catch (e) {
      clrLastResult.value = { success: false, error: String(e) }
      clrInspectorError.value = String(e)
      actionLogStore.addActionLog('clr_inspector', 'Écriture chemin CLR échouée', String(e), 'error')
      return clrLastResult.value
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function writeClrPrimitivePathBatch(objectAddressHex: string, operations: ClrPathWriteOperation[]) {
    const controller = backend.getController()
    const address = objectAddressHex.trim()
    const sanitized = operations
      .map((operation) => ({ path: operation.path.trim(), value: operation.value.trim() }))
      .filter((operation) => operation.path && operation.value)
      .slice(0, 32)
    if (!controller.writeClrPrimitivePathBatch) {
      clrLastResult.value = { success: false, error: 'writePrimitivePathBatch non exposé par ce backend.' }
      clrInspectorError.value = clrLastResult.value.error ?? ''
      return
    }
    if (!address || sanitized.length === 0) return

    clrInspectorBusy.value = true
    try {
      const result = await controller.writeClrPrimitivePathBatch(address, sanitized)
      clrLastResult.value = result
      const inner = result.result as Record<string, unknown> | undefined
      const innerSuccess = Boolean(inner?.success ?? result.success)
      clrInspectorError.value = result.success && innerSuccess ? '' : String(inner?.error ?? result.error ?? '')
      actionLogStore.addActionLog(
        'clr_inspector',
        result.success && innerSuccess ? 'Transaction CLR appliquée' : 'Transaction CLR échouée',
        result.success && innerSuccess
          ? `${sanitized.length} opération(s) appliquée(s).`
          : `${String(inner?.error ?? result.error ?? '')}${inner?.rolledBack === true ? ' Rollback OK.' : ''}`,
        result.success && innerSuccess ? 'success' : 'error',
      )
      if (result.success) {
        await readClrObject(address)
      }
      return result
    } catch (e) {
      clrLastResult.value = { success: false, error: String(e) }
      clrInspectorError.value = String(e)
      actionLogStore.addActionLog('clr_inspector', 'Transaction CLR échouée', String(e), 'error')
      return clrLastResult.value
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function writeClrPrimitivePathByLocator(typeSubstring: string, identityField: string, identityValue: string, path: string, value: string) {
    const controller = backend.getController()
    const type = typeSubstring.trim()
    const idField = identityField.trim()
    const idValue = identityValue.trim()
    const pathText = path.trim()
    const text = value.trim()
    if (!controller.writeClrPrimitivePathByLocator) {
      clrLastResult.value = { success: false, error: 'writeClrPrimitivePathByLocator non exposé par ce backend.' }
      clrInspectorError.value = clrLastResult.value.error ?? ''
      return
    }
    if (!type || !idField || !idValue || !pathText || !text) return

    clrInspectorBusy.value = true
    try {
      const result = await controller.writeClrPrimitivePathByLocator(type, idField, idValue, pathText, text)
      clrLastResult.value = result
      clrInspectorError.value = result.success ? '' : (result.error ?? '')
      const writeResult = result.result as Record<string, unknown> | undefined
      actionLogStore.addActionLog(
        'clr_inspector',
        result.success ? 'Chemin CLR écrit (locator)' : 'Écriture chemin CLR par locator échouée',
        result.success
          ? `${pathText} @ ${String(writeResult?.resolvedAddress ?? '?')} = ${String(writeResult?.value ?? text)}`
          : (result.error ?? ''),
        result.success ? 'success' : 'error',
      )
      const resolvedAddress = (result.result as Record<string, unknown> | undefined)?.resolvedAddress as string | undefined
      if (result.success && resolvedAddress) {
        await readClrObject(resolvedAddress)
      }
      return result
    } catch (e) {
      clrLastResult.value = { success: false, error: String(e) }
      clrInspectorError.value = String(e)
      actionLogStore.addActionLog('clr_inspector', 'Écriture chemin CLR par locator échouée', String(e), 'error')
      return clrLastResult.value
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function writeClrPrimitivePathBatchByLocator(typeSubstring: string, identityField: string, identityValue: string, operations: ClrPathWriteOperation[]) {
    const controller = backend.getController()
    const type = typeSubstring.trim()
    const idField = identityField.trim()
    const idValue = identityValue.trim()
    const sanitized = operations
      .map((operation) => ({ path: operation.path.trim(), value: operation.value.trim() }))
      .filter((operation) => operation.path && operation.value)
      .slice(0, 32)
    if (!controller.writeClrPrimitivePathBatchByLocator) {
      clrLastResult.value = { success: false, error: 'writeClrPrimitivePathBatchByLocator non exposé par ce backend.' }
      clrInspectorError.value = clrLastResult.value.error ?? ''
      return
    }
    if (!type || !idField || !idValue || sanitized.length === 0) return

    clrInspectorBusy.value = true
    try {
      const result = await controller.writeClrPrimitivePathBatchByLocator(type, idField, idValue, sanitized)
      clrLastResult.value = result
      const inner = result.result as Record<string, unknown> | undefined
      const innerSuccess = Boolean(inner?.success ?? result.success)
      clrInspectorError.value = result.success && innerSuccess ? '' : String(inner?.error ?? result.error ?? '')
      actionLogStore.addActionLog(
        'clr_inspector',
        result.success && innerSuccess ? 'Transaction CLR appliquée (locator)' : 'Transaction CLR par locator échouée',
        result.success && innerSuccess
          ? `${sanitized.length} opération(s) appliquée(s) @ ${String(inner?.resolvedAddress ?? '?')}.`
          : `${String(inner?.error ?? result.error ?? '')}${inner?.rolledBack === true ? ' Rollback OK.' : ''}`,
        result.success && innerSuccess ? 'success' : 'error',
      )
      const resolvedAddress = inner?.resolvedAddress as string | undefined
      if (result.success && resolvedAddress) {
        await readClrObject(resolvedAddress)
      }
      return result
    } catch (e) {
      clrLastResult.value = { success: false, error: String(e) }
      clrInspectorError.value = String(e)
      actionLogStore.addActionLog('clr_inspector', 'Transaction CLR par locator échouée', String(e), 'error')
      return clrLastResult.value
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function writeClrPrimitivePathBatchAtomic(objectAddressHex: string, operations: ClrPathWriteOperation[]) {
    const controller = backend.getController()
    const address = objectAddressHex.trim()
    const sanitized = operations
      .map((operation) => ({ path: operation.path.trim(), value: operation.value.trim() }))
      .filter((operation) => operation.path && operation.value)
      .slice(0, 32)
    if (!controller.writeClrPrimitivePathBatchAtomic) {
      clrLastResult.value = { success: false, error: 'writeClrPrimitivePathBatchAtomic non exposé par ce backend.' }
      clrInspectorError.value = clrLastResult.value.error ?? ''
      return
    }
    if (!address || sanitized.length === 0) return

    clrInspectorBusy.value = true
    try {
      const result = await controller.writeClrPrimitivePathBatchAtomic(address, sanitized)
      clrLastResult.value = result
      const inner = result.result as Record<string, unknown> | undefined
      const innerSuccess = Boolean(inner?.success ?? result.success)
      clrInspectorError.value = result.success && innerSuccess ? '' : String(inner?.error ?? result.error ?? '')
      actionLogStore.addActionLog(
        'clr_inspector',
        result.success && innerSuccess ? 'Transaction CLR atomique appliquée' : 'Transaction CLR atomique échouée',
        result.success && innerSuccess
          ? `${sanitized.length} opération(s) appliquée(s), ${String(inner?.suspendedThreadCount ?? 0)} thread(s) suspendue(s) pendant l'écriture.`
          : `${String(inner?.error ?? result.error ?? '')}${inner?.rolledBack === true ? ' Rollback OK.' : ''}`,
        result.success && innerSuccess ? 'success' : 'error',
      )
      if (result.success) {
        await readClrObject(address)
      }
      return result
    } catch (e) {
      clrLastResult.value = { success: false, error: String(e) }
      clrInspectorError.value = String(e)
      actionLogStore.addActionLog('clr_inspector', 'Transaction CLR atomique échouée', String(e), 'error')
      return clrLastResult.value
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function callClrInstanceMethod(objectAddressHex: string, methodName: string, valueText: string, valueType: string) {
    const controller = backend.getController()
    const address = objectAddressHex.trim()
    const method = methodName.trim()
    const text = valueText.trim()
    if (!controller.callClrInstanceMethod) {
      clrCallMethodResult.value = { success: false, error: 'callClrInstanceMethod non exposé par ce backend.' }
      clrInspectorError.value = clrCallMethodResult.value.error ?? ''
      return
    }
    if (!address || !method) return

    clrInspectorBusy.value = true
    try {
      const result = await controller.callClrInstanceMethod(address, method, text, valueType.trim())
      clrLastResult.value = result
      clrCallMethodResult.value = (result.result as ClrCallInstanceMethodResult | undefined) ?? {
        success: result.success,
        error: result.error,
      }
      const innerSuccess = clrCallMethodResult.value?.success ?? result.success
      clrInspectorError.value = innerSuccess ? '' : (clrCallMethodResult.value?.error ?? result.error ?? '')
      actionLogStore.addActionLog(
        'clr_inspector',
        innerSuccess ? 'Setter CLR appelé' : 'Appel de setter CLR échoué',
        innerSuccess
          ? `${clrCallMethodResult.value?.methodName ?? method} @ ${clrCallMethodResult.value?.nativeCodeAddress ?? '?'} (vérifié: ${clrCallMethodResult.value?.verified ? 'oui' : 'non'})`
          : (clrCallMethodResult.value?.error ?? result.error ?? ''),
        innerSuccess ? 'success' : 'error',
      )
      if (innerSuccess) {
        await readClrObject(address)
      }
      return result
    } catch (e) {
      clrLastResult.value = { success: false, error: String(e) }
      clrCallMethodResult.value = { success: false, error: String(e) }
      clrInspectorError.value = String(e)
      actionLogStore.addActionLog('clr_inspector', 'Appel de setter CLR échoué', String(e), 'error')
      return clrLastResult.value
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function enumerateClrRoots(typeSubstring = clrTypeFilter.value) {
    const controller = backend.getController()
    if (!controller.enumerateClrRoots) {
      clrLastResult.value = { success: false, error: 'enumerateRoots non exposé par ce backend.' }
      return
    }
    clrInspectorBusy.value = true
    try {
      const result = await controller.enumerateClrRoots(typeSubstring.trim())
      clrLastResult.value = result
      clrRoots.value = result.success && Array.isArray(result.result) ? result.result : []
      clrInspectorError.value = result.success ? '' : (result.error ?? '')
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function findClrGcRootPath(targetObjectAddressHex: string, maxDepth = 8, maxRootsScanned = 4000) {
    const controller = backend.getController()
    const address = targetObjectAddressHex.trim()
    if (!controller.findClrGcRootPath) {
      clrGcRootPathResult.value = { success: false, error: 'findClrGcRootPath non exposé par ce backend.' }
      clrInspectorError.value = clrGcRootPathResult.value.error ?? ''
      return
    }
    if (!address) return

    clrInspectorBusy.value = true
    try {
      const result = await controller.findClrGcRootPath(address, maxDepth, maxRootsScanned)
      clrLastResult.value = result
      clrGcRootPathResult.value = (result.result as ClrGcRootPathResult | undefined) ?? {
        success: result.success,
        error: result.error,
      }
      const innerSuccess = clrGcRootPathResult.value?.success ?? result.success
      clrInspectorError.value = innerSuccess ? '' : (clrGcRootPathResult.value?.error ?? clrGcRootPathResult.value?.message ?? result.error ?? '')
      actionLogStore.addActionLog(
        'clr_inspector',
        innerSuccess ? 'Chemin GC root trouvé' : 'Chemin GC root introuvable',
        innerSuccess
          ? `${clrGcRootPathResult.value?.rootKind ?? '?'} → ${clrGcRootPathResult.value?.depth ?? 0} saut(s) → ${address}`
          : (clrGcRootPathResult.value?.message ?? clrGcRootPathResult.value?.error ?? result.error ?? ''),
        innerSuccess ? 'success' : 'error',
      )
      return result
    } catch (e) {
      clrLastResult.value = { success: false, error: String(e) }
      clrGcRootPathResult.value = { success: false, error: String(e) }
      clrInspectorError.value = String(e)
      return clrLastResult.value
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function disassembleClrMethod(objectAddressHex: string, methodName: string, instructionCount = 24) {
    const controller = backend.getController()
    const address = objectAddressHex.trim()
    const method = methodName.trim()
    if (!controller.disassembleClrMethod) {
      clrDisassembleResult.value = { success: false, error: 'disassembleClrMethod non exposé par ce backend.' }
      clrInspectorError.value = clrDisassembleResult.value.error ?? ''
      return
    }
    if (!address || !method) return

    clrInspectorBusy.value = true
    try {
      const result = await controller.disassembleClrMethod(address, method, instructionCount)
      clrLastResult.value = result
      clrDisassembleResult.value = (result.result as ClrDisassembleMethodResult | undefined) ?? {
        success: result.success,
        error: result.error,
      }
      const innerSuccess = clrDisassembleResult.value?.success ?? result.success
      clrInspectorError.value = innerSuccess ? '' : (clrDisassembleResult.value?.error ?? result.error ?? '')
      actionLogStore.addActionLog(
        'clr_inspector',
        innerSuccess ? 'Méthode CLR désassemblée' : 'Désassemblage CLR échoué',
        innerSuccess
          ? `${clrDisassembleResult.value?.methodName ?? method} @ ${clrDisassembleResult.value?.nativeCodeAddress ?? '?'} (${clrDisassembleResult.value?.returnedInstructionCount ?? 0} instruction(s))`
          : (clrDisassembleResult.value?.error ?? result.error ?? ''),
        innerSuccess ? 'success' : 'error',
      )
      return result
    } catch (e) {
      clrLastResult.value = { success: false, error: String(e) }
      clrDisassembleResult.value = { success: false, error: String(e) }
      clrInspectorError.value = String(e)
      return clrLastResult.value
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function generateClrObjectReport(objectAddressHex: string, maxDepth = 0, maxNodes = 0, includeGcRootChain = true) {
    const controller = backend.getController()
    const address = objectAddressHex.trim()
    if (!controller.generateClrObjectReport) {
      clrObjectReportResult.value = { success: false, error: 'generateClrObjectReport non exposé par ce backend.' }
      clrInspectorError.value = clrObjectReportResult.value.error ?? ''
      return
    }
    if (!address) return

    clrInspectorBusy.value = true
    try {
      const result = await controller.generateClrObjectReport(address, maxDepth, maxNodes, includeGcRootChain)
      clrLastResult.value = result
      clrObjectReportResult.value = (result.result as ClrObjectReportResult | undefined) ?? {
        success: result.success,
        error: result.error,
      }
      const innerSuccess = clrObjectReportResult.value?.success ?? result.success
      clrInspectorError.value = innerSuccess ? '' : (clrObjectReportResult.value?.error ?? result.error ?? '')
      actionLogStore.addActionLog(
        'clr_inspector',
        innerSuccess ? 'Rapport d\'objet CLR généré' : 'Génération du rapport CLR échouée',
        innerSuccess
          ? `${clrObjectReportResult.value?.rootTypeName ?? '?'} @ ${address} — ${clrObjectReportResult.value?.nodeCount ?? 0} nœud(s)${clrObjectReportResult.value?.truncated ? ' (tronqué)' : ''}`
          : (clrObjectReportResult.value?.error ?? result.error ?? ''),
        innerSuccess ? 'success' : 'error',
      )
      return result
    } catch (e) {
      clrLastResult.value = { success: false, error: String(e) }
      clrObjectReportResult.value = { success: false, error: String(e) }
      clrInspectorError.value = String(e)
      return clrLastResult.value
    } finally {
      clrInspectorBusy.value = false
    }
  }

  return {
    clrInspectorStatus,
    clrInspectorBusy,
    clrInspectorError,
    clrTypeFilter,
    clrObjects,
    clrSelectedObject,
    clrRoots,
    clrLastResult,
    clrFieldLocatorResult,
    clrCallMethodResult,
    clrGcRootPathResult,
    clrDisassembleResult,
    clrObjectReportResult,
    refreshClrInspectorStatus,
    attachClrInspector,
    detachClrInspector,
    shutdownClrInspector,
    flushClrInspectorCache,
    findClrObjects,
    findClrObjectsByFieldValue,
    readClrObject,
    writeClrPrimitiveField,
    writeClrPrimitivePath,
    writeClrPrimitivePathBatch,
    writeClrPrimitivePathByLocator,
    writeClrPrimitivePathBatchByLocator,
    writeClrPrimitivePathBatchAtomic,
    callClrInstanceMethod,
    enumerateClrRoots,
    findClrGcRootPath,
    disassembleClrMethod,
    generateClrObjectReport,
  }
})
