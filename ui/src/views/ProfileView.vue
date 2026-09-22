<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { useAppStore } from '@/stores/app'
import { backend } from '@/services/backend'
import type { ClrFieldInfo, ProfileDurabilityEntry, ProfileDurabilityReport, ProfileResolutionPlanOptions, ProfileKnowledgeNoteOptions, ProfileKnowledgeNoteKind, ProfileKnowledgeNotesResult } from '@/services/backend'
import PanelIntro from '@/components/common/PanelIntro.vue'

const store = useAppStore()
const { t, locale } = useI18n()

// R2 messages kept local to the reserved ProfileView lane (FR/EN reactive).
const durabilityCopy: Record<string, [string, string]> = {
  title: ['Diagnostic de durabilité', 'Durability diagnostics'],
  inspect: ['Vérifier les profils sauvegardés', 'Check saved profile entries'],
  scope: ['Lecture seule des locators enregistrés. Les signatures AOB restent dans l’inspection des patchs. Les objets CLR nécessitent leur inspecteur dédié.', 'Read-only checks of stored locators. AOB signatures remain in patch inspection. CLR objects require their dedicated inspector.'],
  budget: ['Budget de lecture atteint : certains résultats restent non concluants.', 'Read budget reached: some results remain inconclusive.'],
  proof: ['Des conditions mémoire vérifiées ne prouvent pas l’effet recherché. Aucune alternative n’est appliquée automatiquement.', 'Matching memory conditions do not prove the intended effect. No alternative is applied automatically.'],
  configure: ['Définir les conditions', 'Set conditions'],
  discovery: ['Méthode de découverte', 'Discovery method'],
  bytes: ['Octets stables attendus (hex, 64 octets maximum)', 'Expected stable bytes (hex, up to 64 bytes)'],
  bytesHint: ['Laisser vide pour une valeur qui varie. Ne pas prendre un compteur courant pour une signature stable.', 'Leave empty for a changing value. A current counter value is not a stable signature.'],
  test: ['Test nécessaire avant réutilisation', 'Required test before reuse'],
  note: ['Observation ou provenance déclarée', 'Declared observation or provenance'],
  alternatives: ['Autres entrées à conserver comme pistes (8 maximum)', 'Other entries to retain as alternatives (up to 8)'],
  save: ['Enregistrer pour la version attachée', 'Save for the attached version'],
  saved: ['Conditions enregistrées ; aucune écriture effectuée sur la cible.', 'Conditions saved; no target memory was written.'],
  cancel: ['Annuler', 'Cancel'],
  baseline: ['Dernière référence enregistrée', 'Last recorded baseline'],
  observed: ['Octets observés', 'Observed bytes'],
  blocked: ['Action suspendue : résoudre le diagnostic avant réutilisation.', 'Action paused: resolve the diagnostic before reuse.'],
  failed: ['Diagnostic indisponible', 'Diagnostics unavailable'],
  no_process: ['Aucun processus attaché', 'No process attached'],
  too_many_entries: ['Profil trop volumineux (256 entrées maximum)', 'Profile too large (up to 256 entries)'],
  invalid_profile_name: ['Nom de profil invalide', 'Invalid profile name'],
  profile_not_found: ['Profil introuvable', 'Profile not found'],
  profile_save_failed: ['Enregistrement du profil impossible', 'Could not save the profile'],
  mock_backend: ['Diagnostic indisponible dans la démonstration', 'Diagnostics unavailable in the demo'],
  invalid_plan: ['Méthode et test requis ; vérifier la longueur des champs', 'Method and test required; check field lengths'],
  invalid_alternatives: ['Liste de pistes alternatives invalide', 'Invalid alternative list'],
  too_many_alternatives: ['Huit alternatives maximum', 'Up to eight alternatives'],
  alternative_not_unique: ['Nom alternatif absent ou non unique', 'Alternative name missing or not unique'],
  alternative_type_mismatch: ['Les alternatives doivent avoir le même type de valeur', 'Alternatives must use the same value type'],
  entry_not_found: ['Entrée introuvable', 'Entry not found'],
  ambiguous_entry_name: ['Nom d’entrée non unique', 'Entry name is not unique'],
  invalid_entry_kind: ['Type d’entrée invalide', 'Invalid entry kind'],
  unsupported_locator: ['Utiliser l’inspecteur dédié pour ce locator', 'Use the dedicated inspector for this locator'],
  ambiguous_module: ['Plusieurs modules portent ce nom', 'Multiple modules have this name'],
  invalid_probe: ['Paramètres de lecture invalides', 'Invalid read parameters'],
  module_missing: ['Module absent', 'Module missing'],
  module_offset_out_of_range: ['Offset hors du module', 'Offset outside the module'],
  invalid_pointer_chain: ['Chaîne invalide ou supérieure à 16 niveaux', 'Invalid chain or more than 16 levels'],
  pointer_unreadable: ['Pointeur illisible', 'Unreadable pointer'],
  address_unreadable: ['Adresse illisible', 'Unreadable address'],
  null_pointer: ['Pointeur nul', 'Null pointer'],
  address_overflow: ['Adresse hors limites', 'Address out of bounds'],
  unverified: ['Non vérifié : conditions ou empreintes insuffisantes', 'Unverified: insufficient conditions or fingerprints'],
  conditions_verified: ['Conditions mémoire vérifiées', 'Memory conditions verified'],
  wrong_process: ['Le processus attaché ne correspond pas au profil', 'Attached process does not match the profile'],
  invalid_conditions: ['Conditions invalides', 'Invalid conditions'],
  ambiguous: ['Plusieurs adresses possibles : ambiguïté', 'Multiple possible addresses: ambiguous'],
  inconclusive: ['Lecture incomplète ou locator non pris en charge', 'Incomplete read or unsupported locator'],
  conditions_mismatch: ['Les octets attendus ne correspondent plus', 'Expected bytes no longer match'],
  missing: ['Aucune adresse retrouvée parmi les pistes enregistrées', 'No address found among the stored alternatives'],
  version_changed: ['Version de l’exécutable ou du module différente', 'Executable or module version changed'],
  repair_candidate: ['Piste alternative retrouvée, à tester', 'Alternative candidate found, testing required'],
  session_changed: ['Adresse absolue issue d’une autre session', 'Absolute address from another session'],
  edit_conditions: ['Corriger les conditions enregistrées.', 'Correct the saved conditions.'],
  attach_expected_process: ['Attacher le processus correspondant.', 'Attach the matching process.'],
  disambiguate_candidates: ['Départager les candidats avec le test enregistré avant toute réparation.', 'Disambiguate candidates using the saved test before repairing.'],
  retry_read_or_inspect_manually: ['Relancer la lecture ou utiliser l’inspecteur adapté.', 'Retry reading or use the appropriate inspector.'],
  rediscover_target: ['Rechercher de nouveau la source et enregistrer une piste après vérification.', 'Rediscover the source and save a candidate after verification.'],
  revalidate_on_current_version: ['Vérifier les conditions et le test sur cette version avant de renouveler la référence.', 'Check the conditions and test on this version before renewing the baseline.'],
  test_alternative_before_replacing: ['Tester la piste proposée puis remplacer explicitement le locator depuis les outils de profil existants.', 'Test the proposed candidate, then explicitly replace the locator using the existing profile tools.'],
  rediscover_absolute_address: ['Retrouver l’adresse actuelle ; préférer un module ou une chaîne de pointeurs.', 'Rediscover the current address; prefer a module or pointer chain.'],
  record_and_check_stable_conditions: ['Définir une condition stable ; une adresse lisible seule reste non vérifiée.', 'Define a stable condition; a readable address alone remains unverified.'],
  verify_effect_separately: ['Exécuter le test enregistré pour vérifier l’effet indépendamment.', 'Run the saved test to verify the effect separately.'],
}

function durabilityText(key: string): string {
  return durabilityCopy[key]?.[locale.value.startsWith('fr') ? 0 : 1] ?? key
}

const durabilityReport = ref<ProfileDurabilityReport | null>(null)
const durabilityBusy = ref(false)
const durabilityError = ref('')
const durabilityEditor = ref<ProfileDurabilityEntry | null>(null)
const durabilityForm = ref<ProfileResolutionPlanOptions>({
  discoveryMethod: '', expectedBytes: '', validationTest: '', evidenceNote: '', alternativeNames: [],
})
let durabilityRequest = 0

async function inspectDurability(): Promise<ProfileDurabilityReport | null> {
  const name = selectedProfile.value
  if (!name || !store.isAttached) return null
  const request = ++durabilityRequest
  durabilityReport.value = null
  durabilityBusy.value = true
  durabilityError.value = ''
  try {
    const result = await backend.getController().inspectProfileDurability(name)
    if (request !== durabilityRequest || name !== selectedProfile.value || !store.isAttached) return null
    durabilityReport.value = result
    if (!result.success) durabilityError.value = result.errorCode ?? 'failed'
    return result
  } catch (error) {
    if (request === durabilityRequest) durabilityError.value = String(error)
    return null
  } finally {
    if (request === durabilityRequest) durabilityBusy.value = false
  }
}

function editDurability(entry: ProfileDurabilityEntry) {
  durabilityEditor.value = entry
  durabilityForm.value = {
    discoveryMethod: entry.plan.discoveryMethod ?? '',
    expectedBytes: entry.plan.expectedBytes ?? '',
    validationTest: entry.plan.validationTest ?? '',
    evidenceNote: entry.plan.evidenceNote ?? '',
    alternativeNames: [...(entry.plan.alternativeNames ?? [])],
  }
}

async function saveDurabilityPlan() {
  const entry = durabilityEditor.value
  const name = selectedProfile.value
  if (!entry || !name || !store.isAttached || durabilityBusy.value) return
  durabilityBusy.value = true
  try {
    const result = await backend.getController().saveProfileResolutionPlan(name, entry.entryKind, entry.name, durabilityForm.value)
    if (name !== selectedProfile.value) return
    if (!result.success) {
      durabilityError.value = result.errorCode ?? 'failed'
      return
    }
    durabilityEditor.value = null
    statusMessage.value = durabilityText('saved')
    await inspectDurability()
  } catch (error) {
    durabilityError.value = String(error)
  } finally {
    durabilityBusy.value = false
  }
}

// Fresh read before an explicit ProfileView action. This never selects or applies
// an alternative. Legacy/CLR flows keep their own checks outside this R2 lane.
async function checkDurabilityBeforeUse(kind: 'target' | 'patch', names: string[]): Promise<boolean> {
  const report = await inspectDurability()
  if (!report?.success) {
    statusMessage.value = durabilityText('failed')
    return false
  }
  for (const name of names) {
    const entry = report.entries?.find((item) => item.entryKind === kind && item.name === name)
    const legacyClr = kind === 'target' && profileTargets.value.some((target) =>
      target.name === name && target.locatorKind === 'clr_field')
    if (legacyClr) continue
    if (!entry || !['conditions_verified', 'unverified'].includes(entry.status)) {
      statusMessage.value = `${durabilityText('blocked')} ${name}: ${durabilityText(entry?.status ?? 'failed')}`
      return false
    }
  }
  return true
}

// R4 (PRODUIT-R) : mémoire d'enquête réutilisable, une lane distincte de R2 qui
// s'appuie sur le même contrôleur backend et le même profil sélectionné, mais
// n'écrit jamais un locator ni une preuve d'effet (voir ai/effect_proof_ledger.*
// pour la preuve d'effet en session, hors périmètre de ce fichier).
const knowledgeCopy: Record<string, [string, string]> = {
  title: ['Mémoire d’enquête', 'Investigation memory'],
  hint: ['Notes qui survivent aux redémarrages : ce qui a échoué et pourquoi, ce qui a marché, l’expérience qui a tranché, ou un doute à revérifier. Un changement de version ne supprime rien, il signale juste à revérifier.', 'Notes that survive restarts: what failed and why, what worked, the experiment that settled it, or a doubt to recheck. A version change does not delete anything, it just flags it for review.'],
  load: ['Charger les notes', 'Load notes'],
  kind_explained_failure: ['Échec expliqué', 'Explained failure'],
  kind_success_condition: ['Condition de réussite', 'Success condition'],
  kind_discriminating_experiment: ['Expérience discriminante', 'Discriminating experiment'],
  kind_recheck: ['À revérifier', 'Recheck'],
  descriptionLabel: ['Description', 'Description'],
  experimentLabel: ['Expérience (optionnel)', 'Experiment (optional)'],
  evidenceLabel: ['Preuve / observation (optionnel)', 'Evidence / observation (optional)'],
  add: ['Ajouter la note', 'Add note'],
  valid: ['Toujours valable pour cette version', 'Still valid for this version'],
  stale: ['Version différente : à revérifier', 'Different version: recheck needed'],
  unversioned: ['Non versionné (aucun process attaché à l’enregistrement)', 'Unversioned (no process attached when recorded)'],
  noNotes: ['Aucune note pour le moment.', 'No notes yet.'],
  no_profile: ['Aucun profil chargé', 'No profile loaded'],
  invalid_kind: ['Type de note invalide', 'Invalid note kind'],
  invalid_note: ['Description requise (2048 caractères maximum)', 'Description required (up to 2048 characters)'],
  too_many_notes: ['50 notes maximum pour cette entrée', 'Up to 50 notes for this entry'],
}

function knowledgeText(key: string): string {
  const local = knowledgeCopy[key]
  if (local) return local[locale.value.startsWith('fr') ? 0 : 1]
  return durabilityText(key)
}

function knowledgeEntryKey(entryKind: string, name: string): string {
  return `${entryKind}:${name}`
}

const knowledgeNotesByEntry = ref<Record<string, ProfileKnowledgeNotesResult>>({})
const knowledgeBusy = ref<Record<string, boolean>>({})
const knowledgeForm = ref<Record<string, ProfileKnowledgeNoteOptions>>({})

function emptyKnowledgeDraft(): ProfileKnowledgeNoteOptions {
  return { kind: 'recheck' as ProfileKnowledgeNoteKind, description: '', experiment: '', evidenceNote: '' }
}

function notesFor(entry: ProfileDurabilityEntry): ProfileKnowledgeNotesResult | undefined {
  return knowledgeNotesByEntry.value[knowledgeEntryKey(entry.entryKind, entry.name)]
}

async function loadKnowledgeNotes(entryKind: 'target' | 'patch', name: string) {
  const profileName = selectedProfile.value
  const controller = backend.getController()
  if (!profileName || !controller.getProfileKnowledgeNotes) return
  const key = knowledgeEntryKey(entryKind, name)
  knowledgeBusy.value = { ...knowledgeBusy.value, [key]: true }
  try {
    const result = await controller.getProfileKnowledgeNotes(profileName, entryKind, name)
    if (profileName !== selectedProfile.value) return
    knowledgeNotesByEntry.value = { ...knowledgeNotesByEntry.value, [key]: result }
  } catch (error) {
    knowledgeNotesByEntry.value = { ...knowledgeNotesByEntry.value, [key]: { success: false, errorCode: String(error) } }
  } finally {
    knowledgeBusy.value = { ...knowledgeBusy.value, [key]: false }
  }
}

async function addKnowledgeNote(entryKind: 'target' | 'patch', name: string) {
  const profileName = selectedProfile.value
  const controller = backend.getController()
  const key = knowledgeEntryKey(entryKind, name)
  const draft = knowledgeForm.value[key]
  if (!profileName || !controller.addProfileKnowledgeNote || !draft || !draft.description.trim()) return
  knowledgeBusy.value = { ...knowledgeBusy.value, [key]: true }
  try {
    const result = await controller.addProfileKnowledgeNote(profileName, entryKind, name, draft)
    if (profileName !== selectedProfile.value) return
    if (!result.success) {
      statusMessage.value = `${knowledgeText('title')} : ${knowledgeText(result.errorCode ?? 'failed')}`
      return
    }
    knowledgeForm.value = { ...knowledgeForm.value, [key]: emptyKnowledgeDraft() }
    await loadKnowledgeNotes(entryKind, name)
  } catch (error) {
    statusMessage.value = `${knowledgeText('title')} : ${String(error)}`
  } finally {
    knowledgeBusy.value = { ...knowledgeBusy.value, [key]: false }
  }
}

// Garantit un brouillon par entrée avant que le template ne s'y lie (v-model
// a besoin d'un objet déjà présent, pas d'une création paresseuse pendant le rendu).
watch(durabilityReport, (report) => {
  if (!report?.entries) return
  const updated = { ...knowledgeForm.value }
  let changed = false
  for (const entry of report.entries) {
    const key = knowledgeEntryKey(entry.entryKind, entry.name)
    if (!updated[key]) { updated[key] = emptyKnowledgeDraft(); changed = true }
  }
  if (changed) knowledgeForm.value = updated
})

interface ProfileEntry {
  name: string
  gameName?: string
  executableName?: string
  targetCount?: number
  patchCount?: number
}

interface ProfileTargetEntry {
  name: string
  type: string
  locator: string
  description: string
  locatorKind?: string
  dependsOn?: string[]
  ghidraSymbol?: string
  ghidraNote?: string
  clrTypeSubstring?: string
  clrIdentityField?: string
  clrIdentityValue?: string
  clrFieldName?: string
}

interface ProfilePatchEntry {
  name: string
  module?: string
  moduleOffset?: string
  aobPattern?: string
  patchBytes?: string
  originalBytes?: string
  disassembly?: string
  riskLevel?: string
  description?: string
  ghidraSymbol?: string
  ghidraNote?: string
  signatureScore?: number
  signatureLevel?: string
  signatureWarning?: string
  signatureFixedBytes?: number
  signatureWildcardBytes?: number
  signatureUniqueFixedBytes?: number
  signatureFixedRatio?: number
  trainerSafe?: boolean
  signatureMatches?: number
}

interface ProfileTargetGroup {
  name: string
  targets: ProfileTargetEntry[]
}

const profiles = ref<ProfileEntry[]>([])
const selectedProfile = ref<string>('')
const profileTargets = ref<ProfileTargetEntry[]>([])
const profilePatches = ref<ProfilePatchEntry[]>([])
const profileInfo = ref<Record<string, unknown>>({})
const newProfileName = ref('')
const newTargetName = ref('')
const newTargetDescription = ref('')
const resolveResult = ref<Record<string, unknown> | null>(null)
const statusMessage = ref('')
const targetWriteValues = ref<Record<string, string>>({})
const targetResolveStates = ref<Record<string, Record<string, unknown>>>({})
const patchStates = ref<Record<string, Record<string, unknown>>>({})
const trainerBusy = ref(false)
const clrProfileIdentityField = ref('')
const clrProfileIdentityValue = ref('')
const clrProfileTargetField = ref('')
const clrProfileValueType = ref('Int32')
const lastProfileStorageKey = 'killengine.lastProfile'

function resetDurability() {
  ++durabilityRequest
  durabilityReport.value = null
  durabilityEditor.value = null
  durabilityError.value = ''
  durabilityBusy.value = false
  knowledgeNotesByEntry.value = {}
  knowledgeForm.value = {}
  knowledgeBusy.value = {}
}

watch(selectedProfile, resetDurability)
watch([() => store.isAttached, () => store.processModules], () => {
  resetDurability()
  if (selectedProfile.value && store.isAttached) void inspectDurability()
})

const profileSaveTargets = computed(() => {
  if (store.finalCandidateTargets.length > 0) {
    return store.finalCandidateTargets
      .filter((target) => target.address)
      .map((target) => ({
        address: String(target.address ?? ''),
        type: String(target.type ?? store.exactScanType),
      }))
  }

  if (store.selectedCandidateAddress) {
    return [{ address: store.selectedCandidateAddress, type: store.exactScanType }]
  }

  return []
})

const groupedProfileTargets = computed<ProfileTargetGroup[]>(() => {
  const groups = new Map<string, ProfileTargetEntry[]>()
  for (const target of profileTargets.value) {
    const groupName = profileTargetGroupName(target.name)
    groups.set(groupName, [...(groups.get(groupName) ?? []), target])
  }
  return Array.from(groups.entries()).map(([name, targets]) => ({ name, targets }))
})

const clrObjectFields = computed<ClrFieldInfo[]>(() => {
  const details = store.clrSelectedObject?.fieldDetails
  return Array.isArray(details) ? details : []
})

const clrIdentityFields = computed(() => clrObjectFields.value.filter((field) => canUseAsClrIdentity(field)))
const clrWritableFields = computed(() => clrObjectFields.value.filter((field) => field.writable))
const canSaveClrProfileTarget = computed(() => Boolean(
  selectedProfile.value
  && newTargetName.value.trim()
  && store.clrSelectedObject
  && clrProfileIdentityField.value.trim()
  && clrProfileIdentityValue.value.trim()
  && clrProfileTargetField.value.trim()
  && clrProfileValueType.value.trim(),
))

const trainerPatchSummary = computed(() => {
  const states = profilePatches.value.map((patch) => patchStates.value[patch.name]).filter(Boolean)
  const count = (status: string) => states.filter((state) => String(state.status ?? '') === status).length
  const inspected = profilePatches.value.length > 0 && states.length === profilePatches.value.length
  const unsafeQuality = profilePatches.value.filter((patch) => patchQualityBlocksTrainer(patch)).length
  const risky = count('ambiguous') + count('missing') + count('invalid')
  return {
    inspected,
    original: count('original'),
    active: count('active'),
    ambiguous: count('ambiguous'),
    missing: count('missing'),
    invalid: count('invalid'),
    unsafeQuality,
    risky,
    canApplyAll: inspected && risky === 0 && unsafeQuality === 0,
  }
})

function profileTargetGroupName(name: string): string {
  const normalized = name.trim().toLowerCase().replace(/\s+\d+$/, '').trim()
  return normalized || t('profile.targetsTitleFallback')
}

function canUseAsClrIdentity(field: ClrFieldInfo): boolean {
  return field.kind === 'primitive'
    || field.kind === 'string'
    || typeof field.value === 'string'
    || typeof field.value === 'number'
    || typeof field.value === 'boolean'
}

function clrValueTypeForField(field: ClrFieldInfo): string {
  const text = String(field.elementType ?? field.typeName ?? '').toLowerCase()
  if (text.includes('int16')) return 'Int16'
  if (text.includes('int64')) return 'Int64'
  if (text.includes('single') || text.includes('float')) return 'Float32'
  if (text.includes('double')) return 'Float64'
  return 'Int32'
}

function refreshClrProfileDefaults() {
  const identity = clrIdentityFields.value.find((field) => field.name === 'Name') ?? clrIdentityFields.value[0]
  const writable = clrWritableFields.value.find((field) => field.name === 'Health') ?? clrWritableFields.value[0]
  if (identity && !clrProfileIdentityField.value) {
    clrProfileIdentityField.value = identity.name
    clrProfileIdentityValue.value = String(identity.value ?? '')
  }
  if (writable && !clrProfileTargetField.value) {
    clrProfileTargetField.value = writable.name
    clrProfileValueType.value = clrValueTypeForField(writable)
  }
}

function selectClrIdentityField(fieldName: string) {
  const field = clrObjectFields.value.find((item) => item.name === fieldName)
  if (!field) return
  clrProfileIdentityField.value = field.name
  clrProfileIdentityValue.value = String(field.value ?? '')
}

function selectClrTargetField(fieldName: string) {
  const field = clrObjectFields.value.find((item) => item.name === fieldName)
  if (!field) return
  clrProfileTargetField.value = field.name
  clrProfileValueType.value = clrValueTypeForField(field)
}

async function refreshProfiles() {
  try {
    profiles.value = (await backend.getController().listProfiles()) as unknown as ProfileEntry[]
    const preferred = selectedProfile.value || localStorage.getItem(lastProfileStorageKey) || ''
    const match = profiles.value.find((profile) => profile.name === preferred)
    if (!selectedProfile.value && match) {
      await selectProfile(match.name)
    }
  } catch (e) {
    // UX-CHECKUP-7 (22/09/2026) : listProfiles() ne renvoie jamais success:false
    // (seul un échec de transport RPC atterrit ici) -- rare, mais ne doit plus
    // disparaître sans trace pour autant.
    profiles.value = []
    statusMessage.value = '✗ ' + t('profile.errorPrefix', { error: String(e) })
  }
}

const importingLegacyProfiles = ref(false)

// PORT-2c (docs/PORTABILITY_ROADMAP.md, 17/09/2026) : reprise explicite et non
// destructive des profils encore sous l'ancien emplacement système.
async function importLegacyProfiles() {
  importingLegacyProfiles.value = true
  statusMessage.value = t('profile.importLegacyRunning')
  try {
    const result = await backend.getController().importLegacyProfiles()
    const imported = Number(result.importedCount ?? 0)
    const skipped = Number(result.skippedCount ?? 0)
    const invalid = Number(result.invalidCount ?? 0)
    if (imported + skipped + invalid === 0) {
      statusMessage.value = t('profile.importLegacyNone')
    } else {
      statusMessage.value = (imported > 0 ? '✓ ' : '') + t('profile.importLegacySummary', { imported, skipped, invalid })
    }
    if (imported > 0) {
      await refreshProfiles()
    }
  } catch (e) {
    statusMessage.value = '✗ ' + t('profile.errorPrefix', { error: String(e) })
  } finally {
    importingLegacyProfiles.value = false
  }
}

async function selectProfile(name: string) {
  resetDurability()
  selectedProfile.value = name
  localStorage.setItem(lastProfileStorageKey, name)
  resolveResult.value = null
  // UX-CHECKUP-7 : une erreur affichée par une sélection précédente (ex.
  // profil corrompu) ne doit pas rester affichée après avoir sélectionné un
  // profil différent qui se charge normalement.
  statusMessage.value = ''
  try {
    const result = await backend.getController().loadProfile(name)
    if (name !== selectedProfile.value) return
    // UX-CHECKUP-7 (22/09/2026) : loadProfile() ne lève jamais d'exception sur
    // un profil introuvable/corrompu -- il résout normalement avec
    // {success:false, error}. Ce code utilisait `result` sans jamais lire
    // `success`, donc un échec affichait un profil vide (0 cible/0 patch)
    // indiscernable d'un profil réellement vide créé par l'utilisateur. Le
    // profil reste sélectionné (le nom affiché) pour que l'utilisateur voie
    // clairement LEQUEL a échoué, plutôt que d'annuler la sélection.
    if (result.success !== true) {
      profileInfo.value = {}
      profileTargets.value = []
      profilePatches.value = []
      patchStates.value = {}
      statusMessage.value = '✗ ' + String(result.error ?? t('profile.loadFailed'))
      return
    }
    profileInfo.value = result
    profileTargets.value = (result.targets as ProfileTargetEntry[]) ?? []
    profilePatches.value = (result.patches as ProfilePatchEntry[]) ?? []
    patchStates.value = {}
    if (store.isAttached && profilePatches.value.length > 0) {
      await inspectProfilePatches()
    }
    if (store.isAttached && name === selectedProfile.value) await inspectDurability()
  } catch (e) {
    profileTargets.value = []
    profilePatches.value = []
    patchStates.value = {}
    statusMessage.value = '✗ ' + t('profile.errorPrefix', { error: String(e) })
  }
}

async function saveCurrentTarget() {
  if (!selectedProfile.value) {
    statusMessage.value = '⚠ ' + t('profile.selectOrCreateProfileFirst')
    return
  }
  if (profileSaveTargets.value.length === 0) {
    statusMessage.value = '⚠ ' + t('profile.selectAddressOrFinishSearch')
    return
  }
  if (!newTargetName.value.trim()) {
    statusMessage.value = '⚠ ' + t('profile.giveTargetName')
    return
  }

  try {
    const baseName = newTargetName.value.trim()
    const targets = profileSaveTargets.value
    const results = []

    for (let i = 0; i < targets.length; i += 1) {
      const target = targets[i]
      const targetName = targets.length === 1 ? baseName : `${baseName} ${i + 1}`
      const result = await backend.getController().saveProfileTarget(
        selectedProfile.value,
        targetName,
        target.address,
        target.type,
        newTargetDescription.value.trim(),
      )
      results.push(result)
      if (!result.success) {
        statusMessage.value = '✗ ' + (result.error ?? t('profile.saveErrorFor', { name: targetName }))
        return
      }
    }

    if (results.every((result) => result.success)) {
      statusMessage.value = targets.length === 1
        ? '✓ ' + t('profile.targetSaved', { name: baseName, profile: selectedProfile.value, locator: results[0].locator })
        : '✓ ' + t('profile.addressesSaved', { count: targets.length, profile: selectedProfile.value, name: baseName })
      newTargetName.value = ''
      newTargetDescription.value = ''
      await selectProfile(selectedProfile.value)
      await refreshProfiles()
    }
  } catch (e) {
    statusMessage.value = '✗ ' + t('profile.errorPrefix', { error: String(e) })
  }
}

async function saveCurrentClrFieldTarget() {
  if (!selectedProfile.value) {
    statusMessage.value = '⚠ ' + t('profile.selectOrCreateProfileFirst')
    return
  }
  if (!store.clrSelectedObject) {
    statusMessage.value = '⚠ ' + t('profile.readClrObjectFirst')
    return
  }
  if (!newTargetName.value.trim()) {
    statusMessage.value = '⚠ ' + t('profile.giveTargetName')
    return
  }

  const controller = backend.getController()
  if (!controller.saveClrFieldProfileTarget) {
    statusMessage.value = '✗ ' + t('profile.clrSaveUnavailable')
    return
  }

  try {
    const result = await controller.saveClrFieldProfileTarget(
      selectedProfile.value,
      newTargetName.value.trim(),
      store.clrSelectedObject.typeName,
      clrProfileIdentityField.value.trim(),
      clrProfileIdentityValue.value.trim(),
      clrProfileTargetField.value.trim(),
      clrProfileValueType.value.trim(),
      newTargetDescription.value.trim(),
    )
    if (result.success) {
      statusMessage.value = '✓ ' + t('profile.clrTargetSaved', { name: newTargetName.value.trim(), locator: result.locator })
      newTargetName.value = ''
      newTargetDescription.value = ''
      await selectProfile(selectedProfile.value)
      await refreshProfiles()
    } else {
      statusMessage.value = '✗ ' + (result.error ?? t('profile.clrSaveImpossible'))
    }
  } catch (e) {
    statusMessage.value = '✗ ' + t('profile.errorPrefix', { error: String(e) })
  }
}

async function createNewProfile() {
  const name = newProfileName.value.trim()
  if (!name) return
  selectedProfile.value = name
  profileTargets.value = []
  profileInfo.value = { gameName: name, executableName: store.processName }
  statusMessage.value = t('profile.profileReadyCreateFile', { name })
  newProfileName.value = ''
}

async function activateTarget(targetName: string) {
  if (!selectedProfile.value) return
  if (!await checkDurabilityBeforeUse('target', [targetName])) return
  try {
    const result = await backend.getController().activateProfileTarget(selectedProfile.value, targetName)
    resolveResult.value = result
    if (result.success) {
      statusMessage.value = '✓ ' + t('profile.targetActivated', { name: targetName, address: result.address })
      store.addActionLog('profile', t('profile.log.targetActivated', { name: targetName }), t('profile.log.targetActivatedDetail', { profile: selectedProfile.value, address: result.address }), 'success')
    } else {
      const detail = result.error ? String(result.error) : t('profile.activationImpossible')
      statusMessage.value = '✗ ' + detail
      store.addActionLog('profile', t('profile.log.activationRefused', { name: targetName }), detail, 'warning')
    }
  } catch (e) {
    statusMessage.value = '✗ ' + t('profile.errorPrefix', { error: String(e) })
    store.addActionLog('profile', t('profile.log.activationFailed', { name: targetName }), String(e), 'error')
  }
}

async function verifyTarget(target: ProfileTargetEntry) {
  if (!selectedProfile.value) return
  try {
    const result = await backend.getController().resolveProfileTarget(selectedProfile.value, target.name)
    targetResolveStates.value = { ...targetResolveStates.value, [target.name]: result }
    resolveResult.value = result
    statusMessage.value = result.success
      ? '✓ ' + t('profile.targetVerified', { name: target.name, address: result.address })
      : '✗ ' + t('profile.targetNotFound', { name: target.name, error: result.error ?? t('profile.resolutionImpossible') })
  } catch (e) {
    const result = { success: false, error: String(e) }
    targetResolveStates.value = { ...targetResolveStates.value, [target.name]: result }
    statusMessage.value = '✗ ' + t('profile.errorPrefix', { error: String(e) })
  }
}

function targetResolutionLabel(target: ProfileTargetEntry): string {
  const state = targetResolveStates.value[target.name]
  if (!state) return t('profile.toVerify')
  return state.success ? t('profile.resolvedAt', { address: state.address }) : t('profile.notFoundStatus')
}

function targetResolutionClass(target: ProfileTargetEntry): string {
  const state = targetResolveStates.value[target.name]
  if (!state) return 'pending'
  return state.success ? 'ok' : 'fail'
}

// Roadmap section L — Pointer maps : diagnostic groupé de toutes les cibles du
// profil d'un coup, utile après un redémarrage du jeu (nouvelle base ASLR)
// pour éviter de revalider chaque chaîne une par une.
const pointerMapCompareResult = ref<Record<string, unknown> | null>(null)
const pointerMapCompareBusy = ref(false)
const pointerMapExportText = ref('')
const pointerMapImportText = ref('')
const pointerMapTransferResult = ref<Record<string, unknown> | null>(null)
const pointerMapTransferBusy = ref(false)
const pointerMapReplaceExisting = ref(false)
const ghidraExportJson = ref('')
const ghidraExportScript = ref('')
const ghidraImportText = ref('')
const ghidraBridgeResult = ref<Record<string, unknown> | null>(null)
const ghidraBridgeBusy = ref(false)
const pointerMapResults = computed(
  () => (pointerMapCompareResult.value?.results as Array<Record<string, unknown>>) ?? [],
)

async function comparePointerMap() {
  if (!selectedProfile.value) return
  pointerMapCompareBusy.value = true
  try {
    const controller = backend.getController()
    if (!controller.comparePointerMapAcrossRestart) {
      pointerMapCompareResult.value = { success: false, error: t('profile.groupCompareUnavailable') }
      return
    }
    pointerMapCompareResult.value = await controller.comparePointerMapAcrossRestart(selectedProfile.value)
  } catch (e) {
    pointerMapCompareResult.value = { success: false, error: String(e) }
  } finally {
    pointerMapCompareBusy.value = false
  }
}

async function exportGhidraBridge() {
  if (!selectedProfile.value) return
  ghidraBridgeBusy.value = true
  try {
    const controller = backend.getController()
    if (!controller.exportGhidraArtifacts) {
      ghidraBridgeResult.value = { success: false, error: t('profile.ghidraExportUnavailable') }
      return
    }
    const result = await controller.exportGhidraArtifacts(selectedProfile.value)
    ghidraBridgeResult.value = result
    ghidraExportJson.value = result.success ? String(result.json ?? '') : ''
    ghidraExportScript.value = result.success ? String(result.pythonScript ?? '') : ''
  } catch (e) {
    ghidraBridgeResult.value = { success: false, error: String(e) }
  } finally {
    ghidraBridgeBusy.value = false
  }
}

async function importGhidraSymbols() {
  if (!selectedProfile.value || !ghidraImportText.value.trim()) return
  ghidraBridgeBusy.value = true
  try {
    const controller = backend.getController()
    if (!controller.importGhidraSymbols) {
      ghidraBridgeResult.value = { success: false, error: t('profile.ghidraImportUnavailable') }
      return
    }
    const result = await controller.importGhidraSymbols(selectedProfile.value, ghidraImportText.value)
    ghidraBridgeResult.value = result
    if (result.success) {
      await selectProfile(selectedProfile.value)
    }
  } catch (e) {
    ghidraBridgeResult.value = { success: false, error: String(e) }
  } finally {
    ghidraBridgeBusy.value = false
  }
}

async function exportPointerMap() {
  if (!selectedProfile.value) return
  pointerMapTransferBusy.value = true
  try {
    const controller = backend.getController()
    if (!controller.exportPointerMap) {
      pointerMapTransferResult.value = { success: false, error: t('profile.pointerMapExportUnavailable') }
      return
    }
    const result = await controller.exportPointerMap(selectedProfile.value)
    pointerMapTransferResult.value = result
    pointerMapExportText.value = result.success ? String(result.json ?? '') : ''
  } catch (e) {
    pointerMapTransferResult.value = { success: false, error: String(e) }
  } finally {
    pointerMapTransferBusy.value = false
  }
}

async function importPointerMap() {
  if (!selectedProfile.value || !pointerMapImportText.value.trim()) return
  pointerMapTransferBusy.value = true
  try {
    const controller = backend.getController()
    if (!controller.importPointerMap) {
      pointerMapTransferResult.value = { success: false, error: t('profile.pointerMapImportUnavailable') }
      return
    }
    const result = await controller.importPointerMap(selectedProfile.value, pointerMapImportText.value, {
      replaceExisting: pointerMapReplaceExisting.value,
    })
    pointerMapTransferResult.value = result
    if (result.success) {
      await selectProfile(selectedProfile.value)
    }
  } catch (e) {
    pointerMapTransferResult.value = { success: false, error: String(e) }
  } finally {
    pointerMapTransferBusy.value = false
  }
}

async function repairTargetWithCurrentAddress(target: ProfileTargetEntry) {
  if (!selectedProfile.value || !store.selectedCandidateAddress) {
    statusMessage.value = '⚠ ' + t('profile.selectAddressBeforeRepair')
    return
  }
  try {
    const result = await backend.getController().saveProfileTarget(
      selectedProfile.value,
      target.name,
      store.selectedCandidateAddress,
      target.type,
      target.description || t('profile.repairedFromCurrentAddress'),
    )
    if (result.success) {
      statusMessage.value = '✓ ' + t('profile.targetRepaired', { name: target.name, address: store.selectedCandidateAddress })
      await selectProfile(selectedProfile.value)
      await verifyTarget(target)
    } else {
      statusMessage.value = '✗ ' + (result.error ?? t('profile.repairImpossible'))
    }
  } catch (e) {
    statusMessage.value = '✗ ' + t('profile.errorPrefix', { error: String(e) })
  }
}

async function activateAllTargets() {
  if (!selectedProfile.value || profileTargets.value.length === 0) return
  if (!await checkDurabilityBeforeUse('target', profileTargets.value.map((target) => target.name))) return

  let activated = 0
  const activatableTargets = profileTargets.value.filter((target) => target.locatorKind !== 'clr_field')
  for (const target of activatableTargets) {
    try {
      const result = await backend.getController().activateProfileTarget(selectedProfile.value, target.name)
      if (!result.success) {
        statusMessage.value = '✗ ' + (result.error ?? t('profile.activationImpossibleFor', { name: target.name }))
        return
      }
      activated += 1
      resolveResult.value = result
    } catch (e) {
      statusMessage.value = '✗ ' + t('profile.errorPrefix', { error: String(e) })
      return
    }
  }

  const skipped = profileTargets.value.length - activatableTargets.length
  statusMessage.value = '✓ ' + (skipped > 0
    ? t('profile.targetsActivatedForAssistant', { count: activated, skipped })
    : t('profile.targetsActivatedForAssistantSimple', { count: activated }))
}

async function activateTargetGroup(group: ProfileTargetGroup) {
  if (!selectedProfile.value || group.targets.length === 0) return
  if (!await checkDurabilityBeforeUse('target', group.targets.map((target) => target.name))) return

  let activated = 0
  const activatableTargets = group.targets.filter((target) => target.locatorKind !== 'clr_field')
  for (const target of activatableTargets) {
    try {
      const result = await backend.getController().activateProfileTarget(selectedProfile.value, target.name)
      if (!result.success) {
        statusMessage.value = '✗ ' + (result.error ?? t('profile.activationImpossibleFor', { name: target.name }))
        return
      }
      activated += 1
      resolveResult.value = result
    } catch (e) {
      statusMessage.value = '✗ ' + t('profile.errorPrefix', { error: String(e) })
      return
    }
  }

  await store.refreshSmartSearchContext()
  const skipped = group.targets.length - activatableTargets.length
  statusMessage.value = '✓ ' + (skipped > 0
    ? t('profile.groupReadyInAssistant', { name: group.name, count: activated, skipped })
    : t('profile.groupReadyInAssistantSimple', { name: group.name, count: activated }))
}

async function writeProfileTarget(target: ProfileTargetEntry) {
  const value = (targetWriteValues.value[target.name] ?? '').trim()
  if (!selectedProfile.value || !value) return
  if (!await checkDurabilityBeforeUse('target', [target.name])) return

  try {
    const resolved = await backend.getController().resolveProfileTarget(selectedProfile.value, target.name)
    resolveResult.value = resolved
    if (!resolved.success) {
      statusMessage.value = '✗ ' + (resolved.error ?? t('profile.addressNotFoundFor', { name: target.name }))
      return
    }

    const write = resolved.locatorKind === 'clr_field'
      ? await store.writeClrPrimitiveField(String(resolved.address ?? ''), String(resolved.clrFieldName ?? target.clrFieldName ?? ''), value)
      : await store.writeMemoryValueByMode(String(resolved.address ?? ''), target.type, value)
    if (write?.success) {
      statusMessage.value = '✓ ' + t('profile.targetWritten', { name: target.name, value, address: resolved.address })
      await store.refreshSmartSearchContext()
    } else {
      statusMessage.value = '✗ ' + (write?.error || t('profile.writeImpossibleFor', { name: target.name }))
    }
  } catch (e) {
    statusMessage.value = '✗ ' + t('profile.errorPrefix', { error: String(e) })
  }
}

watch(() => store.clrSelectedObject?.address, () => {
  clrProfileIdentityField.value = ''
  clrProfileIdentityValue.value = ''
  clrProfileTargetField.value = ''
  clrProfileValueType.value = 'Int32'
  refreshClrProfileDefaults()
})

async function deleteSelectedProfile() {
  if (!selectedProfile.value) return
  try {
    const ok = await backend.getController().deleteProfile(selectedProfile.value)
    if (ok) {
      statusMessage.value = t('profile.profileDeleted', { name: selectedProfile.value })
      selectedProfile.value = ''
      localStorage.removeItem(lastProfileStorageKey)
      profileTargets.value = []
      profilePatches.value = []
      await refreshProfiles()
    } else {
      statusMessage.value = '✗ ' + t('profile.deleteFailed')
    }
  } catch (e) {
    statusMessage.value = '✗ ' + t('profile.errorPrefix', { error: String(e) })
  }
}

async function applyProfilePatch(patch: ProfilePatchEntry) {
  if (!selectedProfile.value || !patch.name) return
  if (!await checkDurabilityBeforeUse('patch', [patch.name])) return
  if (!patchCanApply(patch)) {
    statusMessage.value = '⚠ ' + (patchStates.value[patch.name]
      ? t('profile.patchNotApplicableState', { name: patch.name, state: patchStateLabel(patch) })
      : (patchQualityBlocksTrainer(patch)
        ? t('profile.patchBlocked', { name: patch.name, reason: patchQualityBlockReason(patch) })
        : t('profile.checkStateBeforeApply', { name: patch.name })))
    return
  }
  trainerBusy.value = true
  try {
    const controller = backend.getController()
    if (!controller.applyProfileCodePatch) {
      statusMessage.value = '✗ ' + t('profile.patchFunctionUnavailable')
      return
    }
    const result = await controller.applyProfileCodePatch(selectedProfile.value, patch.name)
    patchStates.value = { ...patchStates.value, [patch.name]: { ...result, active: Boolean(result.success || result.active) } }
    statusMessage.value = result.success
      ? '✓ ' + t('profile.patchApplied', { name: patch.name, address: result.matchedAddress ?? result.address ?? '' })
        + (result.executableVersionMismatch ? ' ⚠' + t('profile.versionMismatchWarning', { warning: String(result.executableVersionWarning ?? '') }) : '')
      : '✗ ' + (result.error ?? t('profile.patchImpossible', { name: patch.name }))
  } catch (e) {
    statusMessage.value = '✗ ' + t('profile.errorPrefix', { error: String(e) })
  } finally {
    trainerBusy.value = false
  }
}

async function restoreProfilePatch(patch: ProfilePatchEntry) {
  if (!selectedProfile.value || !patch.name) return
  if (!patchCanRestore(patch)) {
    statusMessage.value = '⚠ ' + (patchStates.value[patch.name]
      ? t('profile.patchNotActive', { name: patch.name })
      : t('profile.checkStateBeforeRestore', { name: patch.name }))
    return
  }
  trainerBusy.value = true
  try {
    const controller = backend.getController()
    if (!controller.restoreProfileCodePatch) {
      statusMessage.value = '✗ ' + t('profile.restoreFunctionUnavailable')
      return
    }
    const result = await controller.restoreProfileCodePatch(selectedProfile.value, patch.name)
    patchStates.value = { ...patchStates.value, [patch.name]: { ...result, active: result.success ? false : Boolean(result.active) } }
    statusMessage.value = result.success
      ? '✓ ' + t('profile.patchRestored', { name: patch.name })
      : '✗ ' + (result.error ?? t('profile.restoreImpossibleFor', { name: patch.name }))
  } catch (e) {
    statusMessage.value = '✗ ' + t('profile.errorPrefix', { error: String(e) })
  } finally {
    trainerBusy.value = false
  }
}

function patchStateLabel(patch: ProfilePatchEntry): string {
  const state = patchStates.value[patch.name]
  if (!state) return t('profile.ready')
  const status = String(state.status ?? '')
  if (status === 'original') return t('profile.original')
  if (status === 'active') return t('profile.active')
  if (status === 'ambiguous') return t('profile.ambiguous')
  if (status === 'missing') return t('profile.notFound')
  if (status === 'invalid') return t('profile.invalid')
  if (state.active === true) return t('profile.active')
  if (state.active === false && state.success) return t('profile.restored')
  return state.success ? 'ok' : 'fail'
}

function patchStateClass(patch: ProfilePatchEntry): string {
  const state = patchStates.value[patch.name]
  if (!state) return 'pending'
  const status = String(state.status ?? '')
  if (status === 'active') return 'ok'
  if (status === 'original') return 'pending'
  if (status === 'ambiguous') return 'warn'
  if (status === 'missing' || status === 'invalid') return 'fail'
  if (state.active === true) return 'ok'
  if (state.active === false && state.success) return 'pending'
  return state.success ? 'ok' : 'fail'
}

function patchStateDetail(patch: ProfilePatchEntry): string {
  const state = patchStates.value[patch.name]
  if (!state) return t('profile.checkRequiredBeforeApply')
  const parts = [
    state.matchedAddress ? `0x${state.matchedAddress}` : '',
    state.originalMatches !== undefined ? t('profile.originalMatchesLabel', { count: state.originalMatches }) : '',
    state.patchedMatches !== undefined ? t('profile.patchedMatchesLabel', { count: state.patchedMatches }) : '',
    state.error ? String(state.error) : '',
  ].filter(Boolean)
  return parts.join(' · ')
}

function patchQualityBlocksTrainer(patch: ProfilePatchEntry): boolean {
  const score = Number(patch.signatureScore ?? 100)
  const fixedBytes = Number(patch.signatureFixedBytes ?? 99)
  return fixedBytes < 3 || score < 35
}

function patchQualityBlockReason(patch: ProfilePatchEntry): string {
  if (!patchQualityBlocksTrainer(patch)) return ''
  return t('profile.insufficientAobQuality', { score: patch.signatureScore ?? 0, fixed: patch.signatureFixedBytes ?? 0 })
}

function patchCanApply(patch: ProfilePatchEntry): boolean {
  if (patchQualityBlocksTrainer(patch)) return false
  const state = patchStates.value[patch.name]
  if (!state) return false
  const status = String(state.status ?? '')
  return status === 'original' || (state.success === true && state.active !== true && status === '')
}

function patchCanRestore(patch: ProfilePatchEntry): boolean {
  const state = patchStates.value[patch.name]
  if (!state) return false
  return String(state.status ?? '') === 'active' || state.active === true
}

async function toggleProfilePatch(patch: ProfilePatchEntry, event: Event) {
  const checked = (event.target as HTMLInputElement).checked
  if (checked) {
    await applyProfilePatch(patch)
  } else {
    await restoreProfilePatch(patch)
  }
}

async function inspectProfilePatches() {
  if (!selectedProfile.value || profilePatches.value.length === 0) return
  trainerBusy.value = true
  try {
    const controller = backend.getController()
    if (!controller.inspectProfileCodePatches) {
      statusMessage.value = '✗ ' + t('profile.inspectionUnavailable')
      return
    }
    const result = await controller.inspectProfileCodePatches(selectedProfile.value)
    const nextStates = { ...patchStates.value }
    for (const item of ((result.states as Record<string, unknown>[]) ?? [])) {
      const patchName = String(item.patchName ?? '')
      if (patchName) nextStates[patchName] = { ...item, active: Boolean(item.active) }
    }
    patchStates.value = nextStates
    statusMessage.value = result.success
      ? '✓ ' + t('profile.trainerState', { active: result.active, original: result.original, ambiguous: result.ambiguous })
      : '✗ ' + (String(result.error ?? t('profile.inspectionIncomplete')))
  } catch (e) {
    statusMessage.value = '✗ ' + t('profile.errorPrefix', { error: String(e) })
  } finally {
    trainerBusy.value = false
  }
}

async function applyAllProfilePatches() {
  if (!selectedProfile.value || profilePatches.value.length === 0) return
  if (!await checkDurabilityBeforeUse('patch', profilePatches.value.map((patch) => patch.name))) return
  if (!trainerPatchSummary.value.canApplyAll) {
    statusMessage.value = '⚠ ' + (trainerPatchSummary.value.inspected
      ? (trainerPatchSummary.value.unsafeQuality > 0
        ? t('profile.globalApplyBlockedQuality', { count: trainerPatchSummary.value.unsafeQuality })
        : t('profile.globalApplyBlockedState'))
      : t('profile.checkTrainerStateFirst'))
    return
  }
  trainerBusy.value = true
  try {
    const controller = backend.getController()
    if (!controller.applyAllProfileCodePatches) {
      statusMessage.value = '✗ ' + t('profile.trainerFunctionUnavailable')
      return
    }
    const result = await controller.applyAllProfileCodePatches(selectedProfile.value)
    const nextStates = { ...patchStates.value }
    for (const item of ((result.results as Record<string, unknown>[]) ?? [])) {
      const patchName = String(item.patchName ?? '')
      if (patchName) nextStates[patchName] = { ...item, active: Boolean(item.success || item.active || item.alreadyActive) }
    }
    patchStates.value = nextStates
    statusMessage.value = result.success
      ? '✓ ' + t('profile.patchesApplied', { applied: result.applied, total: result.total, alreadyActive: result.alreadyActive ?? 0 })
      : '✗ ' + (String(result.error ?? t('profile.trainerApplyPartial')))
  } catch (e) {
    statusMessage.value = '✗ ' + t('profile.errorPrefix', { error: String(e) })
  } finally {
    trainerBusy.value = false
  }
}

async function restoreAllProfilePatches() {
  if (!selectedProfile.value || profilePatches.value.length === 0) return
  trainerBusy.value = true
  try {
    const controller = backend.getController()
    if (!controller.restoreAllProfileCodePatches) {
      statusMessage.value = '✗ ' + t('profile.restoreTrainerFunctionUnavailable')
      return
    }
    const result = await controller.restoreAllProfileCodePatches(selectedProfile.value)
    const nextStates = { ...patchStates.value }
    for (const item of ((result.results as Record<string, unknown>[]) ?? [])) {
      const patchName = String(item.patchName ?? '')
      if (patchName) nextStates[patchName] = { ...item, active: item.success ? false : Boolean(item.active ?? nextStates[patchName]?.active) }
    }
    patchStates.value = nextStates
    statusMessage.value = result.success
      ? '✓ ' + t('profile.patchesRestored', { restored: result.restored, alreadyInactive: result.alreadyInactive })
      : '✗ ' + (String(result.error ?? t('profile.trainerRestorePartial')))
  } catch (e) {
    statusMessage.value = '✗ ' + t('profile.errorPrefix', { error: String(e) })
  } finally {
    trainerBusy.value = false
  }
}

onMounted(() => {
  refreshProfiles()
})
</script>

<template>
  <div class="profile-view">
    <div class="header">
      <h1>{{ $t('profile.title') }}</h1>
      <button class="btn btn-secondary" @click="refreshProfiles">↻ {{ $t('profile.refresh') }}</button>
      <button
        class="btn btn-secondary"
        :disabled="importingLegacyProfiles"
        @click="importLegacyProfiles"
      >
        {{ $t('profile.importLegacyProfiles') }}
      </button>
    </div>

    <PanelIntro
      :what="$t('profile.intro.what')"
      :purpose="$t('profile.intro.purpose')"
      :how="$t('profile.intro.how')"
    />

    <div v-if="!store.isAttached" class="warn-box">
      <p>⚠ {{ $t('profile.attachPrompt') }}</p>
      <button class="btn btn-secondary" @click="store.activeView = 'process'">{{ $t('profile.goToProcess') }}</button>
    </div>

    <!-- Liste des profils -->
    <div class="section">
      <h2>{{ $t('profile.registeredProfilesTitle') }}</h2>
      <div v-if="profiles.length === 0" class="empty">{{ $t('profile.noProfileRegistered') }}</div>
      <div v-else class="profile-list">
        <div
          v-for="p in profiles"
          :key="p.name"
          class="profile-card"
          :class="{ active: selectedProfile === p.name }"
          @click="selectProfile(p.name)"
        >
          <div class="profile-name">{{ p.gameName ?? p.name }}</div>
          <div class="profile-meta">
            <span v-if="p.executableName">{{ p.executableName }}</span>
            <span v-if="p.targetCount !== undefined">{{ $t('profile.targetCount', { count: p.targetCount }) }}</span>
            <span v-if="p.patchCount !== undefined">{{ $t('profile.patchCount', { count: p.patchCount }) }}</span>
          </div>
        </div>
      </div>
    </div>

    <!-- Créer un nouveau profil -->
    <div class="section">
      <h2>{{ $t('profile.createProfileTitle') }}</h2>
      <div class="create-row">
        <input
          v-model="newProfileName"
          :placeholder="$t('profile.profileNamePlaceholder')"
          class="scan-input"
          @keyup.enter="createNewProfile()"
        />
        <button class="btn btn-secondary" :disabled="!newProfileName.trim()" @click="createNewProfile()">
          {{ $t('profile.create') }}
        </button>
      </div>
    </div>

    <!-- Détails du profil sélectionné -->
    <div v-if="selectedProfile" class="section">
      <h2>{{ selectedProfile }}</h2>

      <div class="targets-list durability-panel">
        <h3>{{ durabilityText('title') }}</h3>
        <p class="hint">{{ durabilityText('scope') }}</p>
        <p class="hint">{{ durabilityText('proof') }}</p>
        <button class="btn btn-secondary btn-sm" :disabled="durabilityBusy || !store.isAttached" @click="inspectDurability()">
          {{ durabilityText('inspect') }}
        </button>
        <p v-if="durabilityError" role="status">{{ durabilityText('failed') }} : {{ durabilityText(durabilityError) }}</p>
        <p v-if="durabilityReport?.observedAt" class="hint">{{ durabilityReport.observedAt }}</p>
        <p v-if="durabilityReport?.readBudgetExceeded" role="status">{{ durabilityText('budget') }}</p>
        <details v-for="entry in durabilityReport?.entries ?? []" :key="`${entry.entryKind}:${entry.name}`" class="durability-entry">
          <summary><strong>{{ entry.name }}</strong> — {{ durabilityText(entry.status) }}</summary>
          <p>{{ durabilityText(entry.nextAction) }}</p>
          <p v-if="entry.requiredTest">{{ durabilityText('test') }} : {{ entry.requiredTest }}</p>
          <p v-if="entry.plan.recordedAt" class="hint">
            {{ durabilityText('baseline') }} : {{ entry.plan.recordedAt }}
            <code>{{ entry.plan.baselineObservation?.observedBytes }}</code>
          </p>
          <p v-if="entry.plan.evidenceNote" class="hint">{{ durabilityText('note') }} : {{ entry.plan.evidenceNote }}</p>
          <ul>
            <li v-for="candidate in entry.candidates ?? []" :key="candidate.index">
              <code>{{ candidate.locator }}</code>
              <span v-if="candidate.address"> → <code>0x{{ candidate.address }}</code></span>
              <span v-if="candidate.observedBytes"> · {{ durabilityText('observed') }} : <code>{{ candidate.observedBytes }}</code></span>
              <span v-if="candidate.errorCode"> · {{ durabilityText(candidate.errorCode) }}</span>
            </li>
          </ul>
          <button class="btn btn-secondary btn-sm" :disabled="durabilityBusy" @click="editDurability(entry)">{{ durabilityText('configure') }}</button>

          <div class="knowledge-panel">
            <div class="knowledge-head">
              <strong>{{ knowledgeText('title') }}</strong>
              <button
                type="button"
                class="btn btn-secondary btn-sm"
                :disabled="knowledgeBusy[knowledgeEntryKey(entry.entryKind, entry.name)]"
                @click="loadKnowledgeNotes(entry.entryKind, entry.name)"
              >
                {{ knowledgeText('load') }}
              </button>
            </div>
            <p class="hint">{{ knowledgeText('hint') }}</p>
            <template v-if="notesFor(entry)">
              <p v-if="!notesFor(entry)?.success" role="status">{{ knowledgeText(notesFor(entry)?.errorCode ?? 'failed') }}</p>
              <template v-else>
                <div v-for="note in notesFor(entry)?.valid ?? []" :key="note.id" class="knowledge-note valid">
                  <span class="knowledge-kind">{{ knowledgeText(`kind_${note.kind}`) }}</span>
                  <span class="knowledge-status">{{ knowledgeText('valid') }}</span>
                  <p>{{ note.description }}</p>
                  <p v-if="note.experiment" class="hint">{{ note.experiment }}</p>
                  <p v-if="note.evidenceNote" class="hint">{{ note.evidenceNote }}</p>
                </div>
                <div v-for="note in notesFor(entry)?.stale ?? []" :key="note.id" class="knowledge-note stale">
                  <span class="knowledge-kind">{{ knowledgeText(`kind_${note.kind}`) }}</span>
                  <span class="knowledge-status">{{ knowledgeText('stale') }}</span>
                  <p>{{ note.description }}</p>
                </div>
                <div v-for="note in notesFor(entry)?.unversioned ?? []" :key="note.id" class="knowledge-note unversioned">
                  <span class="knowledge-kind">{{ knowledgeText(`kind_${note.kind}`) }}</span>
                  <span class="knowledge-status">{{ knowledgeText('unversioned') }}</span>
                  <p>{{ note.description }}</p>
                </div>
                <p
                  v-if="!(notesFor(entry)?.valid?.length) && !(notesFor(entry)?.stale?.length) && !(notesFor(entry)?.unversioned?.length)"
                  class="muted"
                >
                  {{ knowledgeText('noNotes') }}
                </p>
              </template>
            </template>
            <form
              v-if="knowledgeForm[knowledgeEntryKey(entry.entryKind, entry.name)]"
              class="knowledge-form"
              @submit.prevent="addKnowledgeNote(entry.entryKind, entry.name)"
            >
              <select v-model="knowledgeForm[knowledgeEntryKey(entry.entryKind, entry.name)].kind" class="scan-input">
                <option value="explained_failure">{{ knowledgeText('kind_explained_failure') }}</option>
                <option value="success_condition">{{ knowledgeText('kind_success_condition') }}</option>
                <option value="discriminating_experiment">{{ knowledgeText('kind_discriminating_experiment') }}</option>
                <option value="recheck">{{ knowledgeText('kind_recheck') }}</option>
              </select>
              <textarea
                v-model="knowledgeForm[knowledgeEntryKey(entry.entryKind, entry.name)].description"
                class="scan-input"
                :placeholder="knowledgeText('descriptionLabel')"
                maxlength="2048"
                required
              />
              <input
                v-model="knowledgeForm[knowledgeEntryKey(entry.entryKind, entry.name)].experiment"
                class="scan-input"
                :placeholder="knowledgeText('experimentLabel')"
                maxlength="128"
              />
              <textarea
                v-model="knowledgeForm[knowledgeEntryKey(entry.entryKind, entry.name)].evidenceNote"
                class="scan-input"
                :placeholder="knowledgeText('evidenceLabel')"
                maxlength="2048"
              />
              <button
                type="submit"
                class="btn btn-primary btn-sm"
                :disabled="knowledgeBusy[knowledgeEntryKey(entry.entryKind, entry.name)] || !knowledgeForm[knowledgeEntryKey(entry.entryKind, entry.name)].description.trim()"
              >
                {{ knowledgeText('add') }}
              </button>
            </form>
          </div>
        </details>
        <form v-if="durabilityEditor" class="durability-form" @submit.prevent="saveDurabilityPlan()">
          <strong>{{ durabilityEditor.name }}</strong>
          <label>{{ durabilityText('discovery') }}<input v-model="durabilityForm.discoveryMethod" class="scan-input" required maxlength="512" /></label>
          <label>{{ durabilityText('bytes') }}<input v-model="durabilityForm.expectedBytes" class="scan-input" maxlength="191" placeholder="01 00 00 00" /></label>
          <p class="hint">{{ durabilityText('bytesHint') }}</p>
          <label>{{ durabilityText('test') }}<textarea v-model="durabilityForm.validationTest" class="scan-input" required maxlength="2048" /></label>
          <label>{{ durabilityText('note') }}<textarea v-model="durabilityForm.evidenceNote" class="scan-input" maxlength="2048" /></label>
          <label>{{ durabilityText('alternatives') }}
            <select v-model="durabilityForm.alternativeNames" class="scan-input" multiple>
              <option v-for="entry in (durabilityEditor.entryKind === 'target' ? profileTargets : profilePatches).filter((item) => item.name !== durabilityEditor?.name)" :key="entry.name" :value="entry.name">{{ entry.name }}</option>
            </select>
          </label>
          <button type="submit" class="btn btn-primary btn-sm" :disabled="durabilityBusy || !store.isAttached || durabilityForm.alternativeNames.length > 8">{{ durabilityText('save') }}</button>
          <button type="button" class="btn btn-secondary btn-sm" :disabled="durabilityBusy" @click="durabilityEditor = null">{{ durabilityText('cancel') }}</button>
        </form>
      </div>

      <!-- Sauvegarder une cible -->
      <div class="save-target-box">
        <h3>{{ $t('profile.saveCurrentTargetTitle') }}</h3>
        <p class="hint">
          <template v-if="profileSaveTargets.length === 1">
            {{ $t('profile.addressLabel') }} <code>{{ profileSaveTargets[0].address }}</code>
            {{ $t('profile.typeLabel') }} <code>{{ profileSaveTargets[0].type }}</code>
          </template>
          <template v-else-if="profileSaveTargets.length > 1">
            {{ $t('profile.batchLabel') }} <code>{{ $t('profile.addressesCount', { count: profileSaveTargets.length }) }}</code>
            {{ $t('profile.typeLabel') }} <code>{{ profileSaveTargets[0].type }}</code>
          </template>
          <template v-else>
            {{ $t('profile.addressLabel') }} <code>{{ $t('profile.noneAddress') }}</code>
          </template>
        </p>
        <div class="save-row">
          <input v-model="newTargetName" :placeholder="$t('profile.targetNamePlaceholder')" class="scan-input" />
          <input v-model="newTargetDescription" :placeholder="$t('profile.descriptionPlaceholder')" class="scan-input" />
          <button
            class="btn btn-primary"
            :disabled="profileSaveTargets.length === 0 || !newTargetName.trim()"
            @click="saveCurrentTarget()"
          >
            {{ profileSaveTargets.length > 1 ? $t('profile.saveBatch') : $t('profile.save') }}
          </button>
        </div>
      </div>

      <div v-if="store.clrSelectedObject" class="save-target-box">
        <h3>{{ $t('profile.saveClrFieldTitle') }}</h3>
        <p class="hint">
          {{ $t('profile.objectLabel') }} <code>{{ store.clrSelectedObject.typeName }}</code>
          {{ $t('profile.currentAddressLabel') }} <code>{{ store.clrSelectedObject.address }}</code>
        </p>
        <div class="save-row">
          <select v-model="clrProfileIdentityField" class="scan-input" @change="selectClrIdentityField(clrProfileIdentityField)">
            <option value="">{{ $t('profile.identityFieldOption') }}</option>
            <option v-for="field in clrIdentityFields" :key="`id-${field.name}`" :value="field.name">
              {{ field.name }} = {{ field.value }}
            </option>
          </select>
          <input v-model="clrProfileIdentityValue" :placeholder="$t('profile.identityValuePlaceholder')" class="scan-input" />
          <select v-model="clrProfileTargetField" class="scan-input" @change="selectClrTargetField(clrProfileTargetField)">
            <option value="">{{ $t('profile.targetFieldOption') }}</option>
            <option v-for="field in clrWritableFields" :key="`target-${field.name}`" :value="field.name">
              {{ field.name }} ({{ field.elementType ?? field.typeName ?? $t('profile.primitiveFallback') }})
            </option>
          </select>
          <input v-model="clrProfileValueType" :placeholder="$t('profile.writeTypePlaceholder')" class="scan-input" />
          <button
            class="btn btn-primary"
            :disabled="!canSaveClrProfileTarget"
            @click="saveCurrentClrFieldTarget()"
          >
            {{ $t('profile.saveClr') }}
          </button>
        </div>
      </div>

      <!-- Cibles du profil -->
      <div v-if="profileTargets.length > 0" class="targets-list">
        <div class="targets-header">
          <h3>{{ $t('profile.targetsTitle', { count: profileTargets.length }) }}</h3>
          <button class="btn btn-secondary btn-sm" @click="activateAllTargets()">{{ $t('profile.useAll') }}</button>
        </div>
        <div v-for="group in groupedProfileTargets" :key="group.name" class="target-group">
          <div class="target-group-header">
            <strong>{{ group.name }}</strong>
            <span>{{ $t('profile.targetCount', { count: group.targets.length }) }}</span>
            <button class="btn btn-secondary btn-sm" @click="activateTargetGroup(group)">{{ $t('profile.useGroup') }}</button>
          </div>
          <div v-for="t in group.targets" :key="t.name" class="target-row">
            <div class="target-info">
              <span class="target-name">{{ t.name }}</span>
              <span class="target-type">{{ t.type }}</span>
              <span v-if="t.locatorKind === 'clr_field'" class="target-type">CLR</span>
              <span class="target-locator">{{ t.locator }}</span>
            </div>
            <div class="target-actions">
              <span class="target-resolution" :class="targetResolutionClass(t)">
                {{ targetResolutionLabel(t) }}
              </span>
              <button class="btn btn-secondary btn-sm" @click="verifyTarget(t)">{{ $t('profile.verify') }}</button>
              <button
                class="btn btn-secondary btn-sm"
                :disabled="!store.selectedCandidateAddress || t.locatorKind === 'clr_field'"
                @click="repairTargetWithCurrentAddress(t)"
              >
                {{ $t('profile.repair') }}
              </button>
              <button
                class="btn btn-secondary btn-sm"
                :disabled="t.locatorKind === 'clr_field'"
                @click="activateTarget(t.name)"
              >
                {{ $t('profile.use') }}
              </button>
              <input
                v-model="targetWriteValues[t.name]"
                class="target-write-input"
                :placeholder="$t('profile.valuePlaceholder')"
                @keyup.enter="writeProfileTarget(t)"
              />
              <button
                class="btn btn-primary btn-sm"
                :disabled="!targetWriteValues[t.name]?.trim()"
                @click="writeProfileTarget(t)"
              >
                {{ $t('profile.write') }}
              </button>
            </div>
            <div v-if="t.description" class="target-desc">{{ t.description }}</div>
            <div v-if="t.dependsOn?.length" class="target-desc">{{ $t('profile.dependsOn', { list: t.dependsOn.join(', ') }) }}</div>
            <div v-if="t.ghidraSymbol || t.ghidraNote" class="target-desc">
              {{ $t('profile.ghidraLabel', { symbol: t.ghidraSymbol || $t('profile.unnamedSymbol'), note: t.ghidraNote ? ` · ${t.ghidraNote}` : '' }) }}
            </div>
          </div>
        </div>
      </div>

      <!-- Roadmap section L — Pointer maps : diagnostic groupé après redémarrage -->
      <div v-if="selectedProfile" class="targets-list pointer-map-box">
        <div class="targets-header">
          <h3>{{ $t('profile.verifyAfterRestartTitle') }}</h3>
          <button class="btn btn-secondary btn-sm" :disabled="pointerMapCompareBusy" @click="comparePointerMap()">
            {{ pointerMapCompareBusy ? $t('profile.verifying') : $t('profile.verifyAllTargets') }}
          </button>
        </div>
        <p class="hint">
          {{ $t('profile.verifyAfterRestartHint') }}
        </p>
        <p v-if="pointerMapCompareResult && !pointerMapCompareResult.success" class="error">
          {{ pointerMapCompareResult.error }}
        </p>
        <template v-if="pointerMapResults.length > 0">
          <p class="hint">
            {{ $t('profile.validInvalidCount', { valid: pointerMapCompareResult?.validCount, invalid: pointerMapCompareResult?.invalidCount }) }}
            <template v-if="Number(pointerMapCompareResult?.unsupportedCount ?? 0) > 0">
              {{ $t('profile.unsupportedCount', { count: pointerMapCompareResult?.unsupportedCount }) }}
            </template>
          </p>
          <div v-for="entry in pointerMapResults" :key="String(entry.targetName)" class="target-row">
            <div class="target-info">
              <span class="target-name">{{ entry.targetName }}</span>
              <span class="target-type">{{ entry.locatorKind }}</span>
            </div>
            <div class="target-actions">
              <span
                class="target-resolution"
                :class="{ ok: entry.status === 'valid', fail: entry.status === 'invalid', unsupported: entry.status === 'unsupported' }"
              >
                {{ entry.status === 'valid' ? $t('profile.resolvedAt', { address: entry.address }) : entry.status === 'unsupported' ? $t('profile.unsupportedStatus') : $t('profile.notFoundStatus') }}
              </span>
              <span v-if="entry.previousAddress" class="target-locator">{{ $t('profile.previousAddress', { address: entry.previousAddress }) }}</span>
            </div>
          </div>
        </template>
        <div class="pointer-map-transfer">
          <div class="transfer-actions">
            <button class="btn btn-secondary btn-sm" :disabled="pointerMapTransferBusy" @click="exportPointerMap()">
              {{ pointerMapTransferBusy ? $t('profile.exporting') : $t('profile.exportJson') }}
            </button>
            <label class="replace-toggle">
              <input v-model="pointerMapReplaceExisting" type="checkbox" />
              {{ $t('profile.replaceDuplicates') }}
            </label>
            <button
              class="btn btn-primary btn-sm"
              :disabled="pointerMapTransferBusy || !pointerMapImportText.trim()"
              @click="importPointerMap()"
            >
              {{ $t('profile.import') }}
            </button>
          </div>
          <textarea
            v-model="pointerMapExportText"
            class="pointer-map-textarea"
            readonly
            :placeholder="$t('profile.pointerMapExportPlaceholder')"
          ></textarea>
          <textarea
            v-model="pointerMapImportText"
            class="pointer-map-textarea"
            :placeholder="$t('profile.pointerMapImportPlaceholder')"
          ></textarea>
          <p v-if="pointerMapTransferResult" :class="pointerMapTransferResult.success ? 'hint' : 'error'">
            <template v-if="pointerMapTransferResult.success">
              {{ $t('profile.importedCount', { count: pointerMapTransferResult.imported ?? pointerMapTransferResult.targetCount ?? 0 }) }}
              <template v-if="Number(pointerMapTransferResult.replaced ?? 0) > 0">
                {{ $t('profile.replacedCount', { count: pointerMapTransferResult.replaced }) }}
              </template>
              <template v-if="Number(pointerMapTransferResult.skipped ?? 0) > 0">
                {{ $t('profile.skippedCount', { count: pointerMapTransferResult.skipped }) }}
              </template>
            </template>
            <template v-else>{{ pointerMapTransferResult.error }}</template>
          </p>
        </div>
      </div>

      <div v-if="selectedProfile" class="targets-list ghidra-bridge-box">
        <div class="targets-header">
          <h3>{{ $t('profile.ghidraBridgeTitle') }}</h3>
          <button class="btn btn-secondary btn-sm" :disabled="ghidraBridgeBusy" @click="exportGhidraBridge()">
            {{ ghidraBridgeBusy ? $t('profile.exporting') : $t('profile.exportArtifacts') }}
          </button>
        </div>
        <p class="hint">
          {{ $t('profile.ghidraBridgeHintPart1') }}
          <span class="mono">symbols[]</span> {{ $t('profile.ghidraBridgeHintPart2') }} <span class="mono">module,offset,name,comment</span>{{ $t('profile.ghidraBridgeHintPart3') }}
        </p>
        <div class="ghidra-grid">
          <textarea
            v-model="ghidraExportJson"
            class="pointer-map-textarea"
            readonly
            :placeholder="$t('profile.ghidraJsonPlaceholder')"
          ></textarea>
          <textarea
            v-model="ghidraExportScript"
            class="pointer-map-textarea"
            readonly
            :placeholder="$t('profile.ghidraScriptPlaceholder')"
          ></textarea>
          <textarea
            v-model="ghidraImportText"
            class="pointer-map-textarea ghidra-import-text"
            :placeholder="$t('profile.ghidraImportPlaceholder')"
          ></textarea>
        </div>
        <div class="transfer-actions">
          <button
            class="btn btn-primary btn-sm"
            :disabled="ghidraBridgeBusy || !ghidraImportText.trim()"
            @click="importGhidraSymbols()"
          >
            {{ $t('profile.importSymbols') }}
          </button>
          <span v-if="ghidraBridgeResult" :class="ghidraBridgeResult.success ? 'hint' : 'error'">
            <template v-if="ghidraBridgeResult.success">
              {{ $t('profile.readCount', { count: ghidraBridgeResult.artifactCount ?? ghidraBridgeResult.symbolsRead ?? 0 }) }}
              <template v-if="Number(ghidraBridgeResult.targetsUpdated ?? 0) > 0">
                {{ $t('profile.targetsUpdatedCount', { count: ghidraBridgeResult.targetsUpdated }) }}
              </template>
              <template v-if="Number(ghidraBridgeResult.patchesUpdated ?? 0) > 0">
                {{ $t('profile.patchesUpdatedCount', { count: ghidraBridgeResult.patchesUpdated }) }}
              </template>
              <template v-if="Number(ghidraBridgeResult.unmatched ?? 0) > 0">
                {{ $t('profile.unmatchedCount', { count: ghidraBridgeResult.unmatched }) }}
              </template>
            </template>
            <template v-else>{{ ghidraBridgeResult.error }}</template>
          </span>
        </div>
      </div>

      <div v-if="profilePatches.length > 0" class="patches-list">
        <div class="targets-header">
          <h3>{{ $t('profile.trainerPatchesTitle', { count: profilePatches.length }) }}</h3>
          <div class="trainer-actions">
            <button class="btn btn-secondary btn-sm" :disabled="trainerBusy" @click="inspectProfilePatches()">{{ $t('profile.checkState') }}</button>
            <button class="btn btn-primary btn-sm" :disabled="trainerBusy || !trainerPatchSummary.canApplyAll" @click="applyAllProfilePatches()">{{ $t('profile.applyAll') }}</button>
            <button class="btn btn-secondary btn-sm" :disabled="trainerBusy || !trainerPatchSummary.inspected" @click="restoreAllProfilePatches()">{{ $t('profile.restoreAll') }}</button>
          </div>
        </div>
        <div class="trainer-summary" :class="{ armed: trainerPatchSummary.canApplyAll, blocked: trainerPatchSummary.inspected && (trainerPatchSummary.risky > 0 || trainerPatchSummary.unsafeQuality > 0) }">
          <strong>{{ trainerPatchSummary.inspected ? $t('profile.trainerVerified') : $t('profile.inspectionRequired') }}</strong>
          <span>{{ $t('profile.originalCount', { count: trainerPatchSummary.original }) }}</span>
          <span>{{ $t('profile.activeCount', { count: trainerPatchSummary.active }) }}</span>
          <span>{{ $t('profile.ambiguousCount', { count: trainerPatchSummary.ambiguous }) }}</span>
          <span>{{ $t('profile.missingCount', { count: trainerPatchSummary.missing }) }}</span>
          <span>{{ $t('profile.invalidCount', { count: trainerPatchSummary.invalid }) }}</span>
          <span>{{ $t('profile.lowQualityCount', { count: trainerPatchSummary.unsafeQuality }) }}</span>
          <span :title="$t('profile.relayReadyTitle')">{{ $t('profile.relayReady') }}</span>
        </div>
        <div v-for="patch in profilePatches" :key="patch.name" class="patch-row">
          <div class="patch-info">
            <span class="target-name">{{ patch.name }}</span>
            <span v-if="patch.riskLevel" class="patch-risk">{{ patch.riskLevel }}</span>
            <span
              v-if="patch.signatureLevel"
              class="patch-risk"
              :class="`quality-${patch.signatureLevel}`"
              :title="patch.signatureWarning"
            >
              {{ $t('profile.aobQualityScore', { level: patch.signatureLevel, score: patch.signatureScore ?? 0 }) }}
            </span>
            <span v-if="patch.signatureFixedBytes !== undefined" class="target-locator">
              {{ $t('profile.fixedWildcards', { fixed: patch.signatureFixedBytes, wildcards: patch.signatureWildcardBytes ?? 0 }) }}
            </span>
            <span class="target-resolution" :class="patchStateClass(patch)">{{ patchStateLabel(patch) }}</span>
            <span v-if="patch.module" class="target-locator">{{ patch.module }} +0x{{ patch.moduleOffset }}</span>
            <span v-if="patch.ghidraSymbol" class="patch-risk">{{ $t('profile.ghidraSymbolLabel', { symbol: patch.ghidraSymbol }) }}</span>
          </div>
          <div class="target-actions">
            <label class="patch-toggle" :class="{ active: patchCanRestore(patch), disabled: trainerBusy || (!patchCanApply(patch) && !patchCanRestore(patch)) }">
              <input
                type="checkbox"
                :checked="patchCanRestore(patch)"
                :disabled="trainerBusy || (!patchCanApply(patch) && !patchCanRestore(patch))"
                @change="toggleProfilePatch(patch, $event)"
              />
              <span>{{ patchCanRestore(patch) ? 'ON' : 'OFF' }}</span>
            </label>
            <button class="btn btn-secondary btn-sm" :disabled="trainerBusy || !patchCanRestore(patch)" @click="restoreProfilePatch(patch)">{{ $t('profile.restore') }}</button>
          </div>
          <div class="patch-state-detail">{{ patchStateDetail(patch) }}</div>
          <div v-if="patch.disassembly" class="target-desc">{{ patch.disassembly }}</div>
          <div v-if="patch.aobPattern" class="target-desc">{{ $t('profile.aobPrefix') }} {{ patch.aobPattern }}</div>
          <div v-if="patch.patchBytes" class="target-desc">{{ $t('profile.patchPrefix') }} {{ patch.patchBytes }}</div>
          <div v-if="patch.description" class="target-desc">{{ patch.description }}</div>
        </div>
      </div>

      <!-- Résultat de résolution -->
      <div v-if="resolveResult" class="resolve-box" :class="resolveResult.success ? 'ok' : 'fail'">
        <span v-if="resolveResult.success">✓ {{ $t('profile.readyInAssistant', { address: resolveResult.address }) }}</span>
        <span v-else>✗ {{ resolveResult.error }}</span>
      </div>

      <!-- Supprimer -->
      <button class="btn btn-danger" @click="deleteSelectedProfile()">
        🗑 {{ $t('profile.deleteProfile') }}
      </button>
    </div>

    <!-- Status -->
    <div v-if="statusMessage" class="status-message">{{ statusMessage }}</div>
  </div>
</template>

<style scoped>
.durability-panel {
  margin-bottom: 16px;
}

.durability-entry {
  padding: 10px 0;
  border-bottom: 1px solid var(--border);
  overflow-wrap: anywhere;
}

.durability-entry summary {
  cursor: pointer;
}

.durability-entry p,
.durability-entry ul {
  margin: 8px 0;
}

.durability-form {
  display: grid;
  gap: 10px;
  margin-top: 16px;
}

.durability-form label {
  display: grid;
  gap: 6px;
}

.durability-form .scan-input {
  width: 100%;
  box-sizing: border-box;
}

.durability-form select {
  min-height: 90px;
}

.knowledge-panel {
  margin-top: 12px;
  padding: 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
}

.knowledge-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
}

.knowledge-note {
  margin-top: 8px;
  padding: 8px;
  border-radius: 4px;
  border: 1px solid var(--border);
}

.knowledge-note.stale {
  border-color: var(--warning, #b8860b);
}

.knowledge-note.unversioned {
  opacity: 0.85;
}

.knowledge-kind {
  font-weight: 600;
  margin-right: 8px;
}

.knowledge-status {
  font-size: 0.85em;
  opacity: 0.8;
}

.knowledge-form {
  display: grid;
  gap: 8px;
  margin-top: 10px;
}

.knowledge-form .scan-input {
  width: 100%;
  box-sizing: border-box;
}

.profile-view {
  padding: 24px 32px;
  max-width: 800px;
}

.header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 20px;
}

.header h1 {
  font-size: 22px;
  color: var(--text-primary);
}

.warn-box {
  display: flex;
  flex-direction: column;
  align-items: flex-start;
  gap: 8px;
  background: rgba(224, 175, 104, 0.1);
  border: 1px solid rgba(224, 175, 104, 0.3);
  border-radius: 8px;
  padding: 12px;
  color: var(--warning);
  font-size: 13px;
  margin-bottom: 20px;
}

.section {
  background: var(--bg-tertiary);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 16px;
  margin-bottom: 16px;
}

.section h2 {
  font-size: 15px;
  color: var(--text-primary);
  margin-bottom: 12px;
}

.section h3 {
  font-size: 13px;
  color: var(--text-secondary);
  margin-bottom: 8px;
}

.empty {
  color: var(--text-dim);
  font-size: 13px;
}

.profile-list {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.profile-card {
  background: var(--bg-primary);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 12px;
  cursor: pointer;
  transition: all 0.15s;
}

.profile-card:hover {
  border-color: var(--accent);
}

.profile-card.active {
  border-color: var(--accent);
  background: var(--bg-accent);
}

.profile-name {
  font-size: 14px;
  font-weight: 600;
  color: var(--text-primary);
}

.profile-meta {
  display: flex;
  gap: 12px;
  margin-top: 4px;
  font-size: 12px;
  color: var(--text-dim);
}

.create-row,
.save-row {
  display: flex;
  gap: 8px;
}

.scan-input {
  flex: 1;
  padding: 8px 12px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-size: 13px;
  outline: none;
}

.save-target-box {
  margin-bottom: 16px;
}

.hint {
  font-size: 12px;
  color: var(--text-dim);
  margin-bottom: 8px;
}

.hint code {
  font-family: 'Cascadia Code', monospace;
  color: var(--accent);
}

.targets-list {
  margin-bottom: 16px;
}

.patches-list {
  margin-bottom: 16px;
}

.trainer-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  justify-content: flex-end;
}

.trainer-summary {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  align-items: center;
  margin-bottom: 8px;
  padding: 8px 10px;
  border: 1px solid rgba(224, 175, 104, 0.32);
  border-radius: 6px;
  background: rgba(224, 175, 104, 0.08);
  color: var(--text-dim);
  font-size: 12px;
}

.trainer-summary strong {
  color: var(--warning);
}

.trainer-summary.armed {
  border-color: rgba(158, 206, 106, 0.34);
  background: rgba(158, 206, 106, 0.08);
}

.trainer-summary.armed strong {
  color: var(--success);
}

.trainer-summary.blocked {
  border-color: rgba(247, 118, 142, 0.34);
  background: rgba(247, 118, 142, 0.08);
}

.trainer-summary.blocked strong {
  color: var(--error);
}

.targets-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 12px;
  margin-bottom: 8px;
}

.targets-header h3 {
  margin-bottom: 0;
}

.target-group {
  margin-bottom: 10px;
}

.target-group-header {
  display: grid;
  grid-template-columns: minmax(120px, 1fr) auto auto;
  gap: 8px;
  align-items: center;
  margin-bottom: 6px;
  padding: 7px 9px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: rgba(122, 162, 247, 0.08);
}

.target-group-header strong {
  overflow: hidden;
  color: var(--text-primary);
  font-size: 13px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.target-group-header span {
  color: var(--text-dim);
  font-size: 12px;
}

.target-row {
  background: var(--bg-primary);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 10px 12px;
  margin-bottom: 6px;
  display: grid;
  grid-template-columns: 1fr auto;
  gap: 4px;
}

.target-info {
  display: flex;
  gap: 10px;
  align-items: center;
  font-size: 13px;
}

.target-name {
  font-weight: 600;
  color: var(--text-primary);
}

.target-type {
  color: var(--accent);
  font-size: 12px;
}

.target-locator {
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
  color: var(--text-dim);
}

.target-desc {
  grid-column: 1 / -1;
  font-size: 11px;
  color: var(--text-dim);
  font-style: italic;
}

.patch-row {
  display: grid;
  grid-template-columns: 1fr auto;
  gap: 4px;
  margin-bottom: 6px;
  padding: 10px 12px;
  border: 1px solid rgba(122, 162, 247, 0.16);
  border-radius: 6px;
  background: var(--bg-primary);
}

.patch-info {
  display: flex;
  gap: 10px;
  align-items: center;
  min-width: 0;
  font-size: 13px;
}

.patch-state-detail {
  grid-column: 1 / -1;
  overflow: hidden;
  color: var(--text-dim);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.patch-risk {
  padding: 2px 6px;
  border: 1px solid var(--border);
  border-radius: 999px;
  color: var(--text-dim);
  font-size: 11px;
}

.quality-strong {
  color: var(--success);
}

.quality-medium {
  color: var(--warning);
}

.quality-weak,
.quality-invalid {
  color: var(--error);
}

.patch-toggle {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  min-width: 58px;
  min-height: 28px;
  padding: 0 9px;
  border: 1px solid rgba(224, 175, 104, 0.4);
  border-radius: 999px;
  background: rgba(224, 175, 104, 0.08);
  color: var(--warning);
  cursor: pointer;
  font-size: 12px;
  font-weight: 700;
}

.patch-toggle input {
  position: absolute;
  opacity: 0;
  pointer-events: none;
}

.patch-toggle.active {
  border-color: rgba(158, 206, 106, 0.4);
  background: rgba(158, 206, 106, 0.12);
  color: var(--success);
}

.patch-toggle.disabled {
  cursor: not-allowed;
  opacity: 0.55;
}

.target-actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: flex-end;
  gap: 6px;
}

.target-resolution {
  max-width: 150px;
  overflow: hidden;
  padding: 4px 7px;
  border: 1px solid var(--border);
  border-radius: 999px;
  color: var(--text-dim);
  font-size: 11px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.target-resolution.ok {
  border-color: rgba(158, 206, 106, 0.35);
  color: var(--success);
}

.target-resolution.fail {
  border-color: rgba(247, 118, 142, 0.35);
  color: var(--error);
}

.target-resolution.pending {
  border-color: rgba(224, 175, 104, 0.35);
  color: var(--warning);
}

.target-resolution.warn {
  border-color: rgba(255, 199, 119, 0.5);
  color: var(--warning);
}

.target-resolution.unsupported {
  border-color: var(--border);
  color: var(--text-dim);
}

.pointer-map-box {
  margin-top: 12px;
}

.ghidra-bridge-box {
  margin-top: 12px;
}

.ghidra-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 10px;
  margin-top: 10px;
}

.ghidra-import-text {
  grid-column: 1 / -1;
  min-height: 74px;
}

.pointer-map-transfer {
  display: grid;
  gap: 8px;
  margin-top: 12px;
}

.transfer-actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 8px;
}

.replace-toggle {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  color: var(--text-secondary);
  font-size: 12px;
}

.pointer-map-textarea {
  width: 100%;
  min-height: 96px;
  padding: 8px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
  resize: vertical;
  outline: none;
}

.target-write-input {
  width: 96px;
  min-height: 28px;
  padding: 4px 8px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
  color: var(--text-primary);
  font-size: 12px;
  outline: none;
}

.btn-sm {
  padding: 4px 10px;
  font-size: 12px;
}

.resolve-box {
  padding: 8px 12px;
  border-radius: 6px;
  margin-bottom: 12px;
  font-size: 13px;
}

.resolve-box.ok {
  background: rgba(158, 206, 106, 0.1);
  color: var(--success);
}

.resolve-box.fail {
  background: rgba(247, 118, 142, 0.1);
  color: var(--error);
}

.btn {
  padding: 8px 16px;
  border: none;
  border-radius: 6px;
  font-size: 13px;
  cursor: pointer;
  transition: all 0.15s;
}

.btn-primary {
  background: var(--accent);
  color: var(--bg-primary);
  font-weight: 600;
}

.btn-primary:hover:not(:disabled) {
  background: var(--accent-hover);
}

.btn-secondary {
  background: var(--bg-accent);
  color: var(--text-secondary);
}

.btn-secondary:hover:not(:disabled) {
  background: var(--border);
}

.btn-danger {
  background: var(--error);
  color: white;
  border: none;
}

.btn-danger:hover {
  filter: brightness(1.08);
}

.btn:disabled {
  opacity: 0.4;
  cursor: not-allowed;
}

.status-message {
  margin-top: 16px;
  padding: 10px 14px;
  background: var(--bg-tertiary);
  border: 1px solid var(--border);
  border-radius: 8px;
  font-size: 13px;
  color: var(--text-primary);
}

@media (max-width: 820px) {
  .ghidra-grid {
    grid-template-columns: 1fr;
  }
}
</style>
