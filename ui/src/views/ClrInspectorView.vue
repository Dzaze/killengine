<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { useAppStore } from '@/stores/app'

const store = useAppStore()
const manualAddress = ref('')

const objectFields = computed(() => {
  const fields = store.clrSelectedObject?.fields
  if (!fields || typeof fields !== 'object' || Array.isArray(fields)) return []
  return Object.entries(fields as Record<string, unknown>).map(([name, value]) => ({ name, value }))
})

function valueKind(value: unknown): string {
  if (value === null) return 'null'
  if (typeof value === 'object') return 'ref'
  return typeof value
}

function valueText(value: unknown): string {
  if (value === null) return 'null'
  if (typeof value === 'object') {
    const ref = value as Record<string, unknown>
    const collection = ref.collection as Record<string, unknown> | undefined
    const suffix = collection
      ? ` (${collection.kind ?? 'collection'} ${collection.returned ?? 0}/${collection.count ?? 0})`
      : ''
    return [ref.typeName, ref.address].filter(Boolean).join(' @ ') + suffix
  }
  return String(value)
}

function collectionText(value: unknown): string {
  if (!value || typeof value !== 'object') return ''
  const collection = (value as Record<string, unknown>).collection
  if (!collection || typeof collection !== 'object') return ''
  return JSON.stringify(collection, null, 2)
}

function refAddress(value: unknown): string {
  if (!value || typeof value !== 'object') return ''
  return String((value as Record<string, unknown>).address ?? '')
}

function readManualObject() {
  const address = manualAddress.value.trim()
  if (!address) return
  void store.readClrObject(address)
}

onMounted(() => {
  void store.refreshClrInspectorStatus()
})
</script>

<template>
  <div class="clr-view">
    <div class="header">
      <div>
        <h1>Inspecteur CLR</h1>
        <p>{{ store.isAttached ? store.processName : 'Aucun processus attaché' }}</p>
      </div>
      <div class="header-actions">
        <button class="btn btn-secondary" :disabled="store.clrInspectorBusy" @click="store.refreshClrInspectorStatus()">
          Statut
        </button>
        <button class="btn btn-secondary" :disabled="store.clrInspectorBusy" @click="store.shutdownClrInspector()">
          Stop helper
        </button>
      </div>
    </div>

    <div v-if="!store.isAttached" class="empty-state">
      <p>Attache d'abord une application .NET/CoreCLR autorisée.</p>
      <button class="btn btn-secondary" @click="store.activeView = 'process'">Aller à Processus</button>
    </div>

    <template v-else>
      <section class="status-band">
        <div>
          <span>Helper</span>
          <strong :class="{ ok: store.clrInspectorStatus?.running, warn: !store.clrInspectorStatus?.available }">
            {{ store.clrInspectorStatus?.running ? 'actif' : (store.clrInspectorStatus?.available ? 'prêt' : 'introuvable') }}
          </strong>
        </div>
        <div>
          <span>PID</span>
          <strong>{{ store.clrInspectorStatus?.pid || '-' }}</strong>
        </div>
        <div>
          <span>Pipe</span>
          <code>{{ store.clrInspectorStatus?.pipeName || '-' }}</code>
        </div>
      </section>

      <section class="toolbar">
        <input v-model="store.clrTypeFilter" class="type-input" placeholder="Sous-chaîne de type .NET" />
        <button class="btn btn-primary" :disabled="store.clrInspectorBusy" @click="store.attachClrInspector()">
          Attacher CLR
        </button>
        <button class="btn btn-secondary" :disabled="store.clrInspectorBusy" @click="store.findClrObjects()">
          Objets
        </button>
        <button class="btn btn-secondary" :disabled="store.clrInspectorBusy" @click="store.enumerateClrRoots()">
          Roots
        </button>
        <button class="btn btn-secondary" :disabled="store.clrInspectorBusy" @click="store.flushClrInspectorCache()">
          Flush GC cache
        </button>
        <button class="btn btn-secondary" :disabled="store.clrInspectorBusy" @click="store.detachClrInspector()">
          Détacher CLR
        </button>
      </section>

      <div v-if="store.clrInspectorError" class="alert error">{{ store.clrInspectorError }}</div>
      <div v-else-if="store.clrLastResult && !store.clrLastResult.success" class="alert error">
        {{ store.clrLastResult.error }}
      </div>

      <div class="manual-read">
        <input v-model="manualAddress" class="type-input" placeholder="Adresse objet CLR, ex. 0x2476e00acd8" @keyup.enter="readManualObject" />
        <button class="btn btn-secondary" :disabled="store.clrInspectorBusy || !manualAddress.trim()" @click="readManualObject">
          Lire adresse
        </button>
      </div>

      <div class="content-grid">
        <section class="panel">
          <div class="panel-head">
            <h2>Objets</h2>
            <span>{{ store.clrObjects.length }}</span>
          </div>
          <div class="table-wrap">
            <table>
              <thead>
                <tr>
                  <th>Adresse</th>
                  <th>Type</th>
                  <th>Taille</th>
                  <th></th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="obj in store.clrObjects" :key="obj.address">
                  <td><code>{{ obj.address }}</code></td>
                  <td>{{ obj.typeName }}</td>
                  <td>{{ obj.size ?? '-' }}</td>
                  <td>
                    <button class="mini-btn" :disabled="store.clrInspectorBusy" @click="store.readClrObject(obj.address)">
                      Lire
                    </button>
                  </td>
                </tr>
                <tr v-if="store.clrObjects.length === 0">
                  <td colspan="4" class="empty-row">Aucun objet chargé.</td>
                </tr>
              </tbody>
            </table>
          </div>
        </section>

        <section class="panel">
          <div class="panel-head">
            <h2>Objet lu</h2>
            <span>{{ objectFields.length }} champs</span>
          </div>
          <div v-if="store.clrSelectedObject" class="object-summary">
            <code>{{ store.clrSelectedObject.address }}</code>
            <strong>{{ store.clrSelectedObject.typeName }}</strong>
          </div>
          <div class="table-wrap">
            <table>
              <thead>
                <tr>
                  <th>Champ</th>
                  <th>Kind</th>
                  <th>Valeur</th>
                  <th></th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="field in objectFields" :key="field.name">
                  <td>{{ field.name }}</td>
                  <td>{{ valueKind(field.value) }}</td>
                  <td class="value-cell">
                    <div>{{ valueText(field.value) }}</div>
                    <pre v-if="collectionText(field.value)" class="collection-preview">{{ collectionText(field.value) }}</pre>
                  </td>
                  <td>
                    <button
                      v-if="refAddress(field.value)"
                      class="mini-btn"
                      :disabled="store.clrInspectorBusy"
                      @click="store.readClrObject(refAddress(field.value))"
                    >
                      Lire
                    </button>
                  </td>
                </tr>
                <tr v-if="objectFields.length === 0">
                  <td colspan="4" class="empty-row">Aucun objet sélectionné.</td>
                </tr>
              </tbody>
            </table>
          </div>
        </section>
      </div>

      <section class="panel roots-panel">
        <div class="panel-head">
          <h2>GC roots</h2>
          <span>{{ store.clrRoots.length }}</span>
        </div>
        <div class="table-wrap">
          <table>
            <thead>
              <tr>
                <th>Root</th>
                <th>Kind</th>
                <th>Objet</th>
                <th>Type objet</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="root in store.clrRoots" :key="`${root.rootAddress}-${root.objectAddress}`">
                <td><code>{{ root.rootAddress }}</code></td>
                <td>{{ root.rootKind }}</td>
                <td><code>{{ root.objectAddress }}</code></td>
                <td>{{ root.objectTypeName }}</td>
              </tr>
              <tr v-if="store.clrRoots.length === 0">
                <td colspan="4" class="empty-row">Aucune root chargée.</td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>
    </template>
  </div>
</template>

<style scoped>
.clr-view {
  padding: 24px 32px;
  max-width: 1320px;
}

.header,
.header-actions,
.toolbar,
.manual-read,
.panel-head,
.object-summary {
  display: flex;
  align-items: center;
}

.header {
  justify-content: space-between;
  margin-bottom: 18px;
}

.header h1 {
  color: var(--text-primary);
  font-size: 22px;
}

.header p {
  color: var(--text-dim);
  margin-top: 4px;
}

.header-actions,
.toolbar,
.manual-read {
  gap: 10px;
}

.empty-state,
.alert,
.status-band,
.panel {
  border: 1px solid var(--border);
  background: var(--bg-secondary);
  border-radius: 8px;
}

.empty-state {
  padding: 32px;
  color: var(--text-secondary);
}

.status-band {
  display: grid;
  grid-template-columns: 160px 120px minmax(260px, 1fr);
  gap: 14px;
  padding: 14px;
  margin-bottom: 14px;
}

.status-band span {
  display: block;
  color: var(--text-dim);
  font-size: 12px;
  margin-bottom: 4px;
}

.status-band strong {
  color: var(--text-primary);
}

.status-band strong.ok {
  color: var(--success);
}

.status-band strong.warn {
  color: var(--warning);
}

.toolbar,
.manual-read {
  margin-bottom: 14px;
}

.type-input {
  min-width: 280px;
  flex: 1;
  padding: 10px 12px;
  background: var(--bg-tertiary);
  border: 1px solid var(--border);
  border-radius: 6px;
  color: var(--text-primary);
}

.alert {
  padding: 10px 12px;
  margin-bottom: 14px;
}

.alert.error {
  color: var(--error);
  border-color: rgba(247, 118, 142, 0.45);
}

.content-grid {
  display: grid;
  grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
  gap: 14px;
  margin-bottom: 14px;
}

.panel {
  overflow: hidden;
}

.panel-head {
  justify-content: space-between;
  padding: 12px 14px;
  border-bottom: 1px solid var(--border);
}

.panel-head h2 {
  color: var(--text-primary);
  font-size: 15px;
}

.panel-head span {
  color: var(--text-dim);
  font-size: 13px;
}

.object-summary {
  gap: 10px;
  padding: 10px 14px;
  border-bottom: 1px solid var(--border);
}

.table-wrap {
  max-height: 420px;
  overflow: auto;
}

table {
  width: 100%;
  border-collapse: collapse;
  font-size: 13px;
}

th,
td {
  text-align: left;
  padding: 9px 10px;
  border-bottom: 1px solid rgba(42, 43, 61, 0.7);
  vertical-align: top;
}

th {
  position: sticky;
  top: 0;
  background: var(--bg-secondary);
  color: var(--text-secondary);
  z-index: 1;
}

td {
  color: var(--text-primary);
}

code {
  color: var(--accent);
}

.value-cell {
  max-width: 340px;
  word-break: break-word;
}

.mini-btn {
  padding: 5px 8px;
  background: var(--bg-tertiary);
  color: var(--text-secondary);
  border: 1px solid var(--border);
  border-radius: 6px;
  cursor: pointer;
}

.mini-btn:disabled {
  opacity: 0.5;
  cursor: default;
}

.empty-row {
  color: var(--text-dim);
  text-align: center;
  padding: 22px;
}

@media (max-width: 1000px) {
  .content-grid,
  .status-band {
    grid-template-columns: 1fr;
  }

  .toolbar,
  .manual-read,
  .header {
    align-items: stretch;
    flex-direction: column;
  }
}
</style>
