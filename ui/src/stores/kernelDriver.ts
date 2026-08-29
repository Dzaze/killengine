/**
 * KillEngine — store Driver kernel (extrait de app.ts, candidat S4 de
 * docs/REFACTOR_ROADMAP.md, 29/08/2026).
 *
 * `memoryAccessMode`/`kernelMemoryModeActive`/`writeMemoryValueByMode` et les
 * autres fonctions de dispatch usermode-vs-kernel restent dans `app.ts` --
 * elles ne sont pas spécifiques au driver kernel, elles servent tout le
 * reste de l'app (scan/write). `writeMemoryKernel` n'a PAS le gate
 * confirmRiskAction ici -- `app.ts` confirme AVANT de déléguer. `logAiAudit`
 * (télémétrie liée à Investigation/searchQuery) reste aussi dans `app.ts`,
 * appelé après délégation à partir des résultats renvoyés ici.
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'
import {
  backend,
  type KernelDriverStatus,
  type KernelMemoryReadResult,
  type KernelMemoryWriteResult,
} from '@/services/backend'
import { useActionLogStore } from './actionLog'

export const useKernelDriverStore = defineStore('kernelDriver', () => {
  const actionLogStore = useActionLogStore()

  const kernelDriverStatus = ref<KernelDriverStatus | null>(null)
  const kernelDriverStatusLoading = ref(false)
  const kernelDriverStartLoading = ref(false)
  const kernelDriverStatusError = ref('')
  const kernelMemoryReadResult = ref<KernelMemoryReadResult | null>(null)
  const kernelMemoryReadBusy = ref(false)
  const kernelMemoryWriteResult = ref<KernelMemoryWriteResult | null>(null)
  const kernelMemoryWriteBusy = ref(false)

  async function refreshKernelDriverStatus() {
    kernelDriverStatusLoading.value = true
    kernelDriverStatusError.value = ''
    try {
      const controller = backend.getController()
      if (!controller.probeKernelDriver) {
        kernelDriverStatus.value = null
        kernelDriverStatusError.value = 'Probe driver noyau non exposé par ce backend.'
        return
      }
      kernelDriverStatus.value = await controller.probeKernelDriver()
    } catch (e) {
      kernelDriverStatus.value = null
      kernelDriverStatusError.value = String(e)
    } finally {
      kernelDriverStatusLoading.value = false
    }
  }

  async function startKernelDriver() {
    kernelDriverStartLoading.value = true
    kernelDriverStatusError.value = ''
    try {
      const controller = backend.getController()
      if (!controller.startKernelDriver) {
        kernelDriverStatusError.value = 'Démarrage driver noyau non exposé par ce backend.'
        return
      }
      kernelDriverStatus.value = await controller.startKernelDriver()
      actionLogStore.addActionLog(
        'kernel_driver',
        kernelDriverStatus.value.success ? 'Driver kernel démarré' : 'Driver kernel indisponible',
        kernelDriverStatus.value.message || kernelDriverStatus.value.error || '',
        kernelDriverStatus.value.success ? 'success' : 'warning',
      )
      return kernelDriverStatus.value
    } catch (e) {
      kernelDriverStatusError.value = String(e)
    } finally {
      kernelDriverStartLoading.value = false
    }
  }

  async function readMemoryKernel(addressHex: string, size: number) {
    kernelMemoryReadBusy.value = true
    try {
      const controller = backend.getController()
      if (!controller.readMemoryKernel) {
        kernelMemoryReadResult.value = { success: false, error: 'Lecture kernel non exposée par ce backend.' }
        return
      }
      kernelMemoryReadResult.value = await controller.readMemoryKernel(addressHex, size)
      actionLogStore.addActionLog(
        'kernel_read',
        kernelMemoryReadResult.value.success ? `Lecture kernel 0x${addressHex}` : `Lecture kernel échouée 0x${addressHex}`,
        kernelMemoryReadResult.value.success
          ? `${kernelMemoryReadResult.value.bytesRead} octet(s) lus via le driver noyau.`
          : (kernelMemoryReadResult.value.error ?? ''),
        kernelMemoryReadResult.value.success ? 'success' : 'error',
      )
      return kernelMemoryReadResult.value
    } catch (e) {
      kernelMemoryReadResult.value = { success: false, error: String(e) }
      return kernelMemoryReadResult.value
    } finally {
      kernelMemoryReadBusy.value = false
    }
  }

  /** Pas de confirmRiskAction ici -- app.ts confirme avant d'appeler. */
  async function writeMemoryKernel(addressHex: string, hexBytes: string) {
    kernelMemoryWriteBusy.value = true
    try {
      const controller = backend.getController()
      if (!controller.writeMemoryKernel) {
        kernelMemoryWriteResult.value = { success: false, error: 'Écriture kernel non exposée par ce backend.' }
        return
      }
      kernelMemoryWriteResult.value = await controller.writeMemoryKernel(addressHex, hexBytes)
      actionLogStore.addActionLog(
        'kernel_write',
        kernelMemoryWriteResult.value.success ? `Écriture kernel 0x${addressHex}` : `Écriture kernel échouée 0x${addressHex}`,
        kernelMemoryWriteResult.value.success
          ? `${kernelMemoryWriteResult.value.bytesWritten} octet(s) écrits via le driver noyau.`
          : (kernelMemoryWriteResult.value.error ?? ''),
        kernelMemoryWriteResult.value.success ? 'success' : 'error',
      )
      return kernelMemoryWriteResult.value
    } catch (e) {
      kernelMemoryWriteResult.value = { success: false, error: String(e) }
      return kernelMemoryWriteResult.value
    } finally {
      kernelMemoryWriteBusy.value = false
    }
  }

  return {
    kernelDriverStatus,
    kernelDriverStatusLoading,
    kernelDriverStartLoading,
    kernelDriverStatusError,
    kernelMemoryReadResult,
    kernelMemoryReadBusy,
    kernelMemoryWriteResult,
    kernelMemoryWriteBusy,
    refreshKernelDriverStatus,
    startKernelDriver,
    readMemoryKernel,
    writeMemoryKernel,
  }
})
