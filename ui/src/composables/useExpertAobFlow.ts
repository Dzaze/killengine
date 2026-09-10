import { nextTick, ref } from 'vue'
import { useAppStore } from '@/stores/app'
import { i18n } from '@/i18n'
import {
  backend,
  type AobScanResult,
  type AobSignatureResult,
  type BackwardDisassemblyResult,
  type CandidateFieldTestResult,
  type CodePatchResult,
  type CodePatchSuggestion,
  type CodePatchSuggestionResult,
} from '@/services/backend'
import { cleanTrainerName, formatNumber } from '@/utils/format'

const { t } = i18n.global

function appStore() {
  return useAppStore()
}
// AOB signatures — base du futur trainer engine.
const aobPattern = ref('')
const aobExecutableOnly = ref(true)
const aobImageOnly = ref(true)
const aobMaxResults = ref(200)
const aobBusy = ref(false)
const aobResult = ref<AobScanResult | null>(null)
const aobStabilizeBusy = ref(false)
const aobStabilizeResult = ref<Record<string, unknown> | null>(null)
const aobSignatureBusy = ref(false)
const aobSignatureResult = ref<AobSignatureResult | null>(null)
const disassembleBackwardBusy = ref(false)
const disassembleBackwardResult = ref<BackwardDisassemblyResult | null>(null)
const testCandidateFieldsBusy = ref(false)
const testCandidateFieldsResult = ref<CandidateFieldTestResult | null>(null)
// Message affiché quand generateAobSignatureFromHit() a délibérément SAUTÉ le
// scan auto-enchaîné parce que le pattern stable est trop faible (level
// "weak") pour être fiable — le pattern reste pré-rempli dans aobPattern, le
// bouton "Scanner AOB" manuel reste disponible si l'utilisateur veut quand même.
const aobAutoScanSkippedReason = ref('')
const codePatchAddress = ref('')
const codePatchBytes = ref('90 90')
const codePatchBusy = ref(false)
const codePatchResult = ref<CodePatchResult | null>(null)
const codePatchSuggestBusy = ref(false)
const codePatchSuggestionResult = ref<CodePatchSuggestionResult | null>(null)
// Suggestion "Forcer une valeur" en attente de saisie (needsValueInput) et
// valeur tapée par l'utilisateur pour elle — séparés de codePatchBytes tant
// que la valeur n'a pas été appliquée, pour ne jamais écraser silencieusement
// des bytes déjà choisis manuellement.
const valueOverrideSuggestion = ref<CodePatchSuggestion | null>(null)
const valueOverrideInput = ref('')
const valueOverrideError = ref('')
// "Forcer une valeur (hook)" : marche même quand la source de l'écriture est
// un registre (donc sans immédiat à substituer par valueOverrideSuggestion
// ci-dessus) — installe un trampoline + redirige le site via
// forceWriteInstructionValue. hit gardé pour ré-afficher le RIP ciblé.
const forceHookTargetHit = ref<Record<string, unknown> | null>(null)
const forceHookValueInput = ref('')
const forceHookBusy = ref(false)
const forceHookResult = ref<Record<string, unknown> | null>(null)
const codePatchProfileName = ref('')
const codePatchProfilePatchName = ref('')
const codePatchProfileDescription = ref('')
const codePatchProfileBusy = ref(false)
const codePatchProfileResult = ref<Record<string, unknown> | null>(null)
const codePatchTrainerFlowBusy = ref(false)
const selectedFindWhatWritesRip = ref('')

async function scanAobSignature() {
  const pattern = aobPattern.value.trim()
  if (!pattern) return
  aobBusy.value = true
  aobResult.value = null
  try {
    const controller = backend.getController()
    if (!controller.scanAobPattern) {
      aobResult.value = { success: false, matches: [], error: t('expertAobFlow.backendMethodUnavailable') }
      return
    }
    aobResult.value = await controller.scanAobPattern(pattern, {
      executableOnly: aobExecutableOnly.value,
      imageOnly: aobImageOnly.value,
      maxResults: aobMaxResults.value,
    })
  } catch (e) {
    aobResult.value = { success: false, matches: [], error: String(e) }
  } finally {
    aobBusy.value = false
  }
}

async function scanAobPatternCandidate(pattern: string, maxResults = 1000): Promise<AobScanResult> {
  const controller = backend.getController()
  if (!controller.scanAobPattern) {
    return { success: false, matches: [], error: t('expertAobFlow.backendMethodUnavailable') }
  }
  return controller.scanAobPattern(pattern, {
    executableOnly: aobExecutableOnly.value,
    imageOnly: aobImageOnly.value,
    maxResults,
  })
}

async function stabilizeSelectedAobSignature() {
  const address = codePatchAddress.value.trim() || selectedFindWhatWritesRip.value.trim()
  if (!address) return
  aobStabilizeBusy.value = true
  aobStabilizeResult.value = null
  try {
    const controller = backend.getController()
    if (!controller.generateAobSignature) {
      aobStabilizeResult.value = { success: false, error: t('expertAobFlow.backendMethodUnavailable') }
      return
    }

    const tested: Array<Record<string, unknown>> = []
    const patterns: string[] = []
    const stablePattern = codePatchSuggestionResult.value?.stableAobPattern?.trim()
    if (stablePattern) patterns.push(stablePattern)

    for (const beforeBytes of [0, 4, 8, 12]) {
      for (const length of [16, 24, 32, 48, 64]) {
        const signature = await controller.generateAobSignature(address, { beforeBytes, length })
        if (signature.success && signature.pattern && !patterns.includes(signature.pattern)) {
          patterns.push(signature.pattern)
        }
      }
    }

    let best: { pattern: string, scan: AobScanResult } | null = null
    for (const pattern of patterns) {
      const scan = await scanAobPatternCandidate(pattern, 1000)
      const matchesFound = Number(scan.matchesFound ?? scan.matches?.length ?? 0)
      tested.push({
        pattern,
        matchesFound,
        patternBytes: scan.patternBytes,
        success: scan.success,
        partial: scan.partial,
        error: scan.error,
      })
      if (scan.success && matchesFound === 1) {
        best = { pattern, scan }
        break
      }
      if (scan.success && matchesFound > 0 && (!best || matchesFound < Number(best.scan.matchesFound ?? Number.MAX_SAFE_INTEGER))) {
        best = { pattern, scan }
      }
    }

    if (best) {
      aobPattern.value = best.pattern
      aobResult.value = best.scan
    }
    const matchesFound = Number(best?.scan.matchesFound ?? best?.scan.matches?.length ?? 0)
    aobStabilizeResult.value = {
      success: Boolean(best && matchesFound === 1),
      pattern: best?.pattern ?? '',
      matchesFound,
      tested,
      error: best && matchesFound !== 1
        ? t('expertAobFlow.noUniqueSignature', { count: formatNumber(matchesFound) })
        : (!best ? t('expertAobFlow.noUsableSignatureGenerated') : ''),
    }
  } catch (e) {
    aobStabilizeResult.value = { success: false, error: String(e) }
  } finally {
    aobStabilizeBusy.value = false
  }
}

async function generateAobSignatureFromHit(hit: Record<string, unknown>) {
  const rip = String(hit.instructionPointer ?? '').trim()
  if (!rip) return
  selectedFindWhatWritesRip.value = rip
  aobSignatureBusy.value = true
  aobSignatureResult.value = null
  aobAutoScanSkippedReason.value = ''
  codePatchSuggestBusy.value = true
  codePatchSuggestionResult.value = null
  valueOverrideSuggestion.value = null
  valueOverrideInput.value = ''
  valueOverrideError.value = ''
  forceHookTargetHit.value = null
  forceHookResult.value = null
  codePatchAddress.value = rip
  appStore().memoryPreviewAddress = rip
  try {
    const controller = backend.getController()
    if (!controller.generateAobSignature) {
      aobSignatureResult.value = { success: false, error: t('expertAobFlow.backendMethodUnavailable') }
      return
    }
    const result = await controller.generateAobSignature(rip, {
      beforeBytes: 0,
      length: 24,
    })
    aobSignatureResult.value = result
    let stablePatternIsWeak = false
    if (controller.suggestCodePatches) {
      const suggestionResult = await controller.suggestCodePatches(rip, { maxBytes: 16 })
      codePatchSuggestionResult.value = suggestionResult
      const firstSafe = suggestionResult.suggestions?.find((suggestion) => !suggestion.risky)
      if (suggestionResult.success && firstSafe) {
        codePatchBytes.value = firstSafe.bytesText
      }
      if (suggestionResult.success && suggestionResult.stableAobPattern) {
        aobPattern.value = suggestionResult.stableAobPattern
        // Le pattern "stable" vient du decodage d'UNE seule instruction : pour
        // un mov [mem], reg typique, il ne reste souvent que 2-3 octets fixes
        // (opcode + ModRM) une fois les offsets/registres wildcardes. Un scan
        // executable+image avec un pattern aussi court remonte des centaines
        // de matches sans rapport — pas une vraie signature. On ne lance pas
        // le scan auto dans ce cas, on prévient l'utilisateur pourquoi.
        stablePatternIsWeak = suggestionResult.signatureQuality?.level === 'weak'
      }
    } else {
      codePatchSuggestionResult.value = { success: false, suggestions: [], error: t('expertAobFlow.backendMethodUnavailable') }
    }
    if (!aobPattern.value.trim() && result.success && result.pattern) {
      aobPattern.value = result.pattern
    }
    if (aobPattern.value.trim()) {
      if (stablePatternIsWeak) {
        aobAutoScanSkippedReason.value = t('expertAobFlow.autoScanSkippedWeakSignature')
      } else {
        await scanAobSignature()
      }
      void nextTick(() => {
        document.querySelector('.aob-panel')?.scrollIntoView({ behavior: 'smooth', block: 'start' })
      })
    }
  } catch (e) {
    aobSignatureResult.value = { success: false, error: String(e) }
    codePatchSuggestionResult.value = { success: false, suggestions: [], error: String(e) }
  } finally {
    aobSignatureBusy.value = false
    codePatchSuggestBusy.value = false
  }
}

async function disassembleBackwardFromHit(hit: Record<string, unknown>) {
  const rip = String(hit.instructionPointer ?? '').trim()
  if (!rip) return
  disassembleBackwardBusy.value = true
  disassembleBackwardResult.value = null
  try {
    const controller = backend.getController()
    if (!controller.disassembleBackward) {
      disassembleBackwardResult.value = { success: false, error: t('expertAobFlow.backendMethodUnavailable') }
      return
    }
    disassembleBackwardResult.value = await controller.disassembleBackward(rip, {})
  } catch (e) {
    disassembleBackwardResult.value = { success: false, error: String(e) }
  } finally {
    disassembleBackwardBusy.value = false
  }
}

// Teste automatiquement lequel des champs candidats tient réellement (écrit
// une valeur test, attend, relit, restaure) — pas besoin d'avoir cliqué
// "Désassembler en amont" d'abord, testCandidateFieldsAsync refait la
// résolution des champs en interne à partir du RIP et de l'adresse écrite.
async function testCandidateFieldsFromHit(hit: Record<string, unknown>) {
  const rip = String(hit.instructionPointer ?? '').trim()
  const watchedAddress = String(hit.address ?? '').trim()
  if (!rip || !watchedAddress) return
  testCandidateFieldsBusy.value = true
  testCandidateFieldsResult.value = null
  try {
    testCandidateFieldsResult.value = await appStore().executeCandidateFieldTest(rip, watchedAddress)
  } catch (e) {
    testCandidateFieldsResult.value = { success: false, error: String(e) }
  } finally {
    testCandidateFieldsBusy.value = false
  }
}

function findWhatWritesHitKey(hit: Record<string, unknown>) {
  return `${hit.instructionPointer}:${hit.threadId}:${hit.address}`
}

function isSelectedFindWhatWritesHit(hit: Record<string, unknown>) {
  return selectedFindWhatWritesRip.value !== '' && selectedFindWhatWritesRip.value === String(hit.instructionPointer ?? '').trim()
}

function previewFindWhatWritesHit(hit: Record<string, unknown>) {
  const rip = String(hit.instructionPointer ?? '').trim()
  if (!rip) return
  selectedFindWhatWritesRip.value = rip
  codePatchAddress.value = rip
  appStore().memoryPreviewAddress = rip
  void appStore().readMemoryPreview(rip, 128)
  void nextTick(() => {
    document.querySelector('.aob-panel')?.scrollIntoView({ behavior: 'smooth', block: 'start' })
  })
}

async function copyFindWhatWritesRip(hit: Record<string, unknown>) {
  const rip = String(hit.instructionPointer ?? '').trim()
  if (!rip) return
  selectedFindWhatWritesRip.value = rip
  await navigator.clipboard?.writeText(`0x${rip}`)
}

function useAobMatchAddress(address: string) {
  appStore().memoryPreviewAddress = address
  codePatchAddress.value = address
  void appStore().readMemoryPreview(address, 128)
}

function bookmarkAobMatch(match: Record<string, unknown>) {
  const address = String(match.address ?? '').replace(/^0x/i, '').toUpperCase()
  if (!address) return
  appStore().addWorkspaceBookmark({
    kind: 'aob',
    label: `AOB 0x${address}`,
    address,
    type: 'Code',
    note: `${String(match.module || match.memoryType || 'code')} ${match.moduleOffset ? `+0x${String(match.moduleOffset)}` : ''}`.trim(),
    payload: {
      aobPattern: aobPattern.value.trim(),
      module: match.module,
      moduleOffset: match.moduleOffset,
      protection: match.protection,
      executableOnly: aobExecutableOnly.value,
      imageOnly: aobImageOnly.value,
    },
  })
}

function bookmarkCurrentCodePatch() {
  const address = codePatchAddress.value.trim().replace(/^0x/i, '').toUpperCase()
  if (!address) return
  const suggestion = selectedPatchSuggestion()
  const quality = currentAobQuality()
  appStore().addWorkspaceBookmark({
    kind: 'aob',
    label: codePatchProfilePatchName.value.trim() || `Patch 0x${address}`,
    address,
    type: 'CodePatch',
    value: codePatchBytes.value.trim(),
    note: codePatchSuggestionResult.value?.disassembly || suggestion?.description || codePatchProfileDescription.value.trim(),
    payload: {
      patchBytes: codePatchBytes.value.trim(),
      aobPattern: (codePatchSuggestionResult.value?.stableAobPattern || aobPattern.value).trim(),
      originalBytes: codePatchResult.value?.originalBytes || codePatchSuggestionResult.value?.bytes || '',
      disassembly: codePatchSuggestionResult.value?.disassembly || '',
      riskLevel: suggestion?.riskLevel || '',
      profileName: codePatchProfileName.value.trim(),
      signatureQuality: quality,
      signatureScore: quality?.score,
      signatureLevel: quality?.level,
      signatureWarning: quality?.warning,
      signatureFixedBytes: quality?.fixedBytes,
      signatureWildcardBytes: quality?.wildcardBytes,
      signatureUniqueFixedBytes: quality?.uniqueFixedBytes,
      signatureFixedRatio: quality?.fixedRatio,
      trainerSafe: quality?.trainerSafe,
      signatureMatches: Number(aobResult.value?.matchesFound ?? 0) || undefined,
    },
  })
}

async function selectAobPatchAddress(address: string) {
  codePatchAddress.value = address
  appStore().memoryPreviewAddress = address
  await suggestSelectedCodePatches()
}

function useCodePatchSuggestion(suggestion: CodePatchSuggestion) {
  if (suggestion.needsValueInput) {
    valueOverrideSuggestion.value = suggestion
    valueOverrideInput.value = ''
    valueOverrideError.value = ''
    return
  }
  valueOverrideSuggestion.value = null
  codePatchBytes.value = suggestion.bytesText
}

function parseValueOverrideInput(text: string): bigint | null {
  const trimmed = text.trim()
  if (!trimmed) return null
  try {
    return BigInt(trimmed)
  } catch {
    return null
  }
}

function applyValueOverrideSuggestion() {
  const suggestion = valueOverrideSuggestion.value
  valueOverrideError.value = ''
  if (!suggestion || suggestion.valueOffset == null || !suggestion.valueSize) return
  const originalHex = (codePatchSuggestionResult.value?.bytes || suggestion.bytesText).replace(/\s+/g, '')
  const originalBytes = originalHex.match(/../g)?.map((byte) => parseInt(byte, 16)) ?? []
  if (originalBytes.length < suggestion.valueOffset + suggestion.valueSize) {
    valueOverrideError.value = t('expertAobFlow.insufficientInstructionBytes')
    return
  }
  const parsed = parseValueOverrideInput(valueOverrideInput.value)
  if (parsed === null) {
    valueOverrideError.value = t('expertAobFlow.invalidOverrideValue')
    return
  }
  // Tronque a la largeur du champ immediat (modulo 2^(size*8), les BigInt
  // negatifs se masquent en complement a deux) plutot que de rejeter
  // silencieusement : le comportement est le meme qu'un patch manuel "je
  // sais ce que je fais", avec un avertissement explicite si la valeur
  // demandee ne rentrait pas telle quelle dans le champ.
  const widthBits = BigInt(suggestion.valueSize * 8)
  const mask = (1n << widthBits) - 1n
  const truncated = parsed & mask
  const minSigned = -(1n << (widthBits - 1n))
  const maxUnsigned = (1n << widthBits) - 1n
  if (parsed < minSigned || parsed > maxUnsigned) {
    valueOverrideError.value = t('expertAobFlow.overrideValueOutOfRange', { size: suggestion.valueSize, truncated: truncated.toString(16) })
  }
  const patched = [...originalBytes]
  for (let i = 0; i < suggestion.valueSize; ++i) {
    patched[suggestion.valueOffset + i] = Number((truncated >> BigInt(i * 8)) & 0xffn)
  }
  codePatchBytes.value = patched.map((byte) => byte.toString(16).padStart(2, '0').toUpperCase()).join(' ')
}

function selectForceHookTarget(hit: Record<string, unknown>) {
  forceHookTargetHit.value = hit
  forceHookValueInput.value = ''
  forceHookResult.value = null
}

async function applyForceHookValue() {
  const hit = forceHookTargetHit.value
  const suggestion = codePatchSuggestionResult.value
  if (!hit || !suggestion?.memBaseRegister || !forceHookValueInput.value.trim()) return
  forceHookBusy.value = true
  forceHookResult.value = null
  try {
    const controller = backend.getController()
    if (!controller.forceWriteInstructionValue) {
      forceHookResult.value = { success: false, error: t('expertAobFlow.backendMethodUnavailable') }
      return
    }
    forceHookResult.value = await controller.forceWriteInstructionValue(
      String(hit.instructionPointer ?? ''),
      Number(suggestion.instructionLength ?? 0),
      suggestion.memBaseRegister,
      Number(suggestion.memDisplacement ?? 0),
      appStore().exactScanType,
      forceHookValueInput.value.trim(),
    )
  } catch (e) {
    forceHookResult.value = { success: false, error: String(e) }
  } finally {
    forceHookBusy.value = false
  }
}

function defaultTrainerProfileName() {
  return cleanTrainerName(appStore().processName || 'Trainer', 'Trainer')
}

function defaultPatchNameFromHit(hit: Record<string, unknown>) {
  const moduleName = cleanTrainerName(String(hit.module || aobSignatureResult.value?.module || 'Patch'), 'Patch')
  const offset = String(hit.moduleOffset || aobSignatureResult.value?.moduleOffset || selectedFindWhatWritesRip.value || '0').toUpperCase()
  return `${moduleName}_${offset}`
}

function selectedPatchSuggestion() {
  const patchBytes = codePatchBytes.value.trim()
  return codePatchSuggestionResult.value?.suggestions?.find((suggestion) => suggestion.bytesText === patchBytes)
}

function currentAobQuality() {
  return codePatchSuggestionResult.value?.signatureQuality
    || aobResult.value?.signatureQuality
    || aobSignatureResult.value?.signatureQuality
}

function aobQualityBlocksTrainer() {
  const quality = currentAobQuality()
  if (!quality) return ''
  const score = Number(quality.score ?? 0)
  const fixedBytes = Number(quality.fixedBytes ?? 0)
  if (fixedBytes < 3 || score < 35) {
    return t('expertAobFlow.signatureTooWeak', { score, fixedBytes })
  }
  return ''
}

async function suggestSelectedCodePatches() {
  const address = codePatchAddress.value.trim()
  if (!address) return
  codePatchSuggestBusy.value = true
  codePatchSuggestionResult.value = null
  valueOverrideSuggestion.value = null
  valueOverrideInput.value = ''
  valueOverrideError.value = ''
  try {
    const controller = backend.getController()
    if (!controller.suggestCodePatches) {
      codePatchSuggestionResult.value = { success: false, suggestions: [], error: t('expertAobFlow.backendMethodUnavailable') }
      return
    }
    const result = await controller.suggestCodePatches(address, { maxBytes: 16 })
    codePatchSuggestionResult.value = result
    const firstSafe = result.suggestions?.find((suggestion) => !suggestion.risky)
    if (result.success && firstSafe) {
      codePatchBytes.value = firstSafe.bytesText
    }
    if (result.success && result.stableAobPattern) {
      aobPattern.value = result.stableAobPattern
    }
  } catch (e) {
    codePatchSuggestionResult.value = { success: false, suggestions: [], error: String(e) }
  } finally {
    codePatchSuggestBusy.value = false
  }
}

async function applySelectedCodePatch() {
  const address = codePatchAddress.value.trim()
  const bytes = codePatchBytes.value.trim()
  if (!address || !bytes) return
  if (!await appStore().confirmRiskAction('patch', t('expertAobFlow.patchCodeTitle'), t('expertAobFlow.patchCodeDesc', { address: address.replace(/^0x/i, ''), bytes }))) return
  codePatchBusy.value = true
  codePatchResult.value = null
  try {
    const controller = backend.getController()
    if (!controller.applyCodePatch) {
      codePatchResult.value = { success: false, error: t('expertAobFlow.backendMethodUnavailable') }
      return
    }
    codePatchResult.value = await controller.applyCodePatch(address, bytes, { verify: true })
  } catch (e) {
    codePatchResult.value = { success: false, error: String(e) }
  } finally {
    codePatchBusy.value = false
  }
}

async function restoreSelectedCodePatch() {
  const address = codePatchAddress.value.trim()
  if (!address) return
  codePatchBusy.value = true
  codePatchResult.value = null
  try {
    const controller = backend.getController()
    if (!controller.restoreCodePatch) {
      codePatchResult.value = { success: false, error: t('expertAobFlow.backendMethodUnavailable') }
      return
    }
    codePatchResult.value = await controller.restoreCodePatch(address)
  } catch (e) {
    codePatchResult.value = { success: false, error: String(e) }
  } finally {
    codePatchBusy.value = false
  }
}

async function saveSelectedCodePatchProfile() {
  const profileName = codePatchProfileName.value.trim()
  const patchName = codePatchProfilePatchName.value.trim()
  const address = codePatchAddress.value.trim()
  const pattern = (codePatchSuggestionResult.value?.stableAobPattern || aobPattern.value).trim()
  const patchBytes = codePatchBytes.value.trim()
  if (!profileName || !patchName || !address || !pattern || !patchBytes) return
  const qualityError = aobQualityBlocksTrainer()
  if (qualityError) {
    codePatchProfileResult.value = {
      success: false,
      profileName,
      patchName,
      error: qualityError,
    }
    return
  }

  codePatchProfileBusy.value = true
  codePatchProfileResult.value = null
  try {
    const controller = backend.getController()
    if (!controller.saveProfileCodePatch) {
      codePatchProfileResult.value = { success: false, error: t('expertAobFlow.backendMethodUnavailable') }
      return
    }
    codePatchProfileResult.value = await controller.saveProfileCodePatch(
      profileName,
      patchName,
      address,
      pattern,
      patchBytes,
      {
        originalBytes: codePatchResult.value?.originalBytes || codePatchSuggestionResult.value?.bytes || '',
        disassembly: codePatchSuggestionResult.value?.disassembly || '',
        riskLevel: selectedPatchSuggestion()?.riskLevel || '',
        description: codePatchProfileDescription.value.trim(),
        signatureQuality: currentAobQuality(),
      },
    )
  } catch (e) {
    codePatchProfileResult.value = { success: false, error: String(e) }
  } finally {
    codePatchProfileBusy.value = false
  }
}

async function saveTrainerPatchFromHit(hit: Record<string, unknown>) {
  if (codePatchTrainerFlowBusy.value) return
  codePatchTrainerFlowBusy.value = true
  codePatchProfileResult.value = null
  try {
    await generateAobSignatureFromHit(hit)
    if (Number(aobResult.value?.matchesFound ?? 0) !== 1) {
      await stabilizeSelectedAobSignature()
    }

    const patchBytes = codePatchBytes.value.trim()
    const pattern = (codePatchSuggestionResult.value?.stableAobPattern || aobPattern.value).trim()
    if (!patchBytes || !pattern || !codePatchAddress.value.trim()) {
      codePatchProfileResult.value = {
        success: false,
        error: t('expertAobFlow.incompleteAnalysis'),
      }
      return
    }

    if (!codePatchProfileName.value.trim()) {
      codePatchProfileName.value = defaultTrainerProfileName()
    }
    if (!codePatchProfilePatchName.value.trim()) {
      codePatchProfilePatchName.value = defaultPatchNameFromHit(hit)
    }
    if (!codePatchProfileDescription.value.trim()) {
      const suggestion = selectedPatchSuggestion()
      codePatchProfileDescription.value = [
        codePatchSuggestionResult.value?.disassembly || t('expertAobFlow.patchFromFindWhatWrites'),
        suggestion?.label ? t('expertAobFlow.suggestionLabel', { label: suggestion.label }) : '',
        hit.address ? t('expertAobFlow.observedTarget', { address: hit.address }) : '',
      ].filter(Boolean).join(' | ')
    }

    const matchesFound = Number(aobResult.value?.matchesFound ?? 0)
    const qualityError = aobQualityBlocksTrainer()
    if (qualityError) {
      codePatchProfileResult.value = {
        success: false,
        profileName: codePatchProfileName.value.trim(),
        patchName: codePatchProfilePatchName.value.trim(),
        error: qualityError,
      }
      return
    }
    if (!aobResult.value?.success || matchesFound !== 1) {
      codePatchProfileResult.value = {
        success: false,
        profileName: codePatchProfileName.value.trim(),
        patchName: codePatchProfilePatchName.value.trim(),
        error: matchesFound === 0
          ? t('expertAobFlow.signatureNotFound')
          : t('expertAobFlow.signatureNotUnique', { count: formatNumber(matchesFound) }),
      }
      return
    }

    await saveSelectedCodePatchProfile()
  } finally {
    codePatchTrainerFlowBusy.value = false
  }
}

export function useExpertAobFlow() {
  function resetAobFlowState() {
    aobPattern.value = ''
    aobResult.value = null
    aobStabilizeResult.value = null
    aobSignatureResult.value = null
    aobAutoScanSkippedReason.value = ''
    codePatchAddress.value = ''
    codePatchBytes.value = '90 90'
    codePatchResult.value = null
    codePatchSuggestionResult.value = null
    valueOverrideSuggestion.value = null
    valueOverrideInput.value = ''
    valueOverrideError.value = ''
    forceHookTargetHit.value = null
    forceHookValueInput.value = ''
    forceHookResult.value = null
    codePatchProfileName.value = ''
    codePatchProfilePatchName.value = ''
    codePatchProfileDescription.value = ''
    codePatchProfileResult.value = null
    disassembleBackwardResult.value = null
    testCandidateFieldsResult.value = null
    selectedFindWhatWritesRip.value = ''
  }

  return {
    aobPattern,
    aobExecutableOnly,
    aobImageOnly,
    aobMaxResults,
    aobBusy,
    aobResult,
    aobStabilizeBusy,
    aobStabilizeResult,
    aobSignatureBusy,
    aobSignatureResult,
    disassembleBackwardBusy,
    disassembleBackwardResult,
    testCandidateFieldsBusy,
    testCandidateFieldsResult,
    aobAutoScanSkippedReason,
    codePatchAddress,
    codePatchBytes,
    codePatchBusy,
    codePatchResult,
    codePatchSuggestBusy,
    codePatchSuggestionResult,
    valueOverrideSuggestion,
    valueOverrideInput,
    valueOverrideError,
    forceHookTargetHit,
    forceHookValueInput,
    forceHookBusy,
    forceHookResult,
    codePatchProfileName,
    codePatchProfilePatchName,
    codePatchProfileDescription,
    codePatchProfileBusy,
    codePatchProfileResult,
    codePatchTrainerFlowBusy,
    selectedFindWhatWritesRip,
    scanAobSignature,
    scanAobPatternCandidate,
    stabilizeSelectedAobSignature,
    generateAobSignatureFromHit,
    disassembleBackwardFromHit,
    testCandidateFieldsFromHit,
    findWhatWritesHitKey,
    isSelectedFindWhatWritesHit,
    previewFindWhatWritesHit,
    copyFindWhatWritesRip,
    useAobMatchAddress,
    bookmarkAobMatch,
    bookmarkCurrentCodePatch,
    selectAobPatchAddress,
    useCodePatchSuggestion,
    applyValueOverrideSuggestion,
    selectForceHookTarget,
    applyForceHookValue,
    defaultTrainerProfileName,
    defaultPatchNameFromHit,
    selectedPatchSuggestion,
    currentAobQuality,
    aobQualityBlocksTrainer,
    suggestSelectedCodePatches,
    applySelectedCodePatch,
    restoreSelectedCodePatch,
    saveSelectedCodePatchProfile,
    saveTrainerPatchFromHit,
    resetAobFlowState,
  }
}