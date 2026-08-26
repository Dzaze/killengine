<script setup lang="ts">
import { computed, onMounted } from 'vue'
import { useAppStore } from '@/stores/app'

const store = useAppStore()

const statusLabel = computed(() => {
  if (!store.luaScriptingStatus) return 'non testé'
  if (!store.luaScriptingStatus.available) return 'lua introuvable'
  if (!store.luaScriptingStatus.helperAvailable) return 'helper introuvable'
  return 'prêt'
})

const canRun = computed(() =>
  Boolean(store.luaScriptText.trim()) &&
  !store.luaScriptBusy &&
  store.luaScriptingStatus?.available === true,
)
const canSave = computed(() => Boolean(store.luaScriptText.trim()) && Boolean(store.luaScriptSaveName.trim()))

onMounted(() => {
  void store.refreshLuaScriptingStatus()
  void store.refreshSavedLuaScripts()
})
</script>

<template>
  <div class="scripting-view">
    <div class="header">
      <div>
        <h1>Lua</h1>
        <p>{{ store.isAttached ? store.processName : 'Aucun processus attaché' }}</p>
      </div>
      <div class="header-actions">
        <button class="btn btn-secondary" :disabled="store.luaScriptBusy" @click="store.refreshLuaScriptingStatus()">
          Statut
        </button>
        <button
          class="btn btn-primary"
          :disabled="!canRun"
          @click="store.executeLuaScript()"
        >
          {{ store.luaScriptBusy ? 'Exécution...' : 'Exécuter' }}
        </button>
        <button class="btn btn-danger" :disabled="!store.luaScriptBusy" @click="store.cancelLuaScriptExecution()">
          Stop
        </button>
      </div>
    </div>

    <section class="status-band">
      <div>
        <span>Runtime</span>
        <strong :class="{ ok: store.luaScriptingStatus?.available, warn: !store.luaScriptingStatus?.available }">
          {{ statusLabel }}
        </strong>
      </div>
      <div>
        <span>Lua</span>
        <code>{{ store.luaScriptingStatus?.luaPath || '-' }}</code>
      </div>
      <div>
        <span>Helper</span>
        <code>{{ store.luaScriptingStatus?.helperPath || '-' }}</code>
      </div>
      <div>
        <span>Pipe</span>
        <code>{{ store.luaScriptingStatus?.pipeName || 'KillEngineAutomationPipe' }}</code>
      </div>
    </section>

    <div v-if="store.luaScriptingStatus?.message || store.luaScriptingStatus?.error" class="alert" :class="{ error: store.luaScriptingStatus?.available === false }">
      {{ store.luaScriptingStatus?.error || store.luaScriptingStatus?.message }}
    </div>
    <div v-if="store.luaScriptingStatus?.automationPipeOptIn === false" class="alert warning">
      Les appels <code>ke.call(...)</code> nécessitent KillEngine lancé avec <code>KILLENGINE_AUTOMATION_PIPE=1</code>.
    </div>
    <section class="editor-shell">
      <div class="editor-head">
        <h2>Script</h2>
        <label>
          Timeout
          <input v-model.number="store.luaScriptTimeoutMs" class="small-input" type="number" min="1000" max="120000" step="1000" />
          ms
        </label>
      </div>
      <textarea
        v-model="store.luaScriptText"
        class="script-editor"
        spellcheck="false"
      />
    </section>

    <section class="saved-scripts">
      <div class="saved-scripts-save-row">
        <input
          v-model="store.luaScriptSaveName"
          class="input"
          placeholder="Nom du script (pour le sauvegarder)"
          :disabled="store.luaScriptBusy"
        />
        <button class="btn btn-secondary compact" type="button" :disabled="!canSave" @click="store.saveLuaScript()">
          Sauvegarder dans le profil
        </button>
      </div>
      <p v-if="store.luaScriptSaveResult" :class="store.luaScriptSaveResult.success ? 'hint' : 'error'">
        {{ store.luaScriptSaveResult.success ? `Sauvegardé (${store.luaScriptSaveResult.scriptCount} script(s) dans ce profil)` : store.luaScriptSaveResult.error }}
      </p>

      <div class="saved-scripts-list">
        <div class="saved-scripts-header">
          <h4>Scripts sauvegardés</h4>
          <button class="btn btn-secondary compact" type="button" :disabled="store.luaSavedScriptsBusy" @click="store.refreshSavedLuaScripts()">
            {{ store.luaSavedScriptsBusy ? 'Chargement...' : 'Rafraîchir' }}
          </button>
        </div>
        <p v-if="!store.luaSavedScripts.length" class="hint">Aucun script sauvegardé pour ce profil.</p>
        <div v-for="saved in store.luaSavedScripts" :key="String(saved.name)" class="saved-scripts-entry">
          <span class="saved-scripts-name">{{ saved.name }}</span>
          <div class="saved-scripts-actions">
            <button class="btn btn-secondary compact" type="button" @click="store.loadSavedLuaScript(String(saved.name))">
              Charger
            </button>
            <button class="btn btn-secondary compact" type="button" @click="store.deleteSavedLuaScript(String(saved.name))">
              Supprimer
            </button>
          </div>
        </div>
      </div>
    </section>

    <section class="output-grid">
      <div class="panel">
        <div class="panel-head">
          <h2>stdout</h2>
          <span v-if="store.luaScriptResult">exit {{ store.luaScriptResult.exitCode ?? '-' }}</span>
        </div>
        <pre>{{ store.luaScriptResult?.stdout || '' }}</pre>
      </div>
      <div class="panel">
        <div class="panel-head">
          <h2>stderr</h2>
          <span :class="{ errorText: store.luaScriptResult && !store.luaScriptResult.success }">
            {{ store.luaScriptResult?.success === true ? 'OK' : (store.luaScriptResult ? 'Erreur' : '') }}
          </span>
        </div>
        <pre>{{ store.luaScriptResult?.stderr || store.luaScriptResult?.error || '' }}</pre>
      </div>
    </section>
  </div>
</template>

<style scoped>
.scripting-view {
  display: flex;
  flex-direction: column;
  gap: 16px;
  padding: 24px;
  min-height: 100%;
}

.header,
.editor-head,
.panel-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.header h1,
.panel-head h2,
.editor-head h2 {
  margin: 0;
}

.header p {
  margin: 4px 0 0;
  color: var(--text-dim);
}

.header-actions {
  display: flex;
  gap: 8px;
}

.status-band,
.output-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
  gap: 10px;
}

.status-band > div,
.panel,
.editor-shell {
  border: 1px solid var(--border);
  border-radius: 8px;
  background: var(--bg-secondary);
}

.status-band > div {
  padding: 12px;
  min-width: 0;
}

.status-band span,
.panel-head span,
.editor-head label {
  color: var(--text-dim);
  font-size: 0.85rem;
}

.status-band strong,
.status-band code {
  display: block;
  margin-top: 6px;
  overflow-wrap: anywhere;
}

.ok {
  color: var(--success);
}

.warn,
.errorText {
  color: var(--warning);
}

.alert {
  padding: 10px 12px;
  border: 1px solid var(--border);
  border-radius: 8px;
  background: var(--bg-secondary);
}

.alert.warning {
  border-color: var(--warning);
}

.alert.error {
  border-color: var(--error);
}

.editor-shell {
  padding: 14px;
}

.script-editor {
  width: 100%;
  min-height: 360px;
  margin-top: 12px;
  padding: 12px;
  resize: vertical;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-family: "Cascadia Mono", Consolas, monospace;
  font-size: 0.9rem;
  line-height: 1.45;
}

.small-input {
  width: 96px;
  margin: 0 6px;
}

.btn-danger {
  background: var(--error);
  color: white;
  border: none;
}

.btn-danger:hover:not(:disabled) {
  filter: brightness(1.08);
}

.saved-scripts {
  padding: 14px;
  border: 1px solid var(--border);
  border-radius: 8px;
  background: var(--bg-secondary);
}

.saved-scripts-save-row {
  display: grid;
  grid-template-columns: 1fr auto;
  gap: 8px;
}

.saved-scripts-list {
  margin-top: 10px;
}

.saved-scripts-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 6px;
}

.saved-scripts-header h4 {
  margin: 0;
  color: var(--text-dim);
  font-size: 0.85rem;
}

.saved-scripts-entry {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
  padding: 6px 8px;
  border: 1px solid var(--border);
  border-radius: 6px;
  margin-bottom: 4px;
}

.saved-scripts-name {
  overflow: hidden;
  color: var(--text-primary);
  font-family: "Cascadia Mono", Consolas, monospace;
  font-size: 0.85rem;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.saved-scripts-actions {
  display: flex;
  gap: 6px;
  flex-shrink: 0;
}

.panel {
  min-width: 0;
}

.panel-head {
  padding: 12px;
  border-bottom: 1px solid var(--border);
}

.panel pre {
  min-height: 160px;
  margin: 0;
  padding: 12px;
  overflow: auto;
  white-space: pre-wrap;
  color: var(--text-primary);
}
</style>
