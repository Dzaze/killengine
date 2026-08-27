import { ref } from 'vue'
import { useAppStore } from '@/stores/app'
import { backend, type PointerChainInfo, type PointerChainResolveResult, type PointerScanResult } from '@/services/backend'
import { cleanTrainerName } from '@/utils/format'

// État module-scope (singleton) : partagé entre PointerChainPanel.vue et le
// panneau Write encore inline dans ExpertView.vue (saveStableLocator() y
// appelle savePointerChain() directement) — même raison que
// useExpertWriteSelection.ts, un état par appelant casserait ce partage.
const pointerScanAddress = ref('')
const pointerScanValueType = ref('Int32')
const pointerScanMaxDepth = ref(3)
const pointerScanMaxOffset = ref(0x1000)
const pointerScanResult = ref<PointerScanResult | null>(null)
const pointerResolveResult = ref<PointerChainResolveResult | null>(null)
const pointerScanBusy = ref(false)
const selectedPointerChainIndex = ref<number>(-1)

export function useExpertPointerChain() {
  const store = useAppStore()

  async function runPointerScan() {
    if (!pointerScanAddress.value.trim()) return
    pointerScanBusy.value = true
    pointerScanResult.value = null
    pointerResolveResult.value = null
    try {
      const controller = backend.getController()
      if (controller.scanPointerChains) {
        const result = await controller.scanPointerChains(pointerScanAddress.value, {
          maxDepth: pointerScanMaxDepth.value,
          maxOffset: pointerScanMaxOffset.value,
          maxResults: 100,
          onlyModuleBase: true,
        })
        pointerScanResult.value = result
      } else {
        pointerScanResult.value = { success: false, error: 'Methode backend indisponible (mock mode).' }
      }
    } catch (e) {
      pointerScanResult.value = { success: false, error: String(e) }
    } finally {
      pointerScanBusy.value = false
    }
  }

  async function testPointerChain(chain: PointerChainInfo) {
    try {
      const controller = backend.getController()
      if (controller.resolvePointerChain) {
        pointerResolveResult.value = await controller.resolvePointerChain(chain)
      }
    } catch (e) {
      pointerResolveResult.value = { success: false, error: String(e) }
    }
  }

  function usePointerChainAsCandidate(chain: PointerChainInfo) {
    // Place la chaîne comme adresse candidate pour écriture (test immédiat).
    void testPointerChain(chain).then(() => {
      if (pointerResolveResult.value?.success && pointerResolveResult.value.finalAddress) {
        store.selectedCandidateAddress = pointerResolveResult.value.finalAddress
        store.exactScanType = pointerScanValueType.value
      }
    })
  }

  async function savePointerChain(chain: PointerChainInfo) {
    const profileName = window.prompt('Nom du profil :', cleanTrainerName(store.processName || 'Jeu cible', 'Jeu cible'))
    if (!profileName) return
    const targetName = window.prompt('Nom de la cible :', 'Ressource')
    if (!targetName) return
    try {
      const controller = backend.getController()
      if (controller.savePointerChainProfileTarget) {
        const result = await controller.savePointerChainProfileTarget(
          profileName,
          targetName,
          chain,
          pointerScanValueType.value,
          'Chaine de pointeurs auto-detectee',
        )
        if (!result.success) {
          window.alert('Erreur sauvegarde profil : ' + (result.error ?? 'inconnue'))
        }
      }
    } catch (e) {
      window.alert('Erreur : ' + String(e))
    }
  }

  async function watchPointerChain(chain: PointerChainInfo) {
    await store.addWatchedPointerChain(
      { module: chain.module, baseOffset: chain.baseOffset, offsets: chain.offsets },
      pointerScanValueType.value,
      chain.label || `Chaine 0x${pointerScanAddress.value}`,
    )
  }

  function bookmarkPointerChain(chain: PointerChainInfo) {
    store.addWorkspaceBookmark({
      kind: 'pointer',
      label: chain.label || `Pointer chain ${chain.depth}`,
      address: pointerScanAddress.value,
      type: pointerScanValueType.value,
      note: `profondeur ${chain.depth}`,
      payload: {
        chain,
        targetAddress: pointerScanAddress.value,
        maxDepth: pointerScanMaxDepth.value,
        maxOffset: pointerScanMaxOffset.value,
        resolved: pointerResolveResult.value?.success ? pointerResolveResult.value.finalAddress : undefined,
      },
    })
  }

  function resetPointerChainState() {
    pointerScanAddress.value = ''
    pointerScanResult.value = null
    pointerResolveResult.value = null
    selectedPointerChainIndex.value = -1
  }

  return {
    pointerScanAddress,
    pointerScanValueType,
    pointerScanMaxDepth,
    pointerScanMaxOffset,
    pointerScanResult,
    pointerResolveResult,
    pointerScanBusy,
    selectedPointerChainIndex,
    runPointerScan,
    testPointerChain,
    usePointerChainAsCandidate,
    savePointerChain,
    watchPointerChain,
    bookmarkPointerChain,
    resetPointerChainState,
  }
}
