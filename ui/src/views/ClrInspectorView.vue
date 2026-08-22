<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { useAppStore } from '@/stores/app'
import InfoDot from '@/components/expert/InfoDot.vue'
import type { ClrFieldInfo, ClrPathWriteOperation } from '@/services/backend'

const store = useAppStore()
const manualAddress = ref('')
const fieldWriteValues = ref<Record<string, string>>({})
const pathWritePath = ref('')
const pathWriteValue = ref('')
const batchWriteText = ref('')
const locatorType = ref('KillEngine.ClrTestTarget.Player')
const locatorField = ref('Name')
const locatorValue = ref('TestSubject')
const locatorMaxResults = ref(20)

const clrReady = computed(() => Boolean(store.clrInspectorStatus?.running && store.clrInspectorStatus?.attachedProcess))

const guideSteps = computed(() => [
  {
    label: '1. Processus',
    text: store.isAttached ? store.processName : 'Choisir une cible',
    done: store.isAttached,
    active: !store.isAttached,
  },
  {
    label: '2. CLR',
    text: clrReady.value ? 'Session attachée' : 'Attacher le helper',
    done: clrReady.value,
    active: store.isAttached && !clrReady.value,
  },
  {
    label: '3. Objets',
    text: store.clrObjects.length > 0 ? `${store.clrObjects.length} objet(s)` : 'Lister le heap',
    done: store.clrObjects.length > 0,
    active: clrReady.value && store.clrObjects.length === 0,
  },
  {
    label: '4. Action',
    text: store.clrSelectedObject ? 'Lire, locator, écrire' : 'Sélectionner un objet',
    done: Boolean(store.clrSelectedObject),
    active: store.clrObjects.length > 0 && !store.clrSelectedObject,
  },
])

const selectedTypeLabel = computed(() => store.clrSelectedObject?.typeName ?? 'objet CLR')
const pathExamples = ['Self.Health', 'Inventory.Items[0].Value', 'Inventory.QuickSlots[1].Value']

const objectFields = computed<ClrFieldInfo[]>(() => {
  const details = store.clrSelectedObject?.fieldDetails
  if (Array.isArray(details) && details.length > 0) return details

  const fields = store.clrSelectedObject?.fields
  if (!fields || typeof fields !== 'object' || Array.isArray(fields)) return []
  return Object.entries(fields as Record<string, unknown>).map(([name, value]) => ({
    name,
    value,
    kind: valueKind(value),
    writable: false,
  }))
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

function writeKey(field: ClrFieldInfo): string {
  return `${store.clrSelectedObject?.address ?? ''}:${field.name}`
}

function writeField(field: ClrFieldInfo) {
  const objectAddress = store.clrSelectedObject?.address ?? ''
  const value = fieldWriteValues.value[writeKey(field)]?.trim() ?? ''
  if (!objectAddress || !field.name || !value) return
  void store.writeClrPrimitiveField(objectAddress, field.name, value)
}

function writePath() {
  const objectAddress = store.clrSelectedObject?.address ?? ''
  const path = pathWritePath.value.trim()
  const value = pathWriteValue.value.trim()
  if (!objectAddress || !path || !value) return
  void store.writeClrPrimitivePath(objectAddress, path, value)
}

function parseBatchOperations(): ClrPathWriteOperation[] {
  return batchWriteText.value
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter(Boolean)
    .map((line) => {
      const separator = line.indexOf('=')
      if (separator < 0) return { path: '', value: '' }
      return {
        path: line.slice(0, separator).trim(),
        value: line.slice(separator + 1).trim(),
      }
    })
    .filter((operation) => operation.path && operation.value)
}

function writeBatch() {
  const objectAddress = store.clrSelectedObject?.address ?? ''
  const operations = parseBatchOperations()
  if (!objectAddress || operations.length === 0) return
  void store.writeClrPrimitivePathBatch(objectAddress, operations)
}

function usePathExample(path: string) {
  pathWritePath.value = path
}

function trainerValueType(field: ClrFieldInfo): string {
  const typeName = String(field.typeName ?? field.elementType ?? '').toLowerCase()
  if (typeName.includes('uint64') || typeName.includes('ulong')) return 'UInt64'
  if (typeName.includes('int64') || typeName.includes('long')) return 'Int64'
  if (typeName.includes('uint32') || typeName.includes('uint')) return 'UInt32'
  if (typeName.includes('int32') || typeName.includes('int')) return 'Int32'
  if (typeName.includes('uint16') || typeName.includes('ushort')) return 'UInt16'
  if (typeName.includes('int16') || typeName.includes('short')) return 'Int16'
  if (typeName.includes('double')) return 'Float64'
  if (typeName.includes('single') || typeName.includes('float')) return 'Float32'
  return 'Int32'
}

function createTrainerFeature(field: ClrFieldInfo) {
  const selected = store.clrSelectedObject
  if (!selected || !field.writable || !locatorType.value.trim() || !locatorField.value.trim() || !locatorValue.value.trim()) return
  const value = fieldWriteValues.value[writeKey(field)]?.trim() || String(field.value ?? '')
  const feature = store.createTrainerClrFieldFeature({
    name: `CLR ${field.name}`,
    typeSubstring: locatorType.value,
    identityField: locatorField.value,
    identityValue: locatorValue.value,
    targetField: field.name,
    valueType: trainerValueType(field),
    value,
    address: selected.address,
  })
  if (feature) store.activeView = 'trainer'
}

function canUseAsLocator(field: ClrFieldInfo): boolean {
  return field.kind === 'primitive' || field.kind === 'string' || typeof field.value === 'string' || typeof field.value === 'number' || typeof field.value === 'boolean'
}

function useFieldAsLocator(field: ClrFieldInfo) {
  if (!store.clrSelectedObject || !canUseAsLocator(field)) return
  locatorType.value = store.clrSelectedObject.typeName
  locatorField.value = field.name
  locatorValue.value = String(field.value ?? '')
}

function useDefaultClrTarget() {
  locatorType.value = 'KillEngine.ClrTestTarget.Player'
  locatorField.value = 'Name'
  locatorValue.value = 'TestSubject'
  store.clrTypeFilter = 'KillEngine.ClrTestTarget'
}

function runFieldLocator() {
  void store.findClrObjectsByFieldValue(locatorType.value, locatorField.value, locatorValue.value, locatorMaxResults.value)
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
      <strong>Aucune cible active</strong>
      <p>Choisis d'abord un processus .NET/CoreCLR autorisé. L'inspecteur CLR n'agit que sur le processus attaché.</p>
      <button class="btn btn-primary" aria-label="Ouvrir la vue Processus pour choisir une cible" @click="store.activeView = 'process'">
        Aller à Processus
      </button>
    </div>

    <template v-else>
      <section class="novice-guide" aria-label="Parcours guidé CLR">
        <div class="guide-head">
          <div>
            <span>Parcours guidé</span>
            <strong>{{ clrReady ? 'CLR prêt' : 'Démarre par Attacher CLR' }}</strong>
          </div>
          <InfoDot text="Le tas CLR peut bouger après un GC. Pour une cible durable, préfère le locator par champ et les actions Trainer CLR plutôt qu'une ancienne adresse brute." align="right" />
        </div>
        <ol class="guide-steps">
          <li
            v-for="step in guideSteps"
            :key="step.label"
            :class="{ done: step.done, active: step.active }"
          >
            <span>{{ step.label }}</span>
            <strong>{{ step.text }}</strong>
          </li>
        </ol>
      </section>

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
        <label class="field-label">
          <span>Filtre type</span>
          <input v-model="store.clrTypeFilter" class="type-input" placeholder="Ex. KillEngine.ClrTestTarget" />
        </label>
        <button
          class="btn btn-primary"
          :disabled="store.clrInspectorBusy"
          title="Lance le helper ClrMD et l'attache au processus courant."
          @click="store.attachClrInspector()"
        >
          Attacher CLR
        </button>
        <button
          class="btn btn-secondary"
          :disabled="store.clrInspectorBusy || !clrReady"
          title="Liste les objets managés dont le nom de type contient le filtre."
          @click="store.findClrObjects()"
        >
          Objets
        </button>
        <button
          class="btn btn-secondary"
          :disabled="store.clrInspectorBusy || !clrReady"
          title="Affiche les racines GC visibles par ClrMD."
          @click="store.enumerateClrRoots()"
        >
          Roots
        </button>
        <button
          class="btn btn-secondary"
          :disabled="store.clrInspectorBusy || !clrReady"
          title="À utiliser après un GC ou une grosse mutation côté cible pour forcer ClrMD à relire l'état courant."
          @click="store.flushClrInspectorCache()"
        >
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
        <label class="field-label">
          <span>Lire une adresse connue</span>
          <input v-model="manualAddress" class="type-input" placeholder="Ex. 0x2476e00acd8" @keyup.enter="readManualObject" />
        </label>
        <button
          class="btn btn-secondary"
          :disabled="store.clrInspectorBusy || !manualAddress.trim()"
          aria-label="Lire l'objet CLR à l'adresse saisie"
          @click="readManualObject"
        >
          Lire adresse
        </button>
      </div>

      <section class="locator-panel">
        <div class="locator-head">
          <div>
            <span>Locator stable</span>
            <strong>Retrouver un objet par identité</strong>
          </div>
          <button class="mini-btn" type="button" @click="useDefaultClrTarget">Exemple cible test</button>
        </div>
        <div class="locator-inputs">
          <label class="field-label">
            <span>Type</span>
            <input v-model="locatorType" class="type-input" placeholder="Ex. KillEngine.ClrTestTarget.Player" />
          </label>
          <label class="field-label compact-label">
            <span>Champ identité</span>
            <input v-model="locatorField" class="locator-small-input" placeholder="Ex. Name" />
          </label>
          <label class="field-label compact-label">
            <span>Valeur</span>
            <input v-model="locatorValue" class="locator-small-input" placeholder="Ex. TestSubject" @keyup.enter="runFieldLocator" />
          </label>
          <label class="field-label count-label">
            <span>Max</span>
            <input v-model.number="locatorMaxResults" class="locator-count-input" type="number" min="1" max="200" />
          </label>
          <button
            class="btn btn-secondary"
            :disabled="store.clrInspectorBusy || !locatorType.trim() || !locatorField.trim() || !locatorValue.trim()"
            aria-label="Retrouver les objets CLR qui correspondent au locator"
            @click="runFieldLocator"
          >
            Retrouver
          </button>
        </div>
        <div v-if="store.clrFieldLocatorResult" class="locator-results">
          <span>
            {{ store.clrFieldLocatorResult.matchesReturned }} match(es),
            {{ store.clrFieldLocatorResult.typeMatches }} objet(s) du type,
            {{ store.clrFieldLocatorResult.scannedObjects }} scanné(s)
          </span>
          <button
            v-for="match in store.clrFieldLocatorResult.matches"
            :key="match.address"
            class="locator-match"
            :disabled="store.clrInspectorBusy"
            :aria-label="`Lire l'objet ${match.typeName} à l'adresse ${match.address}`"
            @click="store.readClrObject(match.address)"
          >
            {{ match.typeName }} @ {{ match.address }}
          </button>
        </div>
      </section>

      <div class="content-grid">
        <section class="panel">
          <div class="panel-head">
            <h2>Objets</h2>
            <span>{{ store.clrObjects.length }}</span>
          </div>
          <div v-if="clrReady && store.clrObjects.length === 0" class="panel-hint">
            Lance `Objets` avec un filtre court, puis clique `Lire` sur une ligne.
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
                  <td colspan="4" class="empty-row">Aucun objet listé pour l'instant.</td>
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
          <div v-else class="panel-hint">
            Sélectionne un objet dans la liste ou colle une adresse CLR connue.
          </div>
          <div v-if="store.clrSelectedObject" class="path-write">
            <div class="path-write-head">
              <span>Chemin symbolique depuis {{ selectedTypeLabel }}</span>
              <div class="path-examples">
                <button
                  v-for="example in pathExamples"
                  :key="example"
                  class="mini-btn"
                  type="button"
                  @click="usePathExample(example)"
                >
                  {{ example }}
                </button>
              </div>
            </div>
            <input
              v-model="pathWritePath"
              class="path-input"
              placeholder="Champ ou chemin"
              aria-label="Chemin CLR symbolique"
              @keyup.enter="writePath"
            />
            <input
              v-model="pathWriteValue"
              class="path-value-input"
              placeholder="Valeur"
              aria-label="Nouvelle valeur du champ CLR"
              @keyup.enter="writePath"
            />
            <button
              class="btn btn-secondary"
              :disabled="store.clrInspectorBusy || !pathWritePath.trim() || !pathWriteValue.trim()"
              title="Écrit uniquement un champ primitif feuille atteint par ce chemin."
              @click="writePath"
            >
              Écrire chemin
            </button>
            <div class="batch-write">
              <label class="field-label">
                <span>Transaction multi-champs</span>
                <textarea
                  v-model="batchWriteText"
                  class="batch-input"
                  rows="3"
                  placeholder="Health=100&#10;Stats.Rank=7&#10;Inventory.Currencies[gold]=4125"
                  aria-label="Operations de transaction CLR, une ligne chemin egal valeur"
                ></textarea>
              </label>
              <button
                class="btn btn-secondary"
                :disabled="store.clrInspectorBusy || parseBatchOperations().length === 0"
                title="Applique les lignes dans l'ordre et tente un rollback si une opération échoue."
                @click="writeBatch"
              >
                Transaction
              </button>
            </div>
          </div>
          <div class="table-wrap">
            <table>
              <thead>
                <tr>
                  <th>Champ</th>
                  <th>Kind</th>
                  <th>Adresse</th>
                  <th>Valeur</th>
                  <th></th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="field in objectFields" :key="field.name">
                  <td>{{ field.name }}</td>
                  <td>{{ field.elementType ?? field.kind ?? valueKind(field.value) }}</td>
                  <td><code>{{ field.address ?? '-' }}</code></td>
                  <td class="value-cell">
                    <div>{{ valueText(field.value) }}</div>
                    <pre v-if="collectionText(field.value)" class="collection-preview">{{ collectionText(field.value) }}</pre>
                  </td>
                  <td class="field-actions">
                    <button
                      v-if="refAddress(field.value)"
                      class="mini-btn"
                      :disabled="store.clrInspectorBusy"
                      @click="store.readClrObject(refAddress(field.value))"
                    >
                      Lire
                    </button>
                    <template v-else-if="field.writable">
                      <input
                        v-model="fieldWriteValues[writeKey(field)]"
                        class="field-write-input"
                        :placeholder="String(field.value ?? '')"
                        @keyup.enter="writeField(field)"
                      />
                      <button
                        class="mini-btn"
                        :disabled="store.clrInspectorBusy || !fieldWriteValues[writeKey(field)]?.trim()"
                        :aria-label="`Écrire le champ CLR ${field.name}`"
                        @click="writeField(field)"
                      >
                        Écrire
                      </button>
                      <button
                        class="mini-btn"
                        :disabled="store.clrInspectorBusy || !locatorType.trim() || !locatorField.trim() || !locatorValue.trim()"
                        :aria-label="`Créer une feature Trainer pour le champ CLR ${field.name}`"
                        @click="createTrainerFeature(field)"
                      >
                        Trainer
                      </button>
                    </template>
                    <button
                      v-if="canUseAsLocator(field)"
                      class="mini-btn"
                      :disabled="store.clrInspectorBusy"
                      :aria-label="`Utiliser le champ ${field.name} comme locator CLR`"
                      @click="useFieldAsLocator(field)"
                    >
                      Locator
                    </button>
                  </td>
                </tr>
                <tr v-if="objectFields.length === 0">
                  <td colspan="5" class="empty-row">Aucun objet sélectionné.</td>
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

.novice-guide,
.empty-state,
.alert,
.status-band,
.locator-panel,
.panel {
  border: 1px solid var(--border);
  background: var(--bg-secondary);
  border-radius: 8px;
}

.empty-state {
  padding: 28px;
  color: var(--text-secondary);
}

.empty-state strong {
  display: block;
  margin-bottom: 8px;
  color: var(--text-primary);
}

.empty-state p {
  max-width: 560px;
  margin-bottom: 16px;
}

.novice-guide {
  padding: 14px;
  margin-bottom: 14px;
}

.guide-head,
.locator-head,
.path-write-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.guide-head {
  margin-bottom: 12px;
}

.guide-head span,
.locator-head span,
.path-write-head span,
.field-label span {
  display: block;
  color: var(--text-dim);
  font-size: 12px;
  margin-bottom: 4px;
}

.guide-head strong,
.locator-head strong {
  color: var(--text-primary);
  font-size: 14px;
}

.guide-steps {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 8px;
  list-style: none;
  margin: 0;
  padding: 0;
}

.guide-steps li {
  min-width: 0;
  padding: 9px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
}

.guide-steps li.done {
  border-color: rgba(99, 230, 190, 0.38);
}

.guide-steps li.active {
  border-color: var(--accent);
}

.guide-steps span {
  display: block;
  color: var(--text-dim);
  font-size: 11px;
  margin-bottom: 3px;
}

.guide-steps strong {
  display: block;
  color: var(--text-primary);
  font-size: 12px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
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

.field-label {
  display: flex;
  flex: 1;
  min-width: 220px;
  flex-direction: column;
}

.compact-label {
  flex: 0 0 150px;
  min-width: 150px;
}

.count-label {
  flex: 0 0 74px;
  min-width: 74px;
}

.locator-panel {
  padding: 12px;
  margin-bottom: 14px;
}

.locator-head {
  margin-bottom: 10px;
}

.locator-inputs,
.locator-results {
  display: flex;
  align-items: center;
  gap: 8px;
}

.locator-results {
  flex-wrap: wrap;
  margin-top: 10px;
  color: var(--text-secondary);
  font-size: 12px;
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

.locator-small-input {
  width: 150px;
  padding: 10px 12px;
  background: var(--bg-tertiary);
  border: 1px solid var(--border);
  border-radius: 6px;
  color: var(--text-primary);
}

.locator-count-input {
  width: 74px;
  padding: 10px 12px;
  background: var(--bg-tertiary);
  border: 1px solid var(--border);
  border-radius: 6px;
  color: var(--text-primary);
}

.locator-match {
  padding: 4px 7px;
  background: var(--bg-tertiary);
  color: var(--accent);
  border: 1px solid var(--border);
  border-radius: 6px;
  cursor: pointer;
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

.panel-hint {
  padding: 10px 14px;
  color: var(--text-secondary);
  border-bottom: 1px solid var(--border);
  font-size: 13px;
}

.object-summary {
  gap: 10px;
  padding: 10px 14px;
  border-bottom: 1px solid var(--border);
}

.path-write {
  display: grid;
  grid-template-columns: minmax(240px, 1fr) 120px auto;
  gap: 8px;
  padding: 12px 14px;
  border-bottom: 1px solid var(--border);
}

.path-write-head {
  grid-column: 1 / -1;
}

.batch-write {
  grid-column: 1 / -1;
  display: grid;
  grid-template-columns: minmax(240px, 1fr) auto;
  gap: 8px;
  align-items: end;
}

.path-examples {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  justify-content: flex-end;
}

.path-input,
.path-value-input,
.batch-input {
  min-width: 0;
  padding: 8px 10px;
  background: var(--bg-tertiary);
  border: 1px solid var(--border);
  border-radius: 6px;
  color: var(--text-primary);
}

.batch-input {
  width: 100%;
  resize: vertical;
  font-family: inherit;
  line-height: 1.35;
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

.field-actions {
  min-width: 180px;
  white-space: nowrap;
}

.field-write-input {
  width: 92px;
  margin-right: 6px;
  padding: 5px 7px;
  background: var(--bg-tertiary);
  border: 1px solid var(--border);
  border-radius: 6px;
  color: var(--text-primary);
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
  .path-write,
  .batch-write,
  .guide-steps,
  .status-band {
    grid-template-columns: 1fr;
  }

  .toolbar,
  .locator-inputs,
  .manual-read,
  .guide-head,
  .locator-head,
  .path-write-head,
  .header {
    align-items: stretch;
    flex-direction: column;
  }

  .locator-small-input,
  .locator-count-input {
    width: auto;
  }

  .compact-label,
  .count-label,
  .field-label {
    flex-basis: auto;
    min-width: 0;
  }

  .path-examples {
    justify-content: flex-start;
  }
}
</style>
