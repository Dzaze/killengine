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
const callMethodName = ref('')
const callMethodValue = ref('')
const locatorType = ref('KillEngine.ClrTestTarget.Player')
const locatorField = ref('Name')
const locatorValue = ref('TestSubject')
const locatorMaxResults = ref(20)
// PHASE 59 : quand actif, "Écrire chemin"/"Transaction" relocalisent
// l'objet via le locator (type/champ identité/valeur identité) juste avant
// d'écrire, au lieu d'utiliser l'adresse de l'objet actuellement lu -- utile
// quand l'objet a pu bouger depuis la dernière lecture (GC compactant).
const useLocatorForWrite = ref(false)
// PHASE 59 : quand actif, la Transaction multi-champs suspend TOUTES les
// threads du process attaché pendant l'écriture (killcore::
// ProcessThreadsSuspendGuard) -- plus sûr contre une lecture/écriture
// concurrente d'une autre thread cible, mais best-effort (pas une garantie
// absolue d'absence de deadlock, voir docs/KILLENGINE_CLR_INSPECTOR_SPEC.md).
const suspendDuringBatch = ref(false)
const gcRootTargetAddress = ref('')
const disassembleMethodName = ref('')
const disassembleInstructionCount = ref(24)
const reportMaxDepth = ref(3)
const reportMaxNodes = ref(50)
const reportIncludeGcRootChain = ref(true)

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
  const path = pathWritePath.value.trim()
  const value = pathWriteValue.value.trim()
  if (!path || !value) return
  if (useLocatorForWrite.value) {
    const type = locatorType.value.trim()
    const field = locatorField.value.trim()
    const idValue = locatorValue.value.trim()
    if (!type || !field || !idValue) return
    void store.writeClrPrimitivePathByLocator(type, field, idValue, path, value)
    return
  }
  const objectAddress = store.clrSelectedObject?.address ?? ''
  if (!objectAddress) return
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
  const operations = parseBatchOperations()
  if (operations.length === 0) return
  if (useLocatorForWrite.value) {
    const type = locatorType.value.trim()
    const field = locatorField.value.trim()
    const idValue = locatorValue.value.trim()
    if (!type || !field || !idValue) return
    void store.writeClrPrimitivePathBatchByLocator(type, field, idValue, operations)
    return
  }
  const objectAddress = store.clrSelectedObject?.address ?? ''
  if (!objectAddress) return
  if (suspendDuringBatch.value) {
    void store.writeClrPrimitivePathBatchAtomic(objectAddress, operations)
    return
  }
  void store.writeClrPrimitivePathBatch(objectAddress, operations)
}

function usePathExample(path: string) {
  pathWritePath.value = path
}

function callInstanceMethod() {
  const objectAddress = store.clrSelectedObject?.address ?? ''
  const method = callMethodName.value.trim()
  const value = callMethodValue.value.trim()
  if (!objectAddress || !method) return
  // valueType volontairement vide : le type reel du parametre, resolu via
  // ClrMD (ResolveInstanceMethodAddress), pilote l'encodage cote natif.
  void store.callClrInstanceMethod(objectAddress, method, value, '')
}

function findGcRootPath() {
  const target = gcRootTargetAddress.value.trim() || store.clrSelectedObject?.address || ''
  if (!target) return
  void store.findClrGcRootPath(target)
}

function useSelectedObjectAsGcRootTarget() {
  gcRootTargetAddress.value = store.clrSelectedObject?.address ?? ''
}

function disassembleSelectedMethod() {
  const objectAddress = store.clrSelectedObject?.address ?? ''
  const method = disassembleMethodName.value.trim()
  if (!objectAddress || !method) return
  void store.disassembleClrMethod(objectAddress, method, disassembleInstructionCount.value)
}

function generateSelectedObjectReport() {
  const objectAddress = store.clrSelectedObject?.address ?? ''
  if (!objectAddress) return
  void store.generateClrObjectReport(objectAddress, reportMaxDepth.value, reportMaxNodes.value, reportIncludeGcRootChain.value)
}

// Rendu texte lisible du rapport JSON (chantier "rapport d'objet") : liste
// plate de nœuds + arêtes discoveredVia -> reconstruit une hiérarchie
// indentée en suivant les arêtes depuis la racine (profondeur = depth du
// nœud), pas une vraie sérialisation récursive de l'arbre -- plus simple et
// robuste face aux cycles/références partagées déjà dédupliquées côté helper.
const clrObjectReportText = computed(() => {
  const report = store.clrObjectReportResult
  if (!report) return ''
  if (!report.success) return `Erreur : ${report.error ?? 'inconnue'}`

  const lines: string[] = []
  lines.push(`Rapport d'objet — ${report.rootTypeName ?? '?'} @ ${report.rootAddress ?? '?'}`)
  lines.push(`Généré : ${report.generatedAtUtc ?? '?'} — ${report.nodeCount ?? 0} nœud(s), profondeur max ${report.maxDepth ?? '?'}, ${report.elapsedMs ?? 0} ms`)
  if (report.truncated) {
    const reasons = [
      report.truncatedByDepth ? 'profondeur' : null,
      report.truncatedByNodes ? 'nombre de nœuds' : null,
      report.truncatedByTime ? 'budget de temps' : null,
    ].filter(Boolean).join(', ')
    lines.push(`⚠ Rapport tronqué (limite atteinte : ${reasons}) — le graphe réel est plus grand que ce qui est affiché.`)
  }
  lines.push('')

  if (report.gcRootChain) {
    const chain = report.gcRootChain
    lines.push('Chemin depuis un GC root :')
    lines.push(
      chain.success
        ? `  ${chain.rootKind ?? '?'} → ${(chain.path ?? []).map(step => `${step.fieldName ?? (step.index !== null && step.index !== undefined ? `[${step.index}]` : step.kind)}`).join(' → ')} → (racine du rapport)${chain.shortestPathGuaranteed ? '' : ' (premier chemin trouvé, pas garanti le plus court)'}`
        : `  Introuvable : ${chain.message ?? chain.error ?? 'inconnu'}`,
    )
    lines.push('')
  }

  for (const entry of report.nodes ?? []) {
    const indent = '  '.repeat(entry.depth)
    const viaInfo = entry.discoveredVia
    let via = ' (racine)'
    if (viaInfo) {
      const label = viaInfo.fieldName ?? (viaInfo.index !== null && viaInfo.index !== undefined ? `[${viaInfo.index}]` : viaInfo.kind)
      via = ` (via ${label})`
    }
    lines.push(`${indent}• ${entry.node?.typeName ?? '?'} @ ${entry.address}${via}`)
    for (const [name, value] of Object.entries(entry.node?.fields ?? {})) {
      if (value !== null && typeof value === 'object') continue // référence/collection : déjà un nœud séparé ou dépliée dans "collection"
      lines.push(`${indent}    ${name} = ${JSON.stringify(value)}`)
    }
  }
  return lines.join('\n')
})

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
            <label class="locator-toggle">
              <input v-model="useLocatorForWrite" type="checkbox" />
              <span>Utiliser un locator au lieu d'une adresse (relocalise l'objet via type/champ/valeur juste avant d'écrire — résiste à un déplacement par GC)</span>
            </label>
            <div v-if="useLocatorForWrite" class="locator-write-inputs">
              <input v-model="locatorType" class="type-input" placeholder="Type (ex. KillEngine.ClrTestTarget.Player)" aria-label="Type CLR du locator d'écriture" />
              <input v-model="locatorField" class="locator-small-input" placeholder="Champ identité" aria-label="Champ identité du locator d'écriture" />
              <input v-model="locatorValue" class="locator-small-input" placeholder="Valeur identité" aria-label="Valeur identité du locator d'écriture" />
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
              :disabled="store.clrInspectorBusy || !pathWritePath.trim() || !pathWriteValue.trim() || (useLocatorForWrite && (!locatorType.trim() || !locatorField.trim() || !locatorValue.trim()))"
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
              <label v-if="!useLocatorForWrite" class="locator-toggle batch-suspend-toggle">
                <input v-model="suspendDuringBatch" type="checkbox" />
                <span>Suspendre le process pendant la transaction (plus sûr, plus risqué — suspend toutes les threads cible, best-effort contre un deadlock)</span>
              </label>
              <button
                class="btn btn-secondary"
                :disabled="store.clrInspectorBusy || parseBatchOperations().length === 0 || (useLocatorForWrite && (!locatorType.trim() || !locatorField.trim() || !locatorValue.trim()))"
                title="Applique les lignes dans l'ordre et tente un rollback si une opération échoue."
                @click="writeBatch"
              >
                Transaction
              </button>
            </div>
          </div>

          <div v-if="store.clrSelectedObject" class="setter-call warning-band">
            <div class="setter-call-head">
              <div>
                <strong>Appeler un setter (action avancée — injecte du code)</strong>
                <span>Contrairement aux écritures ci-dessus (mémoire passive), ceci exécute réellement le vrai setter C# dans le processus attaché.</span>
              </div>
              <InfoDot
                text="Résout l'adresse native déjà JITtée du setter via ClrMD, construit un petit shellcode x64 (this en RCX, valeur en RDX) puis l'exécute par injection dans la cible — logique métier réelle (validation, effets de bord), pas un contournement mémoire brut. Setters d'INSTANCE uniquement, 0 ou 1 paramètre primitif entier (bool/int8..int64/uint8..uint64) : pas float/double, pas string/objet/struct. Le setter doit avoir déjà été déclenché au moins une fois en jeu (JIT), sinon l'appel échoue avec un message clair."
                align="right"
              />
            </div>
            <div class="setter-call-inputs">
              <input
                v-model="callMethodName"
                class="path-input"
                placeholder="Propriété ou méthode (ex. Health, set_Health)"
                aria-label="Nom du setter CLR à appeler"
                @keyup.enter="callInstanceMethod"
              />
              <input
                v-model="callMethodValue"
                class="path-value-input"
                placeholder="Valeur (laisser vide si 0 argument)"
                aria-label="Valeur du paramètre du setter CLR"
                @keyup.enter="callInstanceMethod"
              />
              <button
                class="btn btn-danger"
                :disabled="store.clrInspectorBusy || !callMethodName.trim()"
                title="Résout l'adresse native déjà JITtée puis appelle réellement ce setter par injection shellcode."
                @click="callInstanceMethod"
              >
                Appeler
              </button>
            </div>
            <div v-if="store.clrCallMethodResult" class="setter-call-result" :class="{ ok: store.clrCallMethodResult.success, err: !store.clrCallMethodResult.success }">
              <template v-if="store.clrCallMethodResult.success">
                <strong>{{ store.clrCallMethodResult.methodName }}</strong>
                <code>{{ store.clrCallMethodResult.nativeCodeAddress }}</code>
                <span>vérifié : {{ store.clrCallMethodResult.verified ? 'oui' : 'non' }}</span>
              </template>
              <template v-else>
                {{ store.clrCallMethodResult.error }}
              </template>
            </div>
          </div>

          <div v-if="store.clrSelectedObject" class="setter-call">
            <div class="setter-call-head">
              <div>
                <strong>Désassembler ce setter (lecture seule)</strong>
                <span>Résout l'adresse native déjà JITtée puis désassemble en avant — aucune exécution, contrairement au panneau ci-dessus.</span>
              </div>
              <InfoDot
                text="Réutilise la même résolution ClrMD que l'appel de setter (resolveInstanceMethodAddress), puis lit le code natif déjà JITté et le désassemble instruction par instruction (décodeur x64 existant du module de patch). Purement en lecture — n'exécute jamais le code cible."
                align="right"
              />
            </div>
            <div class="setter-call-inputs">
              <input
                v-model="disassembleMethodName"
                class="path-input"
                placeholder="Propriété ou méthode (ex. Vitality, set_Vitality)"
                aria-label="Nom de la méthode CLR à désassembler"
                @keyup.enter="disassembleSelectedMethod"
              />
              <input
                v-model.number="disassembleInstructionCount"
                type="number"
                min="1"
                max="64"
                class="path-value-input"
                aria-label="Nombre d'instructions à désassembler"
              />
              <button
                class="btn btn-secondary"
                :disabled="store.clrInspectorBusy || !disassembleMethodName.trim()"
                title="Résout l'adresse native déjà JITtée puis désassemble en avant (lecture seule)."
                @click="disassembleSelectedMethod"
              >
                Désassembler
              </button>
            </div>
            <div v-if="store.clrDisassembleResult" class="setter-call-result" :class="{ ok: store.clrDisassembleResult.success, err: !store.clrDisassembleResult.success }">
              <template v-if="store.clrDisassembleResult.success">
                <strong>{{ store.clrDisassembleResult.methodName }}</strong>
                <code>{{ store.clrDisassembleResult.nativeCodeAddress }}</code>
                <span>{{ store.clrDisassembleResult.returnedInstructionCount }} instruction(s){{ store.clrDisassembleResult.truncated ? ' (tronqué)' : '' }}</span>
              </template>
              <template v-else>
                {{ store.clrDisassembleResult.error }}
              </template>
            </div>
            <div v-if="store.clrDisassembleResult?.instructions?.length" class="table-wrap disassembly-wrap">
              <table>
                <thead>
                  <tr>
                    <th>Adresse</th>
                    <th>Octets</th>
                    <th>Instruction</th>
                  </tr>
                </thead>
                <tbody>
                  <tr v-for="(instruction, index) in store.clrDisassembleResult.instructions" :key="`${instruction.address}-${index}`">
                    <td><code>{{ instruction.address }}</code></td>
                    <td><code>{{ instruction.rawBytesText }}</code></td>
                    <td>{{ instruction.disassembly || instruction.mnemonicHint }}</td>
                  </tr>
                </tbody>
              </table>
            </div>
          </div>

          <div v-if="store.clrSelectedObject" class="setter-call">
            <div class="setter-call-head">
              <div>
                <strong>Générer un rapport</strong>
                <span>Exporte cet objet et son graphe atteignable (champs, collections, chemin GCRoot) en un document texte — pratique pour sauvegarder ou partager sans tout re-naviguer en live.</span>
              </div>
              <InfoDot
                text="Parcours borné (profondeur/nombre de nœuds/temps) du graphe managé atteignable depuis cet objet, chaque nœud décrit comme dans le panneau de lecture ci-dessus. Un graphe large sera tronqué — augmente la profondeur/le nombre de nœuds si le rapport semble incomplet, au prix d'un appel plus lent."
                align="right"
              />
            </div>
            <div class="setter-call-inputs">
              <input
                v-model.number="reportMaxDepth"
                type="number"
                min="1"
                max="6"
                class="path-value-input"
                aria-label="Profondeur maximale du rapport"
                title="Profondeur maximale (1-6, défaut 3)"
              />
              <input
                v-model.number="reportMaxNodes"
                type="number"
                min="1"
                max="300"
                class="path-value-input"
                aria-label="Nombre maximal de nœuds du rapport"
                title="Nombre maximal de nœuds (1-300, défaut 50)"
              />
              <label class="locator-toggle">
                <input v-model="reportIncludeGcRootChain" type="checkbox" />
                Inclure le chemin GC root
              </label>
              <button
                class="btn btn-secondary"
                :disabled="store.clrInspectorBusy"
                title="Génère le rapport (lecture seule, aucune écriture)."
                @click="generateSelectedObjectReport"
              >
                Générer
              </button>
            </div>
            <div v-if="store.clrObjectReportResult" class="setter-call-result" :class="{ ok: store.clrObjectReportResult.success, err: !store.clrObjectReportResult.success }">
              <template v-if="store.clrObjectReportResult.success">
                <strong>{{ store.clrObjectReportResult.nodeCount }} nœud(s)</strong>
                <span>{{ store.clrObjectReportResult.elapsedMs }} ms{{ store.clrObjectReportResult.truncated ? ' — tronqué' : '' }}</span>
              </template>
              <template v-else>
                {{ store.clrObjectReportResult.error }}
              </template>
            </div>
            <pre v-if="clrObjectReportText" class="clr-object-report-text">{{ clrObjectReportText }}</pre>
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

        <div class="gcroot-path">
          <div class="panel-head">
            <h3>Chemin root → objet (exploratoire)</h3>
            <InfoDot
              text="Reconstruit un chemin complet root -> ... -> objet cible à travers plusieurs sauts de références (équivalent approximatif de !gcroot SOS/WinDbg). Retourne le PREMIER chemin trouvé par un parcours en largeur borné par root, pas garanti le plus court. Peut être lent sur un gros tas (budget de temps/nœuds interne côté helper)."
              align="right"
            />
          </div>
          <div class="setter-call-inputs">
            <input
              v-model="gcRootTargetAddress"
              class="path-input"
              placeholder="Adresse objet cible (0x...)"
              aria-label="Adresse de l'objet cible pour le chemin GC root"
              @keyup.enter="findGcRootPath"
            />
            <button
              class="btn btn-secondary"
              :disabled="store.clrInspectorBusy || !store.clrSelectedObject"
              title="Utilise l'adresse de l'objet actuellement sélectionné."
              @click="useSelectedObjectAsGcRootTarget"
            >
              Objet sélectionné
            </button>
            <button
              class="btn btn-secondary"
              :disabled="store.clrInspectorBusy || !(gcRootTargetAddress.trim() || store.clrSelectedObject)"
              @click="findGcRootPath"
            >
              Retrouver le chemin
            </button>
          </div>
          <div v-if="store.clrGcRootPathResult" class="setter-call-result" :class="{ ok: store.clrGcRootPathResult.success, err: !store.clrGcRootPathResult.success }">
            <template v-if="store.clrGcRootPathResult.success">
              <strong>{{ store.clrGcRootPathResult.rootKind }}</strong>
              <code>{{ store.clrGcRootPathResult.rootObjectAddress }}</code>
              <span>{{ store.clrGcRootPathResult.depth }} saut(s), {{ store.clrGcRootPathResult.nodesVisited }} nœud(s) visité(s)</span>
            </template>
            <template v-else>
              {{ store.clrGcRootPathResult.message || store.clrGcRootPathResult.error }}
            </template>
          </div>
          <ol v-if="store.clrGcRootPathResult?.path?.length" class="gcroot-steps">
            <li>
              <code>{{ store.clrGcRootPathResult.rootObjectAddress }}</code>
              <span>{{ store.clrGcRootPathResult.rootObjectTypeName }}</span>
              <em>(objet du root {{ store.clrGcRootPathResult.rootKind }})</em>
            </li>
            <li v-for="(step, index) in store.clrGcRootPathResult.path" :key="index">
              <span class="step-hop">
                {{ step.kind === 'index' ? `[${step.index}]` : `.${step.fieldName}` }}
              </span>
              →
              <code>{{ step.objectAddress }}</code>
              <span>{{ step.typeName }}</span>
            </li>
          </ol>
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

.locator-toggle {
  grid-column: 1 / -1;
  display: flex;
  align-items: center;
  gap: 8px;
  color: var(--text-secondary);
  font-size: 12px;
}

.locator-write-inputs {
  grid-column: 1 / -1;
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}

.batch-write {
  grid-column: 1 / -1;
  display: grid;
  grid-template-columns: minmax(240px, 1fr) auto;
  gap: 8px;
  align-items: end;
}

.batch-suspend-toggle {
  grid-column: 1 / -1;
}

.warning-band {
  border: 1px solid rgba(245, 158, 11, 0.45);
  background: rgba(245, 158, 11, 0.12);
  border-radius: 8px;
}

.setter-call {
  grid-column: 1 / -1;
  padding: 12px 14px;
  margin: 4px 14px 12px;
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.setter-call-head {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 12px;
}

.setter-call-head strong {
  display: block;
  color: var(--text-primary);
  font-size: 13px;
}

.setter-call-head span {
  display: block;
  color: var(--text-dim);
  font-size: 12px;
  margin-top: 2px;
  max-width: 480px;
}

.setter-call-inputs {
  display: grid;
  grid-template-columns: minmax(180px, 1fr) minmax(140px, 1fr) auto;
  gap: 8px;
}

.setter-call-result {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
  font-size: 12px;
}

.setter-call-result.ok {
  color: var(--success);
}

.setter-call-result.err {
  color: var(--error);
}

.clr-object-report-text {
  margin: 8px 0 0;
  padding: 10px 12px;
  max-height: 360px;
  overflow: auto;
  font-family: var(--font-mono, monospace);
  font-size: 11px;
  line-height: 1.5;
  white-space: pre;
  background: var(--surface-2, rgba(127, 127, 127, 0.08));
  border: 1px solid var(--border);
  border-radius: 6px;
}

.path-examples {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  justify-content: flex-end;
}

.disassembly-wrap {
  max-height: 260px;
  overflow-y: auto;
}

.gcroot-path {
  margin: 14px 0 4px;
  padding-top: 12px;
  border-top: 1px solid var(--border-color, rgba(255, 255, 255, 0.08));
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.gcroot-steps {
  display: flex;
  flex-direction: column;
  gap: 6px;
  margin: 0;
  padding-left: 20px;
  font-size: 12px;
  color: var(--text-dim);
}

.gcroot-steps li {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}

.step-hop {
  color: var(--text-primary);
  font-family: monospace;
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
  .setter-call-inputs,
  .guide-steps,
  .status-band {
    grid-template-columns: 1fr;
  }

  .setter-call-head {
    flex-direction: column;
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
