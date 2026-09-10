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
import { i18n } from '@/i18n'
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

const { t } = i18n.global

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
      clrInspectorError.value = t('clrInspectorStore.inspectorNotExposed')
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
      clrLastResult.value = { success: false, error: t('clrInspectorStore.inspectorNotExposed') }
      return
    }
    clrInspectorBusy.value = true
    clrInspectorError.value = ''
    try {
      const result = await controller.attachClrInspector()
      clrLastResult.value = result
      clrInspectorError.value = result.success ? '' : (result.error ?? t('clrInspectorStore.attachFailed'))
      actionLogStore.addActionLog(
        'clr_inspector',
        result.success ? t('clrInspectorStore.attached') : t('clrInspectorStore.attachRefused'),
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
      clrLastResult.value = { success: false, error: t('clrInspectorStore.flushCacheNotExposed') }
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
      clrLastResult.value = { success: false, error: t('clrInspectorStore.findByTypeNotExposed') }
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
      clrLastResult.value = { success: false, error: t('clrInspectorStore.findByFieldValueNotExposed') }
      clrInspectorError.value = clrLastResult.value.error ?? ''
      return clrLastResult.value
    }
    if (!typeFilter || !field || !value) return { success: false, error: t('clrInspectorStore.incompleteLocator') }

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
        result.success ? t('clrInspectorStore.locatorExecuted') : t('clrInspectorStore.locatorFailed'),
        result.success
          ? t('clrInspectorStore.locatorMatches', { count: clrFieldLocatorResult.value?.matchesReturned ?? 0, type: typeFilter, field, value })
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
      clrLastResult.value = { success: false, error: t('clrInspectorStore.readObjectNotExposed') }
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
      clrLastResult.value = { success: false, error: t('clrInspectorStore.writeFieldNotExposed') }
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
        result.success ? t('clrInspectorStore.fieldWritten') : t('clrInspectorStore.writeFieldFailed'),
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
      actionLogStore.addActionLog('clr_inspector', t('clrInspectorStore.writeFieldFailed'), String(e), 'error')
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
      clrLastResult.value = { success: false, error: t('clrInspectorStore.writePathNotExposed') }
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
        result.success ? t('clrInspectorStore.pathWritten') : t('clrInspectorStore.writePathFailed'),
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
      actionLogStore.addActionLog('clr_inspector', t('clrInspectorStore.writePathFailed'), String(e), 'error')
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
      clrLastResult.value = { success: false, error: t('clrInspectorStore.writePathBatchNotExposed') }
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
        result.success && innerSuccess ? t('clrInspectorStore.transactionApplied') : t('clrInspectorStore.transactionFailed'),
        result.success && innerSuccess
          ? t('clrInspectorStore.operationsApplied', { count: sanitized.length })
          : `${String(inner?.error ?? result.error ?? '')}${inner?.rolledBack === true ? ` ${t('clrInspectorStore.rollbackOk')}` : ''}`,
        result.success && innerSuccess ? 'success' : 'error',
      )
      if (result.success) {
        await readClrObject(address)
      }
      return result
    } catch (e) {
      clrLastResult.value = { success: false, error: String(e) }
      clrInspectorError.value = String(e)
      actionLogStore.addActionLog('clr_inspector', t('clrInspectorStore.transactionFailed'), String(e), 'error')
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
      clrLastResult.value = { success: false, error: t('clrInspectorStore.writePathByLocatorNotExposed') }
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
        result.success ? t('clrInspectorStore.pathWrittenLocator') : t('clrInspectorStore.writePathByLocatorFailed'),
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
      actionLogStore.addActionLog('clr_inspector', t('clrInspectorStore.writePathByLocatorFailed'), String(e), 'error')
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
      clrLastResult.value = { success: false, error: t('clrInspectorStore.writePathBatchByLocatorNotExposed') }
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
        result.success && innerSuccess ? t('clrInspectorStore.transactionAppliedLocator') : t('clrInspectorStore.transactionByLocatorFailed'),
        result.success && innerSuccess
          ? t('clrInspectorStore.operationsAppliedAt', { count: sanitized.length, address: String(inner?.resolvedAddress ?? '?') })
          : `${String(inner?.error ?? result.error ?? '')}${inner?.rolledBack === true ? ` ${t('clrInspectorStore.rollbackOk')}` : ''}`,
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
      actionLogStore.addActionLog('clr_inspector', t('clrInspectorStore.transactionByLocatorFailed'), String(e), 'error')
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
      clrLastResult.value = { success: false, error: t('clrInspectorStore.writePathBatchAtomicNotExposed') }
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
        result.success && innerSuccess ? t('clrInspectorStore.atomicTransactionApplied') : t('clrInspectorStore.atomicTransactionFailed'),
        result.success && innerSuccess
          ? t('clrInspectorStore.atomicOperationsApplied', { count: sanitized.length, threads: String(inner?.suspendedThreadCount ?? 0) })
          : `${String(inner?.error ?? result.error ?? '')}${inner?.rolledBack === true ? ` ${t('clrInspectorStore.rollbackOk')}` : ''}`,
        result.success && innerSuccess ? 'success' : 'error',
      )
      if (result.success) {
        await readClrObject(address)
      }
      return result
    } catch (e) {
      clrLastResult.value = { success: false, error: String(e) }
      clrInspectorError.value = String(e)
      actionLogStore.addActionLog('clr_inspector', t('clrInspectorStore.atomicTransactionFailed'), String(e), 'error')
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
      clrCallMethodResult.value = { success: false, error: t('clrInspectorStore.callMethodNotExposed') }
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
        innerSuccess ? t('clrInspectorStore.setterCalled') : t('clrInspectorStore.setterCallFailed'),
        innerSuccess
          ? t('clrInspectorStore.setterCalledDetail', { method: clrCallMethodResult.value?.methodName ?? method, address: clrCallMethodResult.value?.nativeCodeAddress ?? '?', verified: clrCallMethodResult.value?.verified ? t('clrInspectorStore.yes') : t('clrInspectorStore.no') })
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
      actionLogStore.addActionLog('clr_inspector', t('clrInspectorStore.setterCallFailed'), String(e), 'error')
      return clrLastResult.value
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function enumerateClrRoots(typeSubstring = clrTypeFilter.value) {
    const controller = backend.getController()
    if (!controller.enumerateClrRoots) {
      clrLastResult.value = { success: false, error: t('clrInspectorStore.enumerateRootsNotExposed') }
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
      clrGcRootPathResult.value = { success: false, error: t('clrInspectorStore.findGcRootPathNotExposed') }
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
        innerSuccess ? t('clrInspectorStore.gcRootPathFound') : t('clrInspectorStore.gcRootPathNotFound'),
        innerSuccess
          ? t('clrInspectorStore.gcRootPathDetail', { kind: clrGcRootPathResult.value?.rootKind ?? '?', depth: clrGcRootPathResult.value?.depth ?? 0, address })
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
      clrDisassembleResult.value = { success: false, error: t('clrInspectorStore.disassembleNotExposed') }
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
        innerSuccess ? t('clrInspectorStore.methodDisassembled') : t('clrInspectorStore.disassembleFailed'),
        innerSuccess
          ? t('clrInspectorStore.methodDisassembledDetail', { method: clrDisassembleResult.value?.methodName ?? method, address: clrDisassembleResult.value?.nativeCodeAddress ?? '?', count: clrDisassembleResult.value?.returnedInstructionCount ?? 0 })
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
      clrObjectReportResult.value = { success: false, error: t('clrInspectorStore.generateReportNotExposed') }
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
        innerSuccess ? t('clrInspectorStore.reportGenerated') : t('clrInspectorStore.reportGenerationFailed'),
        innerSuccess
          ? t('clrInspectorStore.reportGeneratedDetail', { type: clrObjectReportResult.value?.rootTypeName ?? '?', address, count: clrObjectReportResult.value?.nodeCount ?? 0, truncated: clrObjectReportResult.value?.truncated ? ` ${t('clrInspectorStore.truncated')}` : '' })
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
