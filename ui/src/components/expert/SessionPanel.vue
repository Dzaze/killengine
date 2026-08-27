<script setup lang="ts">
import { computed } from 'vue'
import { useAppStore, type SessionEntry } from '@/stores/app'
import InfoDot from '@/components/expert/InfoDot.vue'
import RiskBadge from '@/components/expert/RiskBadge.vue'

const store = useAppStore()

const entries = computed(() => store.sessionEntries)

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
</script>

<template>
  <section class="panel">
    <div class="panel-title">
      <div class="panel-heading">
        <h2>Session active</h2>
        <InfoDot text="Adresses suivies pendant le process attaché : freezes actifs et écritures récentes. La liste est vidée au prochain attach ou detach." />
        <RiskBadge level="write" />
      </div>
      <span>{{ entries.length }} entrée(s)</span>
    </div>

    <div v-if="entries.length === 0" class="session-empty">
      Aucun freeze ou écriture récente dans cette session.
    </div>

    <div v-else class="session-list">
      <div v-for="entry in entries" :key="entry.id" class="session-entry" :class="{ disabled: !entry.enabled, unstable: store.hasFreezeInstability(entry.address) }">
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
        </div>
      </div>
    </div>
  </section>
</template>

<style scoped>
.session-empty {
  padding: 10px 0;
  color: var(--text-dim);
  font-size: 12px;
}

.session-list {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.session-entry {
  display: grid;
  grid-template-columns: minmax(300px, 1.2fr) minmax(160px, 1fr) auto;
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

@media (max-width: 820px) {
  .session-entry {
    grid-template-columns: 1fr;
  }

  .session-main {
    grid-template-columns: minmax(120px, 1fr) 70px 76px 62px;
  }

  .session-actions {
    justify-content: flex-start;
  }
}
</style>
