import { computed, nextTick, ref } from 'vue'
import { useAppStore } from '@/stores/app'
import { i18n } from '@/i18n'
import type { MemoryWriteTarget } from '@/services/backend'

const { t } = i18n.global

const selectedCandidateAddresses = ref<string[]>([])
const selectedWriteTargetOverrides = ref<Record<string, MemoryWriteTarget>>({})
const writePanelRef = ref<HTMLElement | null>(null)

function encodedDisplayWriteValue(value: string, variantLabel?: string): string {
  const cleanValue = value.trim().replace(',', '.')
  if (!cleanValue) return '-'
  const multiplier = variantLabel?.match(/\bx\s*(\d+(?:\.\d+)?)\b/i)
  if (!multiplier) return cleanValue
  const numeric = Number(cleanValue)
  const scale = Number(multiplier[1])
  if (!Number.isFinite(numeric) || !Number.isFinite(scale)) return cleanValue
  const encoded = numeric * scale
  return Number.isInteger(encoded) ? String(encoded) : String(encoded)
}

export function useExpertWriteSelection() {
  const store = useAppStore()

  const currentPageCandidates = computed(() => store.candidatePage?.candidates ?? [])
  const displayedCandidates = computed(() => currentPageCandidates.value.filter(
    (candidate) => !store.ignoredCandidateAddresses.includes(candidate.address),
  ))
  const selectedCandidateRecords = computed(() => selectedCandidateAddresses.value
    .map((address) => currentPageCandidates.value.find((candidate) => candidate.address === address))
    .filter((candidate): candidate is NonNullable<typeof candidate> => Boolean(candidate)))
  const selectedWriteTargets = computed<MemoryWriteTarget[]>(() => selectedCandidateAddresses.value.map((address) => {
    const override = selectedWriteTargetOverrides.value[address]
    if (override) return override
    const record = currentPageCandidates.value.find((candidate) => candidate.address === address)
    return {
      address,
      type: String(record?.type ?? store.exactScanType),
      variantLabel: record?.variantLabel,
    }
  }))
  const selectedWriteHasVariants = computed(() => selectedWriteTargets.value.some((target) => Boolean(target.variantLabel)))
  const writePlan = computed(() => selectedWriteTargets.value.map((target) => {
    const displayValue = store.writeValue.trim()
    const encodedValue = encodedDisplayWriteValue(displayValue, target.variantLabel)
    return {
      ...target,
      displayValue,
      encodedValue,
      mode: target.variantLabel || target.type,
    }
  }))
  const writeFailures = computed(() => (store.writeResult?.results ?? []).filter((result) => !result.success))
  const selectedCandidateTypes = computed(() => Array.from(new Set(
    selectedWriteTargets.value.map((target) => String(target.variantLabel || target.type)),
  )))
  const selectedWriteType = computed(() => selectedCandidateTypes.value.length === 1
    ? selectedWriteTargets.value[0]?.type ?? store.exactScanType
    : store.exactScanType)
  const hasSelectedWriteTargets = computed(() => selectedCandidateAddresses.value.length > 0)
  const writeTargetLabel = computed(() => {
    if (!hasSelectedWriteTargets.value) return ''
    const knownCount = selectedCandidateRecords.value.length
    const typeNote = selectedCandidateTypes.value.length === 1
      ? selectedCandidateTypes.value[0]
      : (selectedWriteHasVariants.value ? t('expert.typeNoteAutoSource') : t('expert.typeNoteChosenType'))
    const suffix = knownCount < selectedCandidateAddresses.value.length ? t('expert.someOffPageSuffix') : ''
    return t('expert.selectedAddressesLabel', { count: selectedCandidateAddresses.value.length, typeNote, suffix })
  })
  const canWriteFromPanel = computed(() => hasSelectedWriteTargets.value
    ? Boolean(store.writeValue.trim())
    : store.canWriteSelectedValue)
  const writeButtonLabel = computed(() => hasSelectedWriteTargets.value
    ? t('expert.writeButtonCount', { count: selectedCandidateAddresses.value.length })
    : t('expert.writeButton'))

  function setSelectedWriteTargets(targets: MemoryWriteTarget[]) {
    selectedCandidateAddresses.value = targets.map((target) => target.address)
    selectedWriteTargetOverrides.value = Object.fromEntries(targets.map((target) => [target.address, target]))
    syncSelectedWriteType()
  }

  function clearCandidateSelection() {
    selectedCandidateAddresses.value = []
    selectedWriteTargetOverrides.value = {}
  }

  function scrollToWritePanel() {
    void nextTick(() => {
      writePanelRef.value?.scrollIntoView({ behavior: 'smooth', block: 'start' })
    })
  }

  function useCandidateInAssistant(address: string, type: string) {
    store.selectCandidate(address, type)
    void store.useSuggestedAddresses([{ address }])
  }

  function isCandidateSelected(address: string) {
    return selectedCandidateAddresses.value.includes(address)
  }

  function toggleCandidateSelection(address: string) {
    if (isCandidateSelected(address)) {
      selectedCandidateAddresses.value = selectedCandidateAddresses.value.filter((item) => item !== address)
      const { [address]: _removed, ...rest } = selectedWriteTargetOverrides.value
      selectedWriteTargetOverrides.value = rest
      syncSelectedWriteType()
      return
    }
    selectedCandidateAddresses.value = [...selectedCandidateAddresses.value, address]
    const { [address]: _removed, ...rest } = selectedWriteTargetOverrides.value
    selectedWriteTargetOverrides.value = rest
    syncSelectedWriteType()
  }

  function toggleCurrentPageSelection() {
    const pageAddresses = currentPageCandidates.value.map((candidate) => candidate.address)
    const allPageSelected = pageAddresses.length > 0
      && pageAddresses.every((address) => selectedCandidateAddresses.value.includes(address))
    if (allPageSelected) {
      selectedCandidateAddresses.value = selectedCandidateAddresses.value.filter((address) => !pageAddresses.includes(address))
      selectedWriteTargetOverrides.value = Object.fromEntries(
        Object.entries(selectedWriteTargetOverrides.value).filter(([address]) => !pageAddresses.includes(address)),
      )
      syncSelectedWriteType()
      return
    }
    selectedCandidateAddresses.value = Array.from(new Set([...selectedCandidateAddresses.value, ...pageAddresses]))
    selectedWriteTargetOverrides.value = Object.fromEntries(
      Object.entries(selectedWriteTargetOverrides.value).filter(([address]) => !pageAddresses.includes(address)),
    )
    syncSelectedWriteType()
  }

  function syncSelectedWriteType() {
    const selected = currentPageCandidates.value.filter((candidate) => selectedCandidateAddresses.value.includes(candidate.address))
    const types = Array.from(new Set(selected.map((candidate) => String(candidate.type))))
    if (types.length === 1) {
      store.exactScanType = types[0]
    }
    if (selected.length > 0) {
      store.selectedCandidateAddress = selected[0].address
    }
  }

  function useSelectedCandidatesInAssistant() {
    if (selectedCandidateAddresses.value.length === 0) return
    void store.useSuggestedAddresses(selectedCandidateAddresses.value.map((address) => ({ address })))
  }

  function writeSelectedCandidates() {
    if (selectedCandidateAddresses.value.length === 0 || !store.writeValue.trim()) return
    if (selectedWriteHasVariants.value) {
      void store.writeSelectedTargets(selectedWriteTargets.value, store.writeValue)
      return
    }
    void store.writeSelectedAddresses(selectedCandidateAddresses.value, selectedWriteType.value, store.writeValue)
  }

  function writeSelectedCandidatesAtomic() {
    if (selectedCandidateAddresses.value.length === 0 || !store.writeValue.trim()) return
    void store.writeSelectedAtomic(selectedCandidateAddresses.value, selectedWriteType.value, store.writeValue)
  }

  async function writeSelectedCandidateKernel() {
    const address = selectedCandidateAddresses.value[0]
    if (!address || !store.writeValue.trim()) return
    await store.executeCheckpointKernelWrite({ address, value: store.writeValue, type: selectedWriteType.value })
  }

  function writeFromPanel() {
    if (hasSelectedWriteTargets.value) {
      writeSelectedCandidates()
      return
    }
    void store.writeSelectedValue()
  }

  function watchOrRefreshCandidate(address: string, type: string) {
    const watched = watchedCandidate(address)
    if (watched) {
      void store.refreshWatchedAddress(address)
      return
    }
    store.addAddressToWatch(address, type)
    if (!store.watchLiveEnabled) store.setWatchLiveEnabled(true)
  }

  function watchSelectedCandidates() {
    if (selectedCandidateAddresses.value.length === 0) return
    store.addAddressesToWatch(selectedWriteTargets.value.map((target) => ({
      address: target.address,
      type: target.type,
    })))
    if (!store.watchLiveEnabled) store.setWatchLiveEnabled(true)
  }

  function watchCurrentCandidatePage() {
    if (currentPageCandidates.value.length === 0) return
    store.addAddressesToWatch(currentPageCandidates.value.map((candidate) => ({
      address: candidate.address,
      type: String(candidate.type),
    })))
    if (!store.watchLiveEnabled) store.setWatchLiveEnabled(true)
  }

  function watchedCandidate(address: string) {
    return store.watchedAddresses.find((item) => item.address === address)
  }

  return {
    selectedCandidateAddresses,
    selectedWriteTargetOverrides,
    writePanelRef,
    currentPageCandidates,
    displayedCandidates,
    selectedCandidateRecords,
    selectedWriteTargets,
    selectedWriteHasVariants,
    writePlan,
    writeFailures,
    selectedCandidateTypes,
    selectedWriteType,
    hasSelectedWriteTargets,
    writeTargetLabel,
    canWriteFromPanel,
    writeButtonLabel,
    setSelectedWriteTargets,
    clearCandidateSelection,
    scrollToWritePanel,
    useCandidateInAssistant,
    isCandidateSelected,
    toggleCandidateSelection,
    toggleCurrentPageSelection,
    syncSelectedWriteType,
    useSelectedCandidatesInAssistant,
    writeSelectedCandidates,
    writeSelectedCandidatesAtomic,
    writeSelectedCandidateKernel,
    writeFromPanel,
    watchOrRefreshCandidate,
    watchSelectedCandidates,
    watchCurrentCandidatePage,
    watchedCandidate,
  }
}
