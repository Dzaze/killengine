<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { useAppStore } from '@/stores/app'
import { backend } from '@/services/backend'

const store = useAppStore()

interface ProfileEntry {
  name: string
  gameName?: string
  executableName?: string
  targetCount?: number
}

interface ProfileTargetEntry {
  name: string
  type: string
  locator: string
  description: string
}

const profiles = ref<ProfileEntry[]>([])
const selectedProfile = ref<string>('')
const profileTargets = ref<ProfileTargetEntry[]>([])
const profileInfo = ref<Record<string, unknown>>({})
const newProfileName = ref('')
const newTargetName = ref('')
const newTargetDescription = ref('')
const resolveResult = ref<Record<string, unknown> | null>(null)
const statusMessage = ref('')

async function refreshProfiles() {
  try {
    profiles.value = (await backend.getController().listProfiles()) as unknown as ProfileEntry[]
  } catch {
    profiles.value = []
  }
}

async function selectProfile(name: string) {
  selectedProfile.value = name
  resolveResult.value = null
  try {
    const result = await backend.getController().loadProfile(name)
    profileInfo.value = result
    profileTargets.value = (result.targets as ProfileTargetEntry[]) ?? []
  } catch {
    profileTargets.value = []
  }
}

async function saveCurrentTarget() {
  if (!selectedProfile.value) {
    statusMessage.value = '⚠ Sélectionne ou crée d\'abord un profil.'
    return
  }
  if (!store.selectedCandidateAddress) {
    statusMessage.value = '⚠ Sélectionne d\'abord une adresse dans l\'Assistant.'
    return
  }
  if (!newTargetName.value.trim()) {
    statusMessage.value = '⚠ Donne un nom à la cible.'
    return
  }

  try {
    const result = await backend.getController().saveProfileTarget(
      selectedProfile.value,
      newTargetName.value.trim(),
      store.selectedCandidateAddress,
      store.exactScanType,
      newTargetDescription.value.trim(),
    )
    if (result.success) {
      statusMessage.value = `✓ Cible "${newTargetName.value}" sauvegardée dans "${selectedProfile.value}" (${result.locator}).`
      newTargetName.value = ''
      newTargetDescription.value = ''
      await selectProfile(selectedProfile.value)
      await refreshProfiles()
    } else {
      statusMessage.value = '✗ ' + (result.error ?? 'Erreur de sauvegarde.')
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

async function resolveTarget(targetName: string) {
  if (!selectedProfile.value) return
  try {
    const result = await backend.getController().resolveProfileTarget(selectedProfile.value, targetName)
    resolveResult.value = result
    if (result.success) {
      statusMessage.value = `✓ "${targetName}" résolu à l'adresse 0x${result.address}`
    } else {
      statusMessage.value = '✗ ' + (result.error ?? 'Résolution impossible.')
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
      profileTargets.value = []
      await refreshProfiles()
    } else {
      statusMessage.value = '✗ Échec de la suppression.'
    }
  } catch (e) {
    statusMessage.value = '✗ Erreur : ' + String(e)
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
      ⚠ Attache un processus pour utiliser les profils.
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
          Adresse : <code>{{ store.selectedCandidateAddress || '(aucune)' }}</code>
          · Type : <code>{{ store.exactScanType }}</code>
        </p>
        <div class="save-row">
          <input v-model="newTargetName" placeholder="Nom cible (ex: Money)" class="scan-input" />
          <input v-model="newTargetDescription" placeholder="Description (optionnel)" class="scan-input" />
          <button
            class="btn btn-primary"
            :disabled="!store.selectedCandidateAddress || !newTargetName.trim()"
            @click="saveCurrentTarget()"
          >
            Sauvegarder
          </button>
        </div>
      </div>

      <!-- Cibles du profil -->
      <div v-if="profileTargets.length > 0" class="targets-list">
        <h3>Cibles ({{ profileTargets.length }})</h3>
        <div v-for="t in profileTargets" :key="t.name" class="target-row">
          <div class="target-info">
            <span class="target-name">{{ t.name }}</span>
            <span class="target-type">{{ t.type }}</span>
            <span class="target-locator">{{ t.locator }}</span>
          </div>
          <div class="target-actions">
            <button class="btn btn-secondary btn-sm" @click="resolveTarget(t.name)">Résoudre</button>
          </div>
          <div v-if="t.description" class="target-desc">{{ t.description }}</div>
        </div>
      </div>

      <!-- Résultat de résolution -->
      <div v-if="resolveResult" class="resolve-box" :class="resolveResult.success ? 'ok' : 'fail'">
        <span v-if="resolveResult.success">✓ Adresse résolue : 0x{{ resolveResult.address }}</span>
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
  background: rgba(247, 118, 142, 0.15);
  color: var(--error);
  border: 1px solid rgba(247, 118, 142, 0.3);
}

.btn-danger:hover {
  background: rgba(247, 118, 142, 0.25);
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