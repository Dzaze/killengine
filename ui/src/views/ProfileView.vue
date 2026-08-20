<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { useAppStore } from '@/stores/app'
import { backend } from '@/services/backend'

const store = useAppStore()

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
const lastProfileStorageKey = 'killengine.lastProfile'

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
  return normalized || 'cibles'
}

async function refreshProfiles() {
  try {
    profiles.value = (await backend.getController().listProfiles()) as unknown as ProfileEntry[]
    const preferred = selectedProfile.value || localStorage.getItem(lastProfileStorageKey) || ''
    const match = profiles.value.find((profile) => profile.name === preferred)
    if (!selectedProfile.value && match) {
      await selectProfile(match.name)
    }
  } catch {
    profiles.value = []
  }
}

async function selectProfile(name: string) {
  selectedProfile.value = name
  localStorage.setItem(lastProfileStorageKey, name)
  resolveResult.value = null
  try {
    const result = await backend.getController().loadProfile(name)
    profileInfo.value = result
    profileTargets.value = (result.targets as ProfileTargetEntry[]) ?? []
    profilePatches.value = (result.patches as ProfilePatchEntry[]) ?? []
    patchStates.value = {}
    if (store.isAttached && profilePatches.value.length > 0) {
      await inspectProfilePatches()
    }
  } catch {
    profileTargets.value = []
    profilePatches.value = []
    patchStates.value = {}
  }
}

async function saveCurrentTarget() {
  if (!selectedProfile.value) {
    statusMessage.value = '⚠ Sélectionne ou crée d\'abord un profil.'
    return
  }
  if (profileSaveTargets.value.length === 0) {
    statusMessage.value = '⚠ Sélectionne d\'abord une adresse ou termine une recherche dans l\'Assistant.'
    return
  }
  if (!newTargetName.value.trim()) {
    statusMessage.value = '⚠ Donne un nom à la cible.'
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
        statusMessage.value = '✗ ' + (result.error ?? `Erreur de sauvegarde pour ${targetName}.`)
        return
      }
    }

    if (results.every((result) => result.success)) {
      statusMessage.value = targets.length === 1
        ? `✓ Cible "${baseName}" sauvegardée dans "${selectedProfile.value}" (${results[0].locator}).`
        : `✓ ${targets.length} adresses sauvegardées dans "${selectedProfile.value}" sous "${baseName} 1", "${baseName} 2"...`
      newTargetName.value = ''
      newTargetDescription.value = ''
      await selectProfile(selectedProfile.value)
      await refreshProfiles()
    }
  } catch (e) {
    statusMessage.value = '✗ Erreur : ' + String(e)
  }
}

async function createNewProfile() {
  const name = newProfileName.value.trim()
  if (!name) return
  selectedProfile.value = name
  profileTargets.value = []
  profileInfo.value = { gameName: name, executableName: store.processName }
  statusMessage.value = `Profil "${name}" prêt. Sauvegarde une cible pour créer le fichier.`
  newProfileName.value = ''
}

async function activateTarget(targetName: string) {
  if (!selectedProfile.value) return
  try {
    const result = await backend.getController().activateProfileTarget(selectedProfile.value, targetName)
    resolveResult.value = result
    if (result.success) {
      statusMessage.value = `✓ "${targetName}" activé pour l'Assistant à l'adresse 0x${result.address}`
    } else {
      statusMessage.value = '✗ ' + (result.error ?? 'Activation impossible.')
    }
  } catch (e) {
    statusMessage.value = '✗ Erreur : ' + String(e)
  }
}

async function verifyTarget(target: ProfileTargetEntry) {
  if (!selectedProfile.value) return
  try {
    const result = await backend.getController().resolveProfileTarget(selectedProfile.value, target.name)
    targetResolveStates.value = { ...targetResolveStates.value, [target.name]: result }
    resolveResult.value = result
    statusMessage.value = result.success
      ? `✓ "${target.name}" résolu à 0x${result.address}.`
      : `✗ "${target.name}" introuvable : ${result.error ?? 'résolution impossible.'}`
  } catch (e) {
    const result = { success: false, error: String(e) }
    targetResolveStates.value = { ...targetResolveStates.value, [target.name]: result }
    statusMessage.value = '✗ Erreur : ' + String(e)
  }
}

function targetResolutionLabel(target: ProfileTargetEntry): string {
  const state = targetResolveStates.value[target.name]
  if (!state) return 'à vérifier'
  return state.success ? `résolu 0x${state.address}` : 'introuvable'
}

function targetResolutionClass(target: ProfileTargetEntry): string {
  const state = targetResolveStates.value[target.name]
  if (!state) return 'pending'
  return state.success ? 'ok' : 'fail'
}

async function repairTargetWithCurrentAddress(target: ProfileTargetEntry) {
  if (!selectedProfile.value || !store.selectedCandidateAddress) {
    statusMessage.value = '⚠ Sélectionne une adresse dans Expert avant de réparer cette cible.'
    return
  }
  try {
    const result = await backend.getController().saveProfileTarget(
      selectedProfile.value,
      target.name,
      store.selectedCandidateAddress,
      target.type,
      target.description || 'Réparé depuis l’adresse courante.',
    )
    if (result.success) {
      statusMessage.value = `✓ "${target.name}" réparé avec 0x${store.selectedCandidateAddress}.`
      await selectProfile(selectedProfile.value)
      await verifyTarget(target)
    } else {
      statusMessage.value = '✗ ' + (result.error ?? 'Réparation impossible.')
    }
  } catch (e) {
    statusMessage.value = '✗ Erreur : ' + String(e)
  }
}

async function activateAllTargets() {
  if (!selectedProfile.value || profileTargets.value.length === 0) return

  let activated = 0
  for (const target of profileTargets.value) {
    try {
      const result = await backend.getController().activateProfileTarget(selectedProfile.value, target.name)
      if (!result.success) {
        statusMessage.value = '✗ ' + (result.error ?? `Activation impossible pour ${target.name}.`)
        return
      }
      activated += 1
      resolveResult.value = result
    } catch (e) {
      statusMessage.value = '✗ Erreur : ' + String(e)
      return
    }
  }

  statusMessage.value = `✓ ${activated} cible(s) activée(s) pour l'Assistant.`
}

async function activateTargetGroup(group: ProfileTargetGroup) {
  if (!selectedProfile.value || group.targets.length === 0) return

  let activated = 0
  for (const target of group.targets) {
    try {
      const result = await backend.getController().activateProfileTarget(selectedProfile.value, target.name)
      if (!result.success) {
        statusMessage.value = '✗ ' + (result.error ?? `Activation impossible pour ${target.name}.`)
        return
      }
      activated += 1
      resolveResult.value = result
    } catch (e) {
      statusMessage.value = '✗ Erreur : ' + String(e)
      return
    }
  }

  await store.refreshSmartSearchContext()
  statusMessage.value = `✓ Groupe "${group.name}" prêt dans l'Assistant (${activated} cible(s)).`
}

async function writeProfileTarget(target: ProfileTargetEntry) {
  const value = (targetWriteValues.value[target.name] ?? '').trim()
  if (!selectedProfile.value || !value) return

  try {
    const resolved = await backend.getController().resolveProfileTarget(selectedProfile.value, target.name)
    resolveResult.value = resolved
    if (!resolved.success) {
      statusMessage.value = '✗ ' + (resolved.error ?? `Adresse introuvable pour ${target.name}.`)
      return
    }

    const write = await backend
      .getController()
      .writeMemoryValue(String(resolved.address ?? ''), target.type, value)
    if (write.success) {
      statusMessage.value = `✓ "${target.name}" écrit à ${value} sur 0x${resolved.address}.`
      await store.refreshSmartSearchContext()
    } else {
      statusMessage.value = '✗ ' + (write.error || `Écriture impossible pour ${target.name}.`)
    }
  } catch (e) {
    statusMessage.value = '✗ Erreur : ' + String(e)
  }
}

async function deleteSelectedProfile() {
  if (!selectedProfile.value) return
  try {
    const ok = await backend.getController().deleteProfile(selectedProfile.value)
    if (ok) {
      statusMessage.value = `Profil "${selectedProfile.value}" supprimé.`
      selectedProfile.value = ''
      localStorage.removeItem(lastProfileStorageKey)
      profileTargets.value = []
      profilePatches.value = []
      await refreshProfiles()
    } else {
      statusMessage.value = '✗ Échec de la suppression.'
    }
  } catch (e) {
    statusMessage.value = '✗ Erreur : ' + String(e)
  }
}

async function applyProfilePatch(patch: ProfilePatchEntry) {
  if (!selectedProfile.value || !patch.name) return
  if (!patchCanApply(patch)) {
    statusMessage.value = patchStates.value[patch.name]
      ? `⚠ Patch "${patch.name}" non applicable dans son état actuel (${patchStateLabel(patch)}).`
      : (patchQualityBlocksTrainer(patch)
        ? `⚠ Patch "${patch.name}" bloqué : ${patchQualityBlockReason(patch)}.`
        : `⚠ Vérifie l'état de "${patch.name}" avant application.`)
    return
  }
  trainerBusy.value = true
  try {
    const controller = backend.getController()
    if (!controller.applyProfileCodePatch) {
      statusMessage.value = '✗ Fonction patch profil indisponible.'
      return
    }
    const result = await controller.applyProfileCodePatch(selectedProfile.value, patch.name)
    patchStates.value = { ...patchStates.value, [patch.name]: { ...result, active: Boolean(result.success || result.active) } }
    statusMessage.value = result.success
      ? `✓ Patch "${patch.name}" appliqué à 0x${result.matchedAddress ?? result.address ?? ''}.`
        + (result.executableVersionMismatch ? ` ⚠ ${String(result.executableVersionWarning ?? '')}` : '')
      : '✗ ' + (result.error ?? `Patch "${patch.name}" impossible.`)
  } catch (e) {
    statusMessage.value = '✗ Erreur : ' + String(e)
  } finally {
    trainerBusy.value = false
  }
}

async function restoreProfilePatch(patch: ProfilePatchEntry) {
  if (!selectedProfile.value || !patch.name) return
  if (!patchCanRestore(patch)) {
    statusMessage.value = patchStates.value[patch.name]
      ? `⚠ Patch "${patch.name}" non actif, restauration inutile.`
      : `⚠ Vérifie l'état de "${patch.name}" avant restauration.`
    return
  }
  trainerBusy.value = true
  try {
    const controller = backend.getController()
    if (!controller.restoreProfileCodePatch) {
      statusMessage.value = '✗ Fonction restauration patch indisponible.'
      return
    }
    const result = await controller.restoreProfileCodePatch(selectedProfile.value, patch.name)
    patchStates.value = { ...patchStates.value, [patch.name]: { ...result, active: result.success ? false : Boolean(result.active) } }
    statusMessage.value = result.success
      ? `✓ Patch "${patch.name}" restauré.`
      : '✗ ' + (result.error ?? `Restauration "${patch.name}" impossible.`)
  } catch (e) {
    statusMessage.value = '✗ Erreur : ' + String(e)
  } finally {
    trainerBusy.value = false
  }
}

function patchStateLabel(patch: ProfilePatchEntry): string {
  const state = patchStates.value[patch.name]
  if (!state) return 'prêt'
  const status = String(state.status ?? '')
  if (status === 'original') return 'original'
  if (status === 'active') return 'actif'
  if (status === 'ambiguous') return 'ambigu'
  if (status === 'missing') return 'introuvable'
  if (status === 'invalid') return 'invalide'
  if (state.active === true) return 'actif'
  if (state.active === false && state.success) return 'restauré'
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
  if (!state) return 'Vérification requise avant application.'
  const parts = [
    state.matchedAddress ? `0x${state.matchedAddress}` : '',
    state.originalMatches !== undefined ? `original ${state.originalMatches}` : '',
    state.patchedMatches !== undefined ? `patché ${state.patchedMatches}` : '',
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
  return `qualité AOB insuffisante (${patch.signatureScore ?? 0}/100, ${patch.signatureFixedBytes ?? 0} fixe(s))`
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
      statusMessage.value = '✗ Inspection trainer indisponible.'
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
      ? `✓ État trainer: ${result.active} actif(s), ${result.original} original(aux), ${result.ambiguous} ambigu(s).`
      : '✗ ' + (String(result.error ?? 'Inspection trainer incomplète.'))
  } catch (e) {
    statusMessage.value = '✗ Erreur : ' + String(e)
  } finally {
    trainerBusy.value = false
  }
}

async function applyAllProfilePatches() {
  if (!selectedProfile.value || profilePatches.value.length === 0) return
  if (!trainerPatchSummary.value.canApplyAll) {
    statusMessage.value = trainerPatchSummary.value.inspected
      ? (trainerPatchSummary.value.unsafeQuality > 0
        ? `⚠ Application globale bloquée : ${trainerPatchSummary.value.unsafeQuality} patch(s) ont une qualité AOB insuffisante.`
        : '⚠ Application globale bloquée : au moins un patch est ambigu, introuvable ou invalide.')
      : '⚠ Vérifie d’abord l’état trainer avant d’appliquer le lot.'
    return
  }
  trainerBusy.value = true
  try {
    const controller = backend.getController()
    if (!controller.applyAllProfileCodePatches) {
      statusMessage.value = '✗ Fonction trainer profil indisponible.'
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
      ? `✓ ${result.applied}/${result.total} patch(s) appliqué(s), ${result.alreadyActive ?? 0} déjà actif(s).`
      : '✗ ' + (String(result.error ?? 'Application trainer partielle.'))
  } catch (e) {
    statusMessage.value = '✗ Erreur : ' + String(e)
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
      statusMessage.value = '✗ Fonction restauration trainer indisponible.'
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
      ? `✓ ${result.restored} patch(s) restauré(s), ${result.alreadyInactive} déjà inactif(s).`
      : '✗ ' + (String(result.error ?? 'Restauration trainer partielle.'))
  } catch (e) {
    statusMessage.value = '✗ Erreur : ' + String(e)
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
      <h1>Profils</h1>
      <button class="btn btn-secondary" @click="refreshProfiles">↻ Rafraîchir</button>
    </div>

    <div v-if="!store.isAttached" class="warn-box">
      <p>⚠ Attache un processus pour utiliser les profils.</p>
      <button class="btn btn-secondary" @click="store.activeView = 'process'">Aller à Processus</button>
    </div>

    <!-- Liste des profils -->
    <div class="section">
      <h2>Profils enregistrés</h2>
      <div v-if="profiles.length === 0" class="empty">Aucun profil enregistré.</div>
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
            <span v-if="p.targetCount !== undefined">{{ p.targetCount }} cible(s)</span>
            <span v-if="p.patchCount !== undefined">{{ p.patchCount }} patch(s)</span>
          </div>
        </div>
      </div>
    </div>

    <!-- Créer un nouveau profil -->
    <div class="section">
      <h2>Créer un profil</h2>
      <div class="create-row">
        <input
          v-model="newProfileName"
          placeholder="Nom du profil (ex: Mon Jeu)"
          class="scan-input"
          @keyup.enter="createNewProfile()"
        />
        <button class="btn btn-secondary" :disabled="!newProfileName.trim()" @click="createNewProfile()">
          Créer
        </button>
      </div>
    </div>

    <!-- Détails du profil sélectionné -->
    <div v-if="selectedProfile" class="section">
      <h2>{{ selectedProfile }}</h2>

      <!-- Sauvegarder une cible -->
      <div class="save-target-box">
        <h3>Sauvegarder la cible courante</h3>
        <p class="hint">
          <template v-if="profileSaveTargets.length === 1">
            Adresse : <code>{{ profileSaveTargets[0].address }}</code>
            · Type : <code>{{ profileSaveTargets[0].type }}</code>
          </template>
          <template v-else-if="profileSaveTargets.length > 1">
            Lot final : <code>{{ profileSaveTargets.length }} adresses</code>
            · Type : <code>{{ profileSaveTargets[0].type }}</code>
          </template>
          <template v-else>
            Adresse : <code>(aucune)</code>
          </template>
        </p>
        <div class="save-row">
          <input v-model="newTargetName" placeholder="Nom cible (ex: Money)" class="scan-input" />
          <input v-model="newTargetDescription" placeholder="Description (optionnel)" class="scan-input" />
          <button
            class="btn btn-primary"
            :disabled="profileSaveTargets.length === 0 || !newTargetName.trim()"
            @click="saveCurrentTarget()"
          >
            {{ profileSaveTargets.length > 1 ? 'Sauvegarder le lot' : 'Sauvegarder' }}
          </button>
        </div>
      </div>

      <!-- Cibles du profil -->
      <div v-if="profileTargets.length > 0" class="targets-list">
        <div class="targets-header">
          <h3>Cibles ({{ profileTargets.length }})</h3>
          <button class="btn btn-secondary btn-sm" @click="activateAllTargets()">Utiliser tout</button>
        </div>
        <div v-for="group in groupedProfileTargets" :key="group.name" class="target-group">
          <div class="target-group-header">
            <strong>{{ group.name }}</strong>
            <span>{{ group.targets.length }} cible(s)</span>
            <button class="btn btn-secondary btn-sm" @click="activateTargetGroup(group)">Utiliser le groupe</button>
          </div>
          <div v-for="t in group.targets" :key="t.name" class="target-row">
            <div class="target-info">
              <span class="target-name">{{ t.name }}</span>
              <span class="target-type">{{ t.type }}</span>
              <span class="target-locator">{{ t.locator }}</span>
            </div>
            <div class="target-actions">
              <span class="target-resolution" :class="targetResolutionClass(t)">
                {{ targetResolutionLabel(t) }}
              </span>
              <button class="btn btn-secondary btn-sm" @click="verifyTarget(t)">Vérifier</button>
              <button
                class="btn btn-secondary btn-sm"
                :disabled="!store.selectedCandidateAddress"
                @click="repairTargetWithCurrentAddress(t)"
              >
                Réparer
              </button>
              <button class="btn btn-secondary btn-sm" @click="activateTarget(t.name)">Utiliser</button>
              <input
                v-model="targetWriteValues[t.name]"
                class="target-write-input"
                placeholder="Valeur"
                @keyup.enter="writeProfileTarget(t)"
              />
              <button
                class="btn btn-primary btn-sm"
                :disabled="!targetWriteValues[t.name]?.trim()"
                @click="writeProfileTarget(t)"
              >
                Écrire
              </button>
            </div>
            <div v-if="t.description" class="target-desc">{{ t.description }}</div>
          </div>
        </div>
      </div>

      <div v-if="profilePatches.length > 0" class="patches-list">
        <div class="targets-header">
          <h3>Patchs trainer ({{ profilePatches.length }})</h3>
          <div class="trainer-actions">
            <button class="btn btn-secondary btn-sm" :disabled="trainerBusy" @click="inspectProfilePatches()">Vérifier état</button>
            <button class="btn btn-primary btn-sm" :disabled="trainerBusy || !trainerPatchSummary.canApplyAll" @click="applyAllProfilePatches()">Tout appliquer</button>
            <button class="btn btn-secondary btn-sm" :disabled="trainerBusy || !trainerPatchSummary.inspected" @click="restoreAllProfilePatches()">Tout restaurer</button>
          </div>
        </div>
        <div class="trainer-summary" :class="{ armed: trainerPatchSummary.canApplyAll, blocked: trainerPatchSummary.inspected && (trainerPatchSummary.risky > 0 || trainerPatchSummary.unsafeQuality > 0) }">
          <strong>{{ trainerPatchSummary.inspected ? 'Trainer vérifié' : 'Inspection requise' }}</strong>
          <span>original {{ trainerPatchSummary.original }}</span>
          <span>actif {{ trainerPatchSummary.active }}</span>
          <span>ambigu {{ trainerPatchSummary.ambiguous }}</span>
          <span>introuvable {{ trainerPatchSummary.missing }}</span>
          <span>invalide {{ trainerPatchSummary.invalid }}</span>
          <span>qualité faible {{ trainerPatchSummary.unsafeQuality }}</span>
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
              AOB {{ patch.signatureLevel }} · {{ patch.signatureScore ?? 0 }}/100
            </span>
            <span v-if="patch.signatureFixedBytes !== undefined" class="target-locator">
              fixes {{ patch.signatureFixedBytes }} / wildcards {{ patch.signatureWildcardBytes ?? 0 }}
            </span>
            <span class="target-resolution" :class="patchStateClass(patch)">{{ patchStateLabel(patch) }}</span>
            <span v-if="patch.module" class="target-locator">{{ patch.module }} +0x{{ patch.moduleOffset }}</span>
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
            <button class="btn btn-secondary btn-sm" :disabled="trainerBusy || !patchCanRestore(patch)" @click="restoreProfilePatch(patch)">Restaurer</button>
          </div>
          <div class="patch-state-detail">{{ patchStateDetail(patch) }}</div>
          <div v-if="patch.disassembly" class="target-desc">{{ patch.disassembly }}</div>
          <div v-if="patch.aobPattern" class="target-desc">AOB: {{ patch.aobPattern }}</div>
          <div v-if="patch.patchBytes" class="target-desc">Patch: {{ patch.patchBytes }}</div>
          <div v-if="patch.description" class="target-desc">{{ patch.description }}</div>
        </div>
      </div>

      <!-- Résultat de résolution -->
      <div v-if="resolveResult" class="resolve-box" :class="resolveResult.success ? 'ok' : 'fail'">
        <span v-if="resolveResult.success">✓ Prêt dans l'Assistant : 0x{{ resolveResult.address }}</span>
        <span v-else>✗ {{ resolveResult.error }}</span>
      </div>

      <!-- Supprimer -->
      <button class="btn btn-danger" @click="deleteSelectedProfile()">
        🗑 Supprimer ce profil
      </button>
    </div>

    <!-- Status -->
    <div v-if="statusMessage" class="status-message">{{ statusMessage }}</div>
  </div>
</template>

<style scoped>
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
</style>
