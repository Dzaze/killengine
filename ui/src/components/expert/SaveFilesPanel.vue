<script setup lang="ts">
import { useAppStore } from '@/stores/app'
import RiskBadge from '@/components/expert/RiskBadge.vue'
import { formatBytes, formatNumber } from '@/utils/format'

const store = useAppStore()

function formatSaveFileTime(value?: string): string {
  if (!value) return '-'
  const date = new Date(value)
  if (Number.isNaN(date.getTime())) return value
  return date.toLocaleString('fr-FR')
}

function saveFileName(path: string): string {
  return path.split(/[\\/]/).filter(Boolean).pop() || path
}

function discoverSaveFilesFromExpert() {
  void store.discoverSaveFiles(50)
}

function readSaveFileFromExpert(path: string) {
  void store.readSaveFileText(path, 65536)
}
</script>

<template>
  <section class="panel save-file-panel risk-read">
    <div class="panel-title">
      <div class="panel-heading">
        <h2>{{ $t('saveFilesPanel.title') }}</h2>
        <RiskBadge level="read" />
      </div>
      <span v-if="store.discoveredSaveFiles.length > 0">{{ formatNumber(store.discoveredSaveFiles.length) }} fichier(s)</span>
    </div>
    <div class="panel-actions save-file-actions">
      <button
        class="btn btn-primary"
        type="button"
        :disabled="store.saveFilesBusy"
        @click="discoverSaveFilesFromExpert()"
      >
        <span v-if="store.saveFilesBusy" class="btn-spinner" aria-hidden="true"></span>
        {{ store.saveFilesBusy ? $t('saveFilesPanel.discovering') : $t('saveFilesPanel.discover') }}
      </button>
      <span v-if="store.discoveredSaveFilesFamilyName">{{ store.discoveredSaveFilesFamilyName }}</span>
    </div>
    <p v-if="store.saveFileDiscoveryResult?.error" class="error">{{ store.saveFileDiscoveryResult.error }}</p>
    <div v-if="store.discoveredSaveFiles.length > 0" class="save-file-list">
      <button
        v-for="file in store.discoveredSaveFiles"
        :key="file.path"
        class="save-file-row"
        type="button"
        :class="{ selected: store.selectedSaveFilePath === file.path }"
        :disabled="store.saveFileTextBusy"
        @click="readSaveFileFromExpert(file.path)"
      >
        <strong :title="file.path">{{ saveFileName(file.path) }}</strong>
        <span :title="file.path">{{ file.path }}</span>
        <span>{{ formatBytes(file.sizeBytes) }}</span>
        <span>{{ formatSaveFileTime(file.lastWriteTime) }}</span>
      </button>
    </div>
    <div v-else-if="store.saveFileDiscoveryResult?.success" class="empty compact">
      Aucun fichier de sauvegarde probable trouvé.
    </div>
    <div v-if="store.saveFileTextBusy || store.selectedSaveFileText" class="save-file-preview">
      <div class="source-list-title">
        <strong>{{ store.saveFileTextBusy ? $t('saveFilesPanel.reading') : saveFileName(store.selectedSaveFileText?.path || store.selectedSaveFilePath) }}</strong>
        <span v-if="store.selectedSaveFileText?.truncated" class="warning-text">{{ $t('saveFilesPanel.previewTruncated') }}</span>
      </div>
      <p v-if="store.selectedSaveFileText?.error" class="error">{{ store.selectedSaveFileText.error }}</p>
      <textarea
        v-if="store.selectedSaveFileText?.success"
        class="input save-file-textarea"
        readonly
        :value="store.selectedSaveFileText.text || ''"
      ></textarea>

      <div class="save-file-watch-row">
        <button
          class="btn btn-secondary compact"
          type="button"
          :disabled="store.saveFileWatchBusy || !store.selectedSaveFilePath"
          @click="store.watchSelectedSaveFile(store.selectedSaveFilePath, 8000)"
        >
          <span v-if="store.saveFileWatchBusy" class="btn-spinner" aria-hidden="true"></span>
          {{ store.saveFileWatchBusy ? $t('saveFilesPanel.watching') : $t('saveFilesPanel.watchFile') }}
        </button>
        <button
          v-if="store.saveFileWatchBusy"
          class="btn btn-secondary compact"
          type="button"
          @click="store.cancelSaveFileWatchAction()"
        >
          Annuler
        </button>
        <span v-if="store.saveFileWatchResult && !store.saveFileWatchBusy" :class="store.saveFileWatchResult.changed ? 'hint' : 'warning-text'">
          {{ store.saveFileWatchResult.changed
            ? $t('saveFilesPanel.changeDetected', { type: store.saveFileWatchResult.changeType ?? '?' })
            : (store.saveFileWatchResult.cancelled ? $t('saveFilesPanel.watchCancelled') : (store.saveFileWatchResult.error ?? $t('saveFilesPanel.noChangeTimeout'))) }}
        </span>
      </div>

      <div class="save-file-patch-row">
        <input v-model="store.saveFilePatchFindHex" class="input" :placeholder="$t('saveFilesPanel.findBytesPlaceholder')" />
        <input v-model="store.saveFilePatchReplaceHex" class="input" :placeholder="$t('saveFilesPanel.replaceBytesPlaceholder')" />
        <button
          class="btn btn-danger compact"
          type="button"
          :disabled="store.saveFilePatchBusy || !store.saveFilePatchFindHex.trim() || !store.saveFilePatchReplaceHex.trim()"
          @click="store.patchSelectedSaveFileBytes(store.selectedSaveFilePath, store.saveFilePatchFindHex, store.saveFilePatchReplaceHex)"
        >
          {{ store.saveFilePatchBusy ? $t('saveFilesPanel.patching') : $t('saveFilesPanel.patch') }}
        </button>
      </div>
      <p v-if="store.saveFilePatchResult" :class="store.saveFilePatchResult.success ? 'hint' : 'error'">
        {{ store.saveFilePatchResult.success ? $t('saveFilesPanel.patched', { count: store.saveFilePatchResult.occurrencesFound ?? 1 }) : store.saveFilePatchResult.error }}
      </p>
    </div>

    <div class="local-settings-block">
      <div class="panel-actions">
        <button
          class="btn btn-secondary compact"
          type="button"
          :disabled="store.localSettingsBusy"
          @click="store.inspectLocalSettings(200)"
        >
          <span v-if="store.localSettingsBusy" class="btn-spinner" aria-hidden="true"></span>
          {{ store.localSettingsBusy ? $t('saveFilesPanel.inspecting') : $t('saveFilesPanel.inspectLocalSettings') }}
        </button>
        <span v-if="store.localSettingsResult?.count !== undefined">{{ store.localSettingsResult.count }} valeur(s)</span>
      </div>
      <p v-if="store.localSettingsResult?.error" class="error">{{ store.localSettingsResult.error }}</p>
      <div v-if="store.localSettingsResult?.values?.length" class="save-file-list local-settings-list">
        <div v-for="value in store.localSettingsResult.values" :key="`${value.keyPath}/${value.name}`" class="local-settings-row">
          <strong :title="value.keyPath">{{ value.name }}</strong>
          <span>{{ value.type }}</span>
          <span :title="value.preview">{{ value.preview }}</span>
        </div>
      </div>
    </div>
  </section>
</template>

<style scoped>
.save-file-panel {
  display: grid;
  gap: 10px;
}

.save-file-actions {
  justify-content: flex-start;
  flex-wrap: wrap;
}

.save-file-actions span {
  color: var(--text-dim);
  font-size: 12px;
}

.save-file-list {
  display: grid;
  gap: 6px;
}

.save-file-row {
  display: grid;
  grid-template-columns: minmax(130px, 220px) minmax(220px, 1fr) 86px minmax(150px, 190px);
  gap: 8px;
  align-items: center;
  min-height: 34px;
  padding: 7px 8px;
  border: 1px solid rgba(122, 162, 247, 0.16);
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font: inherit;
  font-size: 12px;
  text-align: left;
  cursor: pointer;
}

.save-file-row:hover,
.save-file-row.selected {
  border-color: rgba(122, 162, 247, 0.48);
  background: rgba(122, 162, 247, 0.08);
}

.save-file-row:disabled {
  cursor: wait;
  opacity: 0.65;
}

.save-file-row strong,
.save-file-row span {
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.save-file-row strong {
  color: var(--text-primary);
}

.save-file-preview {
  display: grid;
  gap: 6px;
  padding: 8px;
  border: 1px solid rgba(122, 162, 247, 0.16);
  border-radius: 4px;
  background: rgba(13, 17, 32, 0.42);
}

.save-file-textarea {
  width: 100%;
  min-height: 220px;
  resize: vertical;
  white-space: pre;
  font-family: var(--font-mono, monospace);
  font-size: 12px;
}

.save-file-watch-row,
.save-file-patch-row {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}

.save-file-patch-row .input {
  flex: 1;
  min-width: 140px;
}

.local-settings-block {
  display: grid;
  gap: 6px;
  margin-top: 4px;
  padding-top: 10px;
  border-top: 1px solid rgba(122, 162, 247, 0.16);
}

.local-settings-list {
  max-height: 260px;
  overflow-y: auto;
}

.local-settings-row {
  display: grid;
  grid-template-columns: minmax(120px, 200px) 90px minmax(160px, 1fr);
  gap: 8px;
  align-items: center;
  min-height: 30px;
  padding: 6px 8px;
}
</style>
