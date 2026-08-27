<script setup lang="ts">
import { computed, ref } from 'vue'
import { useAppStore, type SessionEntry, type SessionGroup } from '@/stores/app'
import InfoDot from '@/components/expert/InfoDot.vue'
import RiskBadge from '@/components/expert/RiskBadge.vue'

const store = useAppStore()

const selectedIds = ref<Set<string>>(new Set())
const entries = computed(() => store.sessionEntries)
const groups = computed(() => store.sessionGroups)
const selectedCount = computed(() => selectedIds.value.size)

function kindLabel(kind: SessionEntry['kind']): string {
  if (kind === 'freeze_breakpoint') return 'Freeze BP'
  if (kind === 'freeze_polling') return 'Freeze'
  return 'Écriture'
}

function createdAtLabel(value: string): string {
  const date = new Date(value)
  if (Number.isNaN(date.getTime())) return '--:--:--'
  return date.toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })
}

function isSelected(id: string): boolean {
  return selectedIds.value.has(id)
}

function setSelected(id: string, selected: boolean) {
  const next = new Set(selectedIds.value)
  if (selected) next.add(id)
  else next.delete(id)
  selectedIds.value = next
}

function clearSelection() {
  selectedIds.value = new Set()
}

function createGroupFromSelection() {
  const created = store.createSessionGroup(Array.from(selectedIds.value))
  if (created) clearSelection()
}

function groupEntries(group: SessionGroup): SessionEntry[] {
  return group.memberIds
    .map((id) => entries.value.find((entry) => entry.id === id))
    .filter((entry): entry is SessionEntry => Boolean(entry))
}

function groupActiveCount(group: SessionGroup): number {
  return groupEntries(group).filter((entry) => entry.enabled).length
}

function selectedMembersInGroup(group: SessionGroup): string[] {
  return group.memberIds.filter((id) => selectedIds.value.has(id))
}

function removeSelectedFromGroup(group: SessionGroup) {
  const removed = selectedMembersInGroup(group)
  if (removed.length === 0) return
  store.removeSessionEntriesFromGroup(group.id, removed)
  clearSelection()
}

function promoteEntry(entry: SessionEntry) {
  void store.promoteSessionEntryToTrainer(entry.id, entry.label || `Session 0x${entry.address}`)
}

function promoteGroup(group: SessionGroup) {
  void store.promoteSessionGroupToTrainer(group.id, group.name || 'Groupe session')
}
</script>

<template>
  <section class="panel">
    <div class="panel-title">
      <div class="panel-heading">
        <h2>Session active</h2>
        <InfoDot text="Adresses suivies pendant le process attaché : freezes actifs et écritures récentes. La liste et les groupes sont vidés au prochain attach ou detach." />
        <RiskBadge level="write" />
      </div>
      <span>{{ entries.length }} entrée(s)</span>
    </div>

    <div v-if="entries.length === 0" class="session-empty">
      Aucun freeze ou écriture récente dans cette session.
    </div>

    <template v-else>
      <div class="session-toolbar">
        <span>{{ selectedCount }} sélectionnée(s)</span>
        <button class="btn compact btn-secondary" type="button" :disabled="selectedCount < 2" @click="createGroupFromSelection">
          Grouper la sélection
        </button>
        <button class="btn compact btn-primary" type="button" :disabled="selectedCount === 0" @click="clearSelection">
          Désélectionner
        </button>
      </div>

      <div v-if="groups.length > 0" class="session-groups">
        <div v-for="group in groups" :key="group.id" class="session-group">
          <div class="session-group-main">
            <input
              class="input session-group-name"
              :value="group.name"
              placeholder="Nom du groupe"
              @input="store.updateSessionGroupName(group.id, ($event.target as HTMLInputElement).value)"
            />
            <span>{{ group.memberIds.length }} membre(s)</span>
            <span>{{ groupActiveCount(group) }} actif(s)</span>
          </div>

          <div class="session-group-members">
            <span v-for="entry in groupEntries(group)" :key="entry.id" class="session-chip" :class="{ disabled: !entry.enabled }">
              0x{{ entry.address }}
            </span>
          </div>

          <div class="session-group-actions">
            <button
              class="btn compact btn-secondary"
              type="button"
              :disabled="groupActiveCount(group) === 0"
              @click="store.disableSessionGroup(group.id)"
            >
              Arrêter groupe
            </button>
            <button
              class="btn compact btn-primary"
              type="button"
              :disabled="selectedMembersInGroup(group).length === 0"
              @click="removeSelectedFromGroup(group)"
            >
              Retirer du groupe
            </button>
            <button class="btn compact btn-primary" type="button" @click="store.removeSessionGroup(group.id)">
              Supprimer
            </button>
            <button
              class="btn compact btn-secondary"
              type="button"
              :disabled="store.isSessionPromotionBusy(group.id)"
              @click="promoteGroup(group)"
            >
              {{ store.isSessionPromotionBusy(group.id) ? 'Promotion...' : 'Promouvoir en Trainer' }}
            </button>
          </div>
        </div>
      </div>

      <div class="session-list">
        <div v-for="entry in entries" :key="entry.id" class="session-entry" :class="{ disabled: !entry.enabled, unstable: store.hasFreezeInstability(entry.address) }">
          <label class="session-select" :aria-label="`Sélectionner 0x${entry.address}`">
            <input type="checkbox" :checked="isSelected(entry.id)" @change="setSelected(entry.id, ($event.target as HTMLInputElement).checked)" />
          </label>

          <div class="session-main">
            <strong>0x{{ entry.address }}</strong>
            <span>{{ entry.valueType }}</span>
            <span class="session-kind">{{ kindLabel(entry.kind) }}</span>
            <span>{{ createdAtLabel(entry.createdAt) }}</span>
          </div>

          <input
            class="input session-label"
            :value="entry.label"
            placeholder="Note session"
            @input="store.updateSessionEntryLabel(entry.id, ($event.target as HTMLInputElement).value)"
          />

          <div class="session-actions">
            <span v-if="store.hasFreezeInstability(entry.address)" class="session-warning">Instable</span>
            <button
              class="btn compact"
              :class="entry.enabled ? 'btn-secondary' : 'btn-primary'"
              type="button"
              :disabled="!entry.enabled || entry.kind === 'write'"
              @click="store.disableSessionEntry(entry.id)"
            >
              {{ entry.enabled ? 'Arrêter' : 'Arrêté' }}
            </button>
            <button
              class="btn compact btn-secondary"
              type="button"
              :disabled="store.isSessionPromotionBusy(entry.id)"
              @click="promoteEntry(entry)"
            >
              {{ store.isSessionPromotionBusy(entry.id) ? 'Promotion...' : 'Promouvoir en Trainer' }}
            </button>
          </div>
        </div>
      </div>
    </template>
  </section>
</template>

<style scoped>
.session-empty {
  padding: 10px 0;
  color: var(--text-dim);
  font-size: 12px;
}

.session-toolbar {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 8px;
  color: var(--text-dim);
  font-size: 12px;
}

.session-groups {
  display: flex;
  flex-direction: column;
  gap: 6px;
  margin-bottom: 8px;
}

.session-group {
  display: grid;
  grid-template-columns: minmax(260px, 1fr) minmax(160px, 1fr) auto;
  gap: 8px;
  align-items: center;
  padding: 8px;
  border: 1px solid color-mix(in srgb, var(--accent) 35%, var(--border));
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font-size: 12px;
}

.session-group-main {
  display: grid;
  grid-template-columns: minmax(120px, 1fr) 86px 68px;
  gap: 8px;
  align-items: center;
  min-width: 0;
}

.session-group-name {
  width: 100%;
  padding: 6px 8px;
  font-size: 12px;
}

.session-group-members {
  display: flex;
  flex-wrap: wrap;
  gap: 4px;
  min-width: 0;
}

.session-chip {
  max-width: 150px;
  overflow: hidden;
  padding: 2px 6px;
  border: 1px solid var(--border);
  border-radius: 6px;
  color: var(--text-secondary);
  font-family: 'Cascadia Code', monospace;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.session-chip.disabled {
  opacity: 0.6;
}

.session-group-actions {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  gap: 6px;
}

.session-list {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.session-entry {
  display: grid;
  grid-template-columns: 24px minmax(300px, 1.2fr) minmax(160px, 1fr) auto;
  gap: 8px;
  align-items: center;
  padding: 8px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font-size: 12px;
}

.session-entry.disabled {
  opacity: 0.7;
}

.session-entry.unstable {
  border-color: color-mix(in srgb, var(--warning) 45%, var(--border));
}

.session-select {
  display: flex;
  align-items: center;
  justify-content: center;
}

.session-select input {
  width: 14px;
  height: 14px;
}

.session-main {
  display: grid;
  grid-template-columns: minmax(120px, 1fr) 78px 78px 64px;
  gap: 8px;
  align-items: center;
  min-width: 0;
}

.session-main strong {
  overflow: hidden;
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  text-overflow: ellipsis;
}

.session-kind {
  color: var(--text-secondary);
}

.session-label {
  width: 100%;
  padding: 6px 8px;
  font-size: 12px;
}

.session-actions {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  gap: 8px;
}

.session-warning {
  color: var(--warning);
  font-size: 11px;
  font-weight: 700;
}

@media (max-width: 980px) {
  .session-group {
    grid-template-columns: 1fr;
  }

  .session-group-actions {
    justify-content: flex-start;
  }
}

@media (max-width: 820px) {
  .session-toolbar {
    flex-wrap: wrap;
  }

  .session-entry {
    grid-template-columns: 24px 1fr;
  }

  .session-main,
  .session-label,
  .session-actions {
    grid-column: 2;
  }

  .session-main {
    grid-template-columns: minmax(120px, 1fr) 70px 76px 62px;
  }

  .session-actions {
    justify-content: flex-start;
  }
}
</style>
