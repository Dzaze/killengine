<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { useAppStore } from '@/stores/app'
import InfoDot from '@/components/expert/InfoDot.vue'
import PanelIntro from '@/components/common/PanelIntro.vue'
import type { ClrFieldInfo, ClrPathWriteOperation } from '@/services/backend'

const store = useAppStore()
const { t } = useI18n()
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
    label: t('clrInspector.guide.step1Label'),
    text: store.isAttached ? store.processName : t('clrInspector.guide.chooseTarget'),
    done: store.isAttached,
    active: !store.isAttached,
  },
  {
    label: t('clrInspector.guide.step2Label'),
    text: clrReady.value ? t('clrInspector.guide.sessionAttached') : t('clrInspector.guide.attachHelper'),
    done: clrReady.value,
    active: store.isAttached && !clrReady.value,
  },
  {
    label: t('clrInspector.guide.step3Label'),
    text: store.clrObjects.length > 0 ? t('clrInspector.guide.objectCount', { count: store.clrObjects.length }) : t('clrInspector.guide.listHeap'),
    done: store.clrObjects.length > 0,
    active: clrReady.value && store.clrObjects.length === 0,
  },
  {
    label: t('clrInspector.guide.step4Label'),
    text: store.clrSelectedObject ? t('clrInspector.guide.readLocatorWrite') : t('clrInspector.guide.selectObject'),
    done: Boolean(store.clrSelectedObject),
    active: store.clrObjects.length > 0 && !store.clrSelectedObject,
  },
])

const selectedTypeLabel = computed(() => store.clrSelectedObject?.typeName ?? t('clrInspector.defaultTypeLabel'))
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
  if (!report.success) return t('clrInspector.report.error', { error: report.error ?? t('clrInspector.report.unknownError') })

  const lines: string[] = []
  lines.push(t('clrInspector.report.reportHeader', { type: report.rootTypeName ?? '?', address: report.rootAddress ?? '?' }))
  lines.push(t('clrInspector.report.generatedLine', { date: report.generatedAtUtc ?? '?', nodeCount: report.nodeCount ?? 0, maxDepth: report.maxDepth ?? '?', ms: report.elapsedMs ?? 0 }))
  if (report.truncated) {
    const reasons = [
      report.truncatedByDepth ? t('clrInspector.report.truncatedByDepth') : null,
      report.truncatedByNodes ? t('clrInspector.report.truncatedByNodes') : null,
      report.truncatedByTime ? t('clrInspector.report.truncatedByTime') : null,
    ].filter(Boolean).join(', ')
    lines.push(t('clrInspector.report.truncatedWarning', { reasons }))
  }
  lines.push('')

  if (report.gcRootChain) {
    const chain = report.gcRootChain
    lines.push(t('clrInspector.report.gcRootChainHeader'))
    lines.push(
      chain.success
        ? t('clrInspector.report.gcRootChainLine', {
          rootKind: chain.rootKind ?? '?',
          path: (chain.path ?? []).map(step => `${step.fieldName ?? (step.index !== null && step.index !== undefined ? `[${step.index}]` : step.kind)}`).join(' → '),
          shortestGuarantee: chain.shortestPathGuaranteed ? '' : t('clrInspector.report.notShortestGuaranteed'),
        })
        : t('clrInspector.report.gcRootChainNotFound', { message: chain.message ?? chain.error ?? t('clrInspector.report.unknownValue') }),
    )
    lines.push('')
  }

  for (const entry of report.nodes ?? []) {
    const indent = '  '.repeat(entry.depth)
    const viaInfo = entry.discoveredVia
    let via = t('clrInspector.report.rootSuffix')
    if (viaInfo) {
      const label = viaInfo.fieldName ?? (viaInfo.index !== null && viaInfo.index !== undefined ? `[${viaInfo.index}]` : viaInfo.kind)
      via = t('clrInspector.report.viaSuffix', { label })
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
  store.createTrainerClrFieldFeature({
    name: `CLR ${field.name}`,
    typeSubstring: locatorType.value,
    identityField: locatorField.value,
    identityValue: locatorValue.value,
    targetField: field.name,
    valueType: trainerValueType(field),
    value,
    address: selected.address,
  })
}

function canUseAsLocator(field: ClrFieldInfo): boolean {
  return field.kind === 'primitive' || field.kind === 'string' || typeof field.value === 'string' || typeof field.value === 'number' || typeof field.value === 'boolean'
}

function useFieldAsLocator(field: ClrFieldInfo) {
  if (!store.clrSelectedObject || !canUseAsLocator(field)) return
  locatorType.value = store.clrSelectedObject.typeName
  locatorField.value = field.name
  locatorValue.value = String(field.value ?? '')
  locatorWizardStep.value = 3
}

function useDefaultClrTarget() {
  locatorType.value = 'KillEngine.ClrTestTarget.Player'
  locatorField.value = 'Name'
  locatorValue.value = 'TestSubject'
  store.clrTypeFilter = 'KillEngine.ClrTestTarget'
  locatorWizardStep.value = 3
}

function runFieldLocator() {
  void store.findClrObjectsByFieldValue(locatorType.value, locatorField.value, locatorValue.value, locatorMaxResults.value)
}

// Flux guidé du locator stable (chantier UX novice) : remplace les 3 champs
// texte à la fois par un parcours en 3 étapes qui ne fait jamais taper un nom
// de type/champ à l'aveugle -- étape 1 choisit un objet réel dans la liste
// déjà chargée (réutilise readClrObject), étape 2 propose les champs de cet
// objet en boutons (réutilise useFieldAsLocator ci-dessus), étape 3 confirme
// et lance la recherche. Le mode avancé (texte libre) reste disponible via
// `locatorWizardAdvanced` pour un utilisateur qui connaît déjà ses valeurs.
const locatorWizardAdvanced = ref(false)
const locatorWizardStep = ref<1 | 2 | 3>(1)

const wizardLocatorCandidateFields = computed<ClrFieldInfo[]>(() => objectFields.value.filter(canUseAsLocator))

async function chooseWizardObject(address: string) {
  await store.readClrObject(address)
  if (store.clrSelectedObject) locatorWizardStep.value = 2
}

function restartLocatorWizard() {
  locatorWizardStep.value = 1
}

onMounted(() => {
  void store.refreshClrInspectorStatus()
})
</script>

<template>
  <div class="clr-view">
    <div class="header">
      <div>
        <h1>{{ $t('clrInspector.title') }}</h1>
        <p>{{ store.isAttached ? store.processName : $t('clrInspector.noProcess') }}</p>
      </div>
      <div class="header-actions">
        <button class="btn btn-secondary" :disabled="store.clrInspectorBusy" @click="store.refreshClrInspectorStatus()">
          {{ $t('clrInspector.status') }}
        </button>
        <button class="btn btn-secondary" :disabled="store.clrInspectorBusy" @click="store.shutdownClrInspector()">
          {{ $t('clrInspector.stopHelper') }}
        </button>
      </div>
    </div>

    <PanelIntro
      :what="$t('clrInspector.intro.what')"
      :purpose="$t('clrInspector.intro.purpose')"
      :how="$t('clrInspector.intro.how')"
    />

    <div v-if="!store.isAttached" class="empty-state">
      <strong>{{ $t('clrInspector.noActiveTarget') }}</strong>
      <p>{{ $t('clrInspector.noActiveTargetBody') }}</p>
      <button class="btn btn-primary" :aria-label="$t('clrInspector.goToProcessAriaLabel')" @click="store.activeView = 'process'">
        {{ $t('clrInspector.goToProcess') }}
      </button>
    </div>

    <template v-else>
      <section class="novice-guide" :aria-label="$t('clrInspector.guide.ariaLabel')">
        <div class="guide-head">
          <div>
            <span>{{ $t('clrInspector.guide.label') }}</span>
            <strong>{{ clrReady ? $t('clrInspector.guide.ready') : $t('clrInspector.guide.start') }}</strong>
          </div>
          <InfoDot topic="clrGuide" align="right" />
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
          <span>{{ $t('clrInspector.statusBand.helper') }}</span>
          <strong :class="{ ok: store.clrInspectorStatus?.running, warn: !store.clrInspectorStatus?.available }">
            {{ store.clrInspectorStatus?.running ? $t('clrInspector.statusBand.active') : (store.clrInspectorStatus?.available ? $t('clrInspector.statusBand.ready') : $t('clrInspector.statusBand.notFound')) }}
          </strong>
        </div>
        <div>
          <span>{{ $t('clrInspector.statusBand.pid') }}</span>
          <strong>{{ store.clrInspectorStatus?.pid || '-' }}</strong>
        </div>
        <div>
          <span>{{ $t('clrInspector.statusBand.pipe') }}</span>
          <code>{{ store.clrInspectorStatus?.pipeName || '-' }}</code>
        </div>
      </section>

      <section class="toolbar">
        <label class="field-label">
          <span>{{ $t('clrInspector.toolbar.typeFilterLabel') }}<InfoDot topic="clrAttach" /></span>
          <input v-model="store.clrTypeFilter" class="type-input" :placeholder="$t('clrInspector.toolbar.typeFilterPlaceholder')" />
        </label>
        <button
          class="btn btn-primary"
          :disabled="store.clrInspectorBusy"
          :title="$t('clrInspector.toolbar.attachTitle')"
          @click="store.attachClrInspector()"
        >
          {{ $t('clrInspector.toolbar.attach') }}
        </button>
        <button
          class="btn btn-secondary"
          :disabled="store.clrInspectorBusy || !clrReady"
          :title="$t('clrInspector.toolbar.objectsTitle')"
          @click="store.findClrObjects()"
        >
          {{ $t('clrInspector.toolbar.objects') }}
        </button>
        <button
          class="btn btn-secondary"
          :disabled="store.clrInspectorBusy || !clrReady"
          :title="$t('clrInspector.toolbar.rootsTitle')"
          @click="store.enumerateClrRoots()"
        >
          {{ $t('clrInspector.toolbar.roots') }}
        </button>
        <button
          class="btn btn-secondary"
          :disabled="store.clrInspectorBusy || !clrReady"
          :title="$t('clrInspector.toolbar.flushTitle')"
          @click="store.flushClrInspectorCache()"
        >
          {{ $t('clrInspector.toolbar.flush') }}
        </button>
        <button class="btn btn-secondary" :disabled="store.clrInspectorBusy" @click="store.detachClrInspector()">
          {{ $t('clrInspector.toolbar.detach') }}
        </button>
      </section>

      <div v-if="store.clrInspectorError" class="alert error">{{ store.clrInspectorError }}</div>
      <div v-else-if="store.clrLastResult && !store.clrLastResult.success" class="alert error">
        {{ store.clrLastResult.error }}
      </div>

      <div class="manual-read">
        <label class="field-label">
          <span>{{ $t('clrInspector.manualRead.label') }}<InfoDot topic="clrManualRead" /></span>
          <input v-model="manualAddress" class="type-input" :placeholder="$t('clrInspector.manualRead.placeholder')" @keyup.enter="readManualObject" />
        </label>
        <button
          class="btn btn-secondary"
          :disabled="store.clrInspectorBusy || !manualAddress.trim()"
          :aria-label="$t('clrInspector.manualRead.ariaLabel')"
          @click="readManualObject"
        >
          {{ $t('clrInspector.manualRead.button') }}
        </button>
      </div>

      <section class="locator-panel">
        <div class="locator-head">
          <div>
            <span>{{ $t('clrInspector.locator.label') }}</span>
            <strong>{{ $t('clrInspector.locator.title') }}</strong>
          </div>
          <div class="locator-head-actions">
            <InfoDot topic="clrLocator" align="right" />
            <button class="mini-btn" type="button" @click="useDefaultClrTarget">{{ $t('clrInspector.locator.exampleTarget') }}</button>
            <label class="wizard-mode-toggle">
              <input v-model="locatorWizardAdvanced" type="checkbox" />
              <span>{{ $t('clrInspector.locator.advancedMode') }}</span>
            </label>
          </div>
        </div>

        <template v-if="!locatorWizardAdvanced">
          <ol class="wizard-steps">
            <li :class="{ done: locatorWizardStep > 1, active: locatorWizardStep === 1 }">
              <span>{{ $t('clrInspector.locator.step1') }}</span>
              <strong>{{ store.clrSelectedObject ? store.clrSelectedObject.typeName : $t('clrInspector.locator.toChoose') }}</strong>
            </li>
            <li :class="{ done: locatorWizardStep > 2, active: locatorWizardStep === 2 }">
              <span>{{ $t('clrInspector.locator.step2') }}</span>
              <strong>{{ locatorWizardStep > 1 && locatorField ? locatorField : $t('clrInspector.locator.toChoose') }}</strong>
            </li>
            <li :class="{ active: locatorWizardStep === 3 }">
              <span>{{ $t('clrInspector.locator.step3') }}</span>
              <strong>{{ locatorWizardStep === 3 ? $t('clrInspector.locator.ready') : $t('clrInspector.locator.waiting') }}</strong>
            </li>
          </ol>

          <div v-if="locatorWizardStep === 1" class="wizard-step">
            <p class="wizard-hint">
              {{ $t('clrInspector.locator.step1Hint') }}
            </p>
            <div v-if="store.clrObjects.length" class="table-wrap wizard-object-list">
              <table>
                <thead>
                  <tr>
                    <th>{{ $t('clrInspector.locator.addressColumn') }}</th>
                    <th>{{ $t('clrInspector.locator.typeColumn') }}</th>
                    <th></th>
                  </tr>
                </thead>
                <tbody>
                  <tr v-for="obj in store.clrObjects" :key="obj.address">
                    <td><code>{{ obj.address }}</code></td>
                    <td>{{ obj.typeName }}</td>
                    <td>
                      <button class="mini-btn" :disabled="store.clrInspectorBusy" @click="chooseWizardObject(obj.address)">
                        {{ $t('clrInspector.locator.choose') }}
                      </button>
                    </td>
                  </tr>
                </tbody>
              </table>
            </div>
            <p v-else class="panel-hint">{{ $t('clrInspector.locator.noObjectListed') }}</p>
          </div>

          <div v-else-if="locatorWizardStep === 2" class="wizard-step">
            <p class="wizard-hint">
              {{ $t('clrInspector.locator.step2Hint', { type: store.clrSelectedObject?.typeName, address: store.clrSelectedObject?.address }) }}
            </p>
            <div v-if="wizardLocatorCandidateFields.length" class="wizard-field-chips">
              <button
                v-for="field in wizardLocatorCandidateFields"
                :key="field.name"
                class="mini-btn"
                type="button"
                @click="useFieldAsLocator(field)"
              >
                {{ field.name }} = {{ valueText(field.value) }}
              </button>
            </div>
            <p v-else class="panel-hint">
              {{ $t('clrInspector.locator.noSimpleField') }}
            </p>
            <button class="mini-btn" type="button" @click="restartLocatorWizard">{{ $t('clrInspector.locator.changeObject') }}</button>
          </div>

          <div v-else class="wizard-step">
            <p class="wizard-hint">{{ $t('clrInspector.locator.step3Ready') }}</p>
            <div class="wizard-summary">
              <div class="wizard-summary-item">
                <span>{{ $t('clrInspector.locator.typeLabel') }}</span>
                <strong>{{ locatorType }}</strong>
              </div>
              <div class="wizard-summary-item">
                <span>{{ $t('clrInspector.locator.fieldLabel') }}</span>
                <strong>{{ locatorField }}</strong>
              </div>
              <label class="field-label compact-label">
                <span>{{ $t('clrInspector.locator.valueLabel') }}</span>
                <input v-model="locatorValue" class="locator-small-input" @keyup.enter="runFieldLocator" />
              </label>
              <label class="field-label count-label">
                <span>{{ $t('clrInspector.locator.maxLabel') }}</span>
                <input v-model.number="locatorMaxResults" class="locator-count-input" type="number" min="1" max="200" />
              </label>
            </div>
            <div class="wizard-step-actions">
              <button class="mini-btn" type="button" @click="locatorWizardStep = 2">{{ $t('clrInspector.locator.changeField') }}</button>
              <button
                class="btn btn-secondary"
                :disabled="store.clrInspectorBusy || !locatorType.trim() || !locatorField.trim() || !locatorValue.trim()"
                :aria-label="$t('clrInspector.locator.findAriaLabel')"
                @click="runFieldLocator"
              >
                {{ $t('clrInspector.locator.find') }}
              </button>
            </div>
          </div>
        </template>

        <div v-else class="locator-inputs">
          <label class="field-label">
            <span>{{ $t('clrInspector.locator.typeLabel') }}</span>
            <input v-model="locatorType" class="type-input" :placeholder="$t('clrInspector.locator.typePlaceholderAdvanced')" />
          </label>
          <label class="field-label compact-label">
            <span>{{ $t('clrInspector.readObject.locatorFieldPlaceholder') }}</span>
            <input v-model="locatorField" class="locator-small-input" :placeholder="$t('clrInspector.locator.identityFieldPlaceholder')" />
          </label>
          <label class="field-label compact-label">
            <span>{{ $t('clrInspector.locator.valueLabel') }}</span>
            <input v-model="locatorValue" class="locator-small-input" :placeholder="$t('clrInspector.locator.valuePlaceholderAdvanced')" @keyup.enter="runFieldLocator" />
          </label>
          <label class="field-label count-label">
            <span>{{ $t('clrInspector.locator.maxLabel') }}</span>
            <input v-model.number="locatorMaxResults" class="locator-count-input" type="number" min="1" max="200" />
          </label>
          <button
            class="btn btn-secondary"
            :disabled="store.clrInspectorBusy || !locatorType.trim() || !locatorField.trim() || !locatorValue.trim()"
            :aria-label="$t('clrInspector.locator.findAriaLabel')"
            @click="runFieldLocator"
          >
            {{ $t('clrInspector.locator.find') }}
          </button>
        </div>
        <div v-if="store.clrFieldLocatorResult" class="locator-results">
          <span>
            {{ $t('clrInspector.locator.resultsSummary', { returned: store.clrFieldLocatorResult.matchesReturned, typeMatches: store.clrFieldLocatorResult.typeMatches, scanned: store.clrFieldLocatorResult.scannedObjects }) }}
          </span>
          <button
            v-for="match in store.clrFieldLocatorResult.matches"
            :key="match.address"
            class="locator-match"
            :disabled="store.clrInspectorBusy"
            :aria-label="$t('clrInspector.locator.readObjectAriaLabel', { type: match.typeName, address: match.address })"
            @click="store.readClrObject(match.address)"
          >
            {{ match.typeName }} @ {{ match.address }}
          </button>
        </div>
      </section>

      <div class="content-grid">
        <section class="panel">
          <div class="panel-head">
            <h2>{{ $t('clrInspector.objectsPanel.title') }}</h2>
            <span>{{ store.clrObjects.length }}</span>
          </div>
          <div v-if="clrReady && store.clrObjects.length === 0" class="panel-hint">
            {{ $t('clrInspector.objectsPanel.hint') }}
          </div>
          <div class="table-wrap">
            <table>
              <thead>
                <tr>
                  <th>{{ $t('clrInspector.objectsPanel.addressColumn') }}</th>
                  <th>{{ $t('clrInspector.objectsPanel.typeColumn') }}</th>
                  <th>{{ $t('clrInspector.objectsPanel.sizeColumn') }}</th>
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
                      {{ $t('clrInspector.objectsPanel.read') }}
                    </button>
                  </td>
                </tr>
                <tr v-if="store.clrObjects.length === 0">
                  <td colspan="4" class="empty-row">{{ $t('clrInspector.objectsPanel.noObjectListed') }}</td>
                </tr>
              </tbody>
            </table>
          </div>
        </section>

        <section class="panel">
          <div class="panel-head">
            <h2>{{ $t('clrInspector.readObject.title') }}</h2>
            <div class="panel-head-actions">
              <span>{{ $t('clrInspector.readObject.fieldCount', { count: objectFields.length }) }}</span>
              <InfoDot topic="clrFieldTable" align="right" />
            </div>
          </div>
          <div v-if="store.clrSelectedObject" class="object-summary">
            <code>{{ store.clrSelectedObject.address }}</code>
            <strong>{{ store.clrSelectedObject.typeName }}</strong>
          </div>
          <div v-else class="panel-hint">
            {{ $t('clrInspector.readObject.hintSelect') }}
          </div>
          <div v-if="store.clrSelectedObject" class="path-write">
            <div class="path-write-head">
              <span>{{ $t('clrInspector.readObject.pathWriteLabel', { type: selectedTypeLabel }) }}<InfoDot topic="clrPathWrite" align="right" /></span>
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
              <span>{{ $t('clrInspector.readObject.useLocatorInstead') }}</span>
            </label>
            <div v-if="useLocatorForWrite" class="locator-write-inputs">
              <input v-model="locatorType" class="type-input" :placeholder="$t('clrInspector.readObject.locatorTypePlaceholder')" :aria-label="$t('clrInspector.readObject.locatorTypeAriaLabel')" />
              <input v-model="locatorField" class="locator-small-input" :placeholder="$t('clrInspector.readObject.locatorFieldPlaceholder')" :aria-label="$t('clrInspector.readObject.locatorFieldAriaLabel')" />
              <input v-model="locatorValue" class="locator-small-input" :placeholder="$t('clrInspector.readObject.locatorValuePlaceholder')" :aria-label="$t('clrInspector.readObject.locatorValueAriaLabel')" />
            </div>
            <input
              v-model="pathWritePath"
              class="path-input"
              :placeholder="$t('clrInspector.readObject.pathPlaceholder')"
              :aria-label="$t('clrInspector.readObject.pathAriaLabel')"
              @keyup.enter="writePath"
            />
            <input
              v-model="pathWriteValue"
              class="path-value-input"
              :placeholder="$t('clrInspector.readObject.valuePlaceholder')"
              :aria-label="$t('clrInspector.readObject.valueAriaLabel')"
              @keyup.enter="writePath"
            />
            <button
              class="btn btn-secondary"
              :disabled="store.clrInspectorBusy || !pathWritePath.trim() || !pathWriteValue.trim() || (useLocatorForWrite && (!locatorType.trim() || !locatorField.trim() || !locatorValue.trim()))"
              :title="$t('clrInspector.readObject.writePathTitle')"
              @click="writePath"
            >
              {{ $t('clrInspector.readObject.writePath') }}
            </button>
            <div class="batch-write">
              <label class="field-label">
                <span>{{ $t('clrInspector.readObject.batchLabel') }}<InfoDot topic="clrBatchWrite" align="right" /></span>
                <textarea
                  v-model="batchWriteText"
                  class="batch-input"
                  rows="3"
                  placeholder="Health=100&#10;Stats.Rank=7&#10;Inventory.Currencies[gold]=4125"
                  :aria-label="$t('clrInspector.readObject.batchAriaLabel')"
                ></textarea>
              </label>
              <label v-if="!useLocatorForWrite" class="locator-toggle batch-suspend-toggle">
                <input v-model="suspendDuringBatch" type="checkbox" />
                <span>{{ $t('clrInspector.readObject.suspendDuringBatch') }}</span>
              </label>
              <button
                class="btn btn-secondary"
                :disabled="store.clrInspectorBusy || parseBatchOperations().length === 0 || (useLocatorForWrite && (!locatorType.trim() || !locatorField.trim() || !locatorValue.trim()))"
                :title="$t('clrInspector.readObject.batchApplyTitle')"
                @click="writeBatch"
              >
                {{ $t('clrInspector.readObject.batchApply') }}
              </button>
            </div>
          </div>

          <div v-if="store.clrSelectedObject" class="setter-call warning-band">
            <div class="setter-call-head">
              <div>
                <strong>{{ $t('clrInspector.setter.callTitle') }}</strong>
                <span>{{ $t('clrInspector.setter.callDescription') }}</span>
              </div>
              <InfoDot topic="clrCallSetter" align="right" />
            </div>
            <div class="setter-call-inputs">
              <input
                v-model="callMethodName"
                class="path-input"
                :placeholder="$t('clrInspector.setter.namePlaceholder')"
                :aria-label="$t('clrInspector.setter.nameAriaLabel')"
                @keyup.enter="callInstanceMethod"
              />
              <input
                v-model="callMethodValue"
                class="path-value-input"
                :placeholder="$t('clrInspector.setter.valuePlaceholder')"
                :aria-label="$t('clrInspector.setter.valueAriaLabel')"
                @keyup.enter="callInstanceMethod"
              />
              <button
                class="btn btn-danger"
                :disabled="store.clrInspectorBusy || !callMethodName.trim()"
                :title="$t('clrInspector.setter.callButtonTitle')"
                @click="callInstanceMethod"
              >
                {{ $t('clrInspector.setter.call') }}
              </button>
            </div>
            <div v-if="store.clrCallMethodResult" class="setter-call-result" :class="{ ok: store.clrCallMethodResult.success, err: !store.clrCallMethodResult.success }">
              <template v-if="store.clrCallMethodResult.success">
                <strong>{{ store.clrCallMethodResult.methodName }}</strong>
                <code>{{ store.clrCallMethodResult.nativeCodeAddress }}</code>
                <span v-if="store.clrCallMethodResult.parameterIsReferenceType">{{ $t('clrInspector.setter.referenceParameter') }}</span>
                <span>{{ $t('clrInspector.setter.verified', { value: store.clrCallMethodResult.verified ? $t('clrInspector.setter.yes') : $t('clrInspector.setter.no') }) }}</span>
              </template>
              <template v-else>
                {{ store.clrCallMethodResult.error }}
              </template>
            </div>
          </div>

          <div v-if="store.clrSelectedObject" class="setter-call">
            <div class="setter-call-head">
              <div>
                <strong>{{ $t('clrInspector.disassemble.title') }}</strong>
                <span>{{ $t('clrInspector.disassemble.description') }}</span>
              </div>
              <InfoDot topic="clrDisassemble" align="right" />
            </div>
            <div class="setter-call-inputs">
              <input
                v-model="disassembleMethodName"
                class="path-input"
                :placeholder="$t('clrInspector.disassemble.namePlaceholder')"
                :aria-label="$t('clrInspector.disassemble.nameAriaLabel')"
                @keyup.enter="disassembleSelectedMethod"
              />
              <input
                v-model.number="disassembleInstructionCount"
                type="number"
                min="1"
                max="64"
                class="path-value-input"
                :aria-label="$t('clrInspector.disassemble.countAriaLabel')"
              />
              <button
                class="btn btn-secondary"
                :disabled="store.clrInspectorBusy || !disassembleMethodName.trim()"
                :title="$t('clrInspector.disassemble.buttonTitle')"
                @click="disassembleSelectedMethod"
              >
                {{ $t('clrInspector.disassemble.button') }}
              </button>
            </div>
            <div v-if="store.clrDisassembleResult" class="setter-call-result" :class="{ ok: store.clrDisassembleResult.success, err: !store.clrDisassembleResult.success }">
              <template v-if="store.clrDisassembleResult.success">
                <strong>{{ store.clrDisassembleResult.methodName }}</strong>
                <code>{{ store.clrDisassembleResult.nativeCodeAddress }}</code>
                <span>{{ $t('clrInspector.disassemble.instructionCount', { count: store.clrDisassembleResult.returnedInstructionCount, truncated: store.clrDisassembleResult.truncated ? $t('clrInspector.disassemble.truncatedSuffix') : '' }) }}</span>
              </template>
              <template v-else>
                {{ store.clrDisassembleResult.error }}
              </template>
            </div>
            <div v-if="store.clrDisassembleResult?.instructions?.length" class="table-wrap disassembly-wrap">
              <table>
                <thead>
                  <tr>
                    <th>{{ $t('clrInspector.disassemble.addressColumn') }}</th>
                    <th>{{ $t('clrInspector.disassemble.bytesColumn') }}</th>
                    <th>{{ $t('clrInspector.disassemble.instructionColumn') }}</th>
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
                <strong>{{ $t('clrInspector.report.title') }}</strong>
                <span>{{ $t('clrInspector.report.description') }}</span>
              </div>
              <InfoDot topic="clrReport" align="right" />
            </div>
            <div class="setter-call-inputs">
              <input
                v-model.number="reportMaxDepth"
                type="number"
                min="1"
                max="6"
                class="path-value-input"
                :aria-label="$t('clrInspector.report.maxDepthAriaLabel')"
                :title="$t('clrInspector.report.maxDepthTitle')"
              />
              <input
                v-model.number="reportMaxNodes"
                type="number"
                min="1"
                max="300"
                class="path-value-input"
                :aria-label="$t('clrInspector.report.maxNodesAriaLabel')"
                :title="$t('clrInspector.report.maxNodesTitle')"
              />
              <label class="locator-toggle">
                <input v-model="reportIncludeGcRootChain" type="checkbox" />
                {{ $t('clrInspector.report.includeGcRootChain') }}
              </label>
              <button
                class="btn btn-secondary"
                :disabled="store.clrInspectorBusy"
                :title="$t('clrInspector.report.generateButtonTitle')"
                @click="generateSelectedObjectReport"
              >
                {{ $t('clrInspector.report.generate') }}
              </button>
            </div>
            <div v-if="store.clrObjectReportResult" class="setter-call-result" :class="{ ok: store.clrObjectReportResult.success, err: !store.clrObjectReportResult.success }">
              <template v-if="store.clrObjectReportResult.success">
                <strong>{{ $t('clrInspector.report.nodeCount', { count: store.clrObjectReportResult.nodeCount }) }}</strong>
                <span>{{ $t('clrInspector.report.elapsed', { ms: store.clrObjectReportResult.elapsedMs, truncated: store.clrObjectReportResult.truncated ? $t('clrInspector.report.truncatedSuffix') : '' }) }}</span>
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
                  <th>{{ $t('clrInspector.fieldTable.fieldColumn') }}</th>
                  <th>{{ $t('clrInspector.fieldTable.kindColumn') }}</th>
                  <th>{{ $t('clrInspector.fieldTable.addressColumn') }}</th>
                  <th>{{ $t('clrInspector.fieldTable.valueColumn') }}</th>
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
                      {{ $t('clrInspector.fieldTable.read') }}
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
                        :aria-label="$t('clrInspector.fieldTable.writeAriaLabel', { name: field.name })"
                        @click="writeField(field)"
                      >
                        {{ $t('clrInspector.fieldTable.write') }}
                      </button>
                      <button
                        class="mini-btn"
                        :disabled="store.clrInspectorBusy || !locatorType.trim() || !locatorField.trim() || !locatorValue.trim()"
                        :aria-label="$t('clrInspector.fieldTable.createTrainerAriaLabel', { name: field.name })"
                        @click="createTrainerFeature(field)"
                      >
                        {{ $t('clrInspector.fieldTable.trainer') }}
                      </button>
                    </template>
                    <button
                      v-if="canUseAsLocator(field)"
                      class="mini-btn"
                      :disabled="store.clrInspectorBusy"
                      :aria-label="$t('clrInspector.fieldTable.useAsLocatorAriaLabel', { name: field.name })"
                      @click="useFieldAsLocator(field)"
                    >
                      {{ $t('clrInspector.fieldTable.locator') }}
                    </button>
                  </td>
                </tr>
                <tr v-if="objectFields.length === 0">
                  <td colspan="5" class="empty-row">{{ $t('clrInspector.fieldTable.noObjectSelected') }}</td>
                </tr>
              </tbody>
            </table>
          </div>
        </section>
      </div>

      <section class="panel roots-panel">
        <div class="panel-head">
          <h2>{{ $t('clrInspector.gcRoots.title') }}</h2>
          <div class="panel-head-actions">
            <span>{{ store.clrRoots.length }}</span>
            <InfoDot topic="clrRootsTable" align="right" />
          </div>
        </div>
        <div class="table-wrap">
          <table>
            <thead>
              <tr>
                <th>{{ $t('clrInspector.gcRoots.rootColumn') }}</th>
                <th>{{ $t('clrInspector.gcRoots.kindColumn') }}</th>
                <th>{{ $t('clrInspector.gcRoots.objectColumn') }}</th>
                <th>{{ $t('clrInspector.gcRoots.objectTypeColumn') }}</th>
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
                <td colspan="4" class="empty-row">{{ $t('clrInspector.gcRoots.noRootLoaded') }}</td>
              </tr>
            </tbody>
          </table>
        </div>

        <div class="gcroot-path">
          <div class="panel-head">
            <h3>{{ $t('clrInspector.gcRoots.pathTitle') }}</h3>
            <InfoDot topic="clrGcRootPath" align="right" />
          </div>
          <div class="setter-call-inputs">
            <input
              v-model="gcRootTargetAddress"
              class="path-input"
              :placeholder="$t('clrInspector.gcRoots.targetAddressPlaceholder')"
              :aria-label="$t('clrInspector.gcRoots.targetAddressAriaLabel')"
              @keyup.enter="findGcRootPath"
            />
            <button
              class="btn btn-secondary"
              :disabled="store.clrInspectorBusy || !store.clrSelectedObject"
              :title="$t('clrInspector.gcRoots.useSelectedObjectTitle')"
              @click="useSelectedObjectAsGcRootTarget"
            >
              {{ $t('clrInspector.gcRoots.useSelectedObject') }}
            </button>
            <button
              class="btn btn-secondary"
              :disabled="store.clrInspectorBusy || !(gcRootTargetAddress.trim() || store.clrSelectedObject)"
              @click="findGcRootPath"
            >
              {{ $t('clrInspector.gcRoots.findPath') }}
            </button>
          </div>
          <div v-if="store.clrGcRootPathResult" class="setter-call-result" :class="{ ok: store.clrGcRootPathResult.success, err: !store.clrGcRootPathResult.success }">
            <template v-if="store.clrGcRootPathResult.success">
              <strong>{{ store.clrGcRootPathResult.rootKind }}</strong>
              <code>{{ store.clrGcRootPathResult.rootObjectAddress }}</code>
              <span>{{ $t('clrInspector.gcRoots.hops', { depth: store.clrGcRootPathResult.depth, nodesVisited: store.clrGcRootPathResult.nodesVisited }) }}</span>
            </template>
            <template v-else>
              {{ store.clrGcRootPathResult.message || store.clrGcRootPathResult.error }}
            </template>
          </div>
          <ol v-if="store.clrGcRootPathResult?.path?.length" class="gcroot-steps">
            <li>
              <code>{{ store.clrGcRootPathResult.rootObjectAddress }}</code>
              <span>{{ store.clrGcRootPathResult.rootObjectTypeName }}</span>
              <em>{{ $t('clrInspector.gcRoots.rootObjectSuffix', { kind: store.clrGcRootPathResult.rootKind }) }}</em>
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
  color: var(--text-muted);
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
  color: var(--text-muted);
  font-size: 12px;
  margin-bottom: 4px;
}

.path-write-head span :deep(.info-dot),
.field-label span :deep(.info-dot) {
  margin-left: 6px;
  vertical-align: middle;
}

.locator-head-actions,
.panel-head-actions {
  display: flex;
  align-items: center;
  gap: 8px;
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
  color: var(--text-muted);
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
  color: var(--text-muted);
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

.wizard-mode-toggle {
  display: flex;
  align-items: center;
  gap: 6px;
  color: var(--text-secondary);
  font-size: 12px;
  white-space: nowrap;
}

.wizard-steps {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 8px;
  list-style: none;
  margin: 0 0 12px;
  padding: 0;
}

.wizard-steps li {
  min-width: 0;
  padding: 9px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
}

.wizard-steps li.done {
  border-color: rgba(99, 230, 190, 0.38);
}

.wizard-steps li.active {
  border-color: var(--accent);
}

.wizard-steps span {
  display: block;
  color: var(--text-muted);
  font-size: 11px;
  margin-bottom: 3px;
}

.wizard-steps strong {
  display: block;
  color: var(--text-primary);
  font-size: 12px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.wizard-step {
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.wizard-hint {
  margin: 0;
  color: var(--text-secondary);
  font-size: 12px;
  line-height: 1.5;
}

.wizard-object-list {
  max-height: 220px;
}

.wizard-field-chips {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
}

.wizard-summary {
  display: flex;
  flex-wrap: wrap;
  align-items: flex-end;
  gap: 12px;
}

.wizard-summary-item span {
  display: block;
  color: var(--text-muted);
  font-size: 11px;
  margin-bottom: 2px;
}

.wizard-summary-item strong {
  display: block;
  color: var(--text-primary);
  font-size: 13px;
}

.wizard-step-actions {
  display: flex;
  align-items: center;
  gap: 10px;
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
  color: var(--text-muted);
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
  color: var(--text-muted);
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
  background: var(--bg-tertiary);
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
  border-top: 1px solid var(--border);
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
  color: var(--text-muted);
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
  color: var(--text-muted);
  text-align: center;
  padding: 22px;
}

@media (max-width: 1000px) {
  .content-grid,
  .path-write,
  .batch-write,
  .setter-call-inputs,
  .guide-steps,
  .wizard-steps,
  .status-band {
    grid-template-columns: 1fr;
  }

  .locator-head-actions {
    flex-wrap: wrap;
  }

  .wizard-summary {
    flex-direction: column;
    align-items: stretch;
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
