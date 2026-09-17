<script setup lang="ts">
// PORT-3b (docs/PORTABILITY_ROADMAP.md, 17/09/2026) : bannière partagée par
// tous les stores dont la persistance localStorage peut échouer (trainer.ts,
// workspaceItems.ts, workspaceSession.ts) -- rend l'échec visible et permet
// de réessayer ou d'exporter les données encore en mémoire, sans dupliquer
// tout le stockage frontend dans un nouveau système.
import { ref } from 'vue'
import { useI18n } from 'vue-i18n'

const props = defineProps<{
  error: string | null
  retry?: () => void
  exportData?: () => string
}>()

const { t } = useI18n()
const showExport = ref(false)
const exportText = ref('')

function toggleExport() {
  if (!props.exportData) return
  if (!showExport.value) {
    exportText.value = props.exportData()
  }
  showExport.value = !showExport.value
}

function selectAll(event: Event) {
  ;(event.target as HTMLTextAreaElement).select()
}
</script>

<template>
  <div v-if="error" class="persistence-error-banner">
    <div class="persistence-error-line">
      <span>⚠ {{ t('common.persistenceBanner.unsavedChanges') }} — {{ error }}</span>
      <button v-if="retry" class="btn btn-secondary btn-small" @click="retry">
        {{ t('common.persistenceBanner.retry') }}
      </button>
      <button v-if="exportData" class="btn btn-secondary btn-small" @click="toggleExport">
        {{ showExport ? t('common.persistenceBanner.hideExport') : t('common.persistenceBanner.exportInMemory') }}
      </button>
    </div>
    <textarea
      v-if="showExport"
      readonly
      class="persistence-error-export"
      :value="exportText"
      @click="selectAll"
    />
  </div>
</template>

<style scoped>
.persistence-error-banner {
  display: flex;
  flex-direction: column;
  gap: 0.4rem;
  padding: 0.5rem 0.75rem;
  margin: 0.5rem 0;
  background: rgba(220, 53, 69, 0.12);
  border: 1px solid rgba(220, 53, 69, 0.4);
  border-radius: 4px;
  font-size: 0.85rem;
}
.persistence-error-line {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.5rem;
}
.persistence-error-export {
  width: 100%;
  min-height: 80px;
  font-family: monospace;
  font-size: 0.75rem;
}
</style>
