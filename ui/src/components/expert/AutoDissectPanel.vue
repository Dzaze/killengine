<script setup lang="ts">
import { onMounted, onUnmounted, ref } from 'vue'
import { useAppStore } from '@/stores/app'
import { backend } from '@/services/backend'
import { useI18n } from 'vue-i18n'
import InfoDot from '@/components/expert/InfoDot.vue'
import RiskBadge from '@/components/expert/RiskBadge.vue'

const store = useAppStore()
const { t } = useI18n()

const busy = ref(false)
const result = ref<Record<string, unknown> | null>(null)
const maxResults = ref(128)
const minConfidence = ref(0.5)

// findStructureInstancesAsync ne bloque plus le thread GUI (le scan peut
// tester des dizaines de millions de positions sur toute la mémoire
// writable de la cible, potentiellement plusieurs secondes/minutes) :
// elle renvoie juste {started:true} immédiatement, le vrai résultat arrive
// via le signal findStructureInstancesFinished (branché ci-dessous).
async function runAutoDissect() {
  // Utiliser le template sélectionné ou le dernier sauvegardé
  const template = store.structureTemplates[store.structureTemplates.length - 1]
  if (!template) {
    result.value = { success: false, error: t('autoDissect.noTemplateError') }
    return
  }

  busy.value = true
  result.value = null
  try {
    const controller = backend.getController()
    if (!controller.findStructureInstancesAsync) {
      busy.value = false
      result.value = { success: false, error: t('autoDissect.backendUnavailable') }
      return
    }
    const templateJson: Record<string, unknown> = {
      name: template.name,
      instanceDelta: 0,
      fields: template.fields,
      maxResults: maxResults.value,
      minConfidence: minConfidence.value,
      requirePointerValidity: true,
    }
    const started = await controller.findStructureInstancesAsync(templateJson)
    if (!started?.started) {
      busy.value = false
      result.value = started ?? { success: false, error: t('autoDissect.missingBackendResponse') }
    }
  } catch (e) {
    busy.value = false
    result.value = { success: false, error: String(e) }
  }
}

onMounted(() => {
  // AUDIT-UI-MODULES-B1 (docs/PHASE_TRACKER.md, 28/09/2026) : même défaut que
  // ModulesView.vue, trouvé en corrigeant celui-ci -- `backend.getController()`
  // retourne la même instance pour toute la session alors que ce composant
  // est détruit/recréé à chaque affichage ; sans disconnect explicite,
  // chaque visite ajoutait une nouvelle callback sans jamais retirer les
  // précédentes.
  const controller = backend.getController()
  const onFindStructureInstancesFinished = (finished: Record<string, unknown>) => {
    busy.value = false
    result.value = finished
  }
  controller.findStructureInstancesFinished?.connect(onFindStructureInstancesFinished)
  onUnmounted(() => {
    controller.findStructureInstancesFinished?.disconnect?.(onFindStructureInstancesFinished)
  })
})
</script>

<template>
  <section class="panel">
    <div class="panel-head">
      <div class="panel-heading">
        <h2>{{ $t('autoDissect.title') }}</h2>
        <InfoDot topic="autoDissect" />
        <RiskBadge level="read" />
      </div>
      <span v-if="result?.instanceCount">
        {{ $t('autoDissect.instanceCount', { count: result.instanceCount }) }}
      </span>
    </div>

    <p class="hint">
      {{ $t('autoDissect.intro') }}
    </p>

    <div class="controls">
      <label>
        {{ $t('autoDissect.maxResults') }}
        <input v-model.number="maxResults" class="small-input" type="number" min="1" max="1024" />
      </label>
      <label>
        {{ $t('autoDissect.minConfidence') }}
        <input v-model.number="minConfidence" class="small-input" type="number" min="0" max="1" step="0.1" />
      </label>
      <button
        class="btn btn-primary"
        :disabled="busy || store.structureTemplates.length === 0"
        @click="runAutoDissect()"
      >
        {{ busy ? $t('autoDissect.scanning') : $t('autoDissect.scan') }}
      </button>
    </div>

    <!-- Résultat : table d'instances -->
    <div v-if="result?.instanceCount" class="instances-table">
      <table class="data-table">
        <thead>
          <tr>
            <th>#</th>
            <th>{{ $t('autoDissect.address') }}</th>
            <th>{{ $t('autoDissect.confidence') }}</th>
            <th>{{ $t('autoDissect.values') }}</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="(inst, idx) in (result.instances as Record<string, unknown>[] ?? [])" :key="idx">
            <td class="idx-cell">{{ idx + 1 }}</td>
            <td class="addr-cell"><code>{{ inst.baseAddress }}</code></td>
            <td class="conf-cell">
              <span :class="['conf-badge', { good: (inst.confidence as number) >= 0.8, mid: (inst.confidence as number) >= 0.5 && (inst.confidence as number) < 0.8 }]">
                {{ ((inst.confidence as number) * 100).toFixed(0) }}%
              </span>
            </td>
            <td class="vals-cell">
              <span v-for="(v, vi) in (inst.fieldValues as unknown[] ?? [])" :key="vi" class="field-val">
                {{ v }}
              </span>
            </td>
          </tr>
        </tbody>
      </table>
    </div>

    <!-- Aucun résultat -->
    <div v-else-if="result && !result.success" class="empty-state">
      {{ result.error ?? $t('autoDissect.empty') }}
    </div>

    <div v-if="result?.scannedRegions" class="scan-stats">
      <span>{{ $t('autoDissect.scannedRegions') }} : {{ result.scannedRegions }}</span>
      <span>{{ $t('autoDissect.totalCandidates') }} : {{ result.totalCandidates }}</span>
    </div>
  </section>
</template>

<style scoped>
.panel-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  margin-bottom: 12px;
}

.panel-heading {
  display: flex;
  align-items: center;
  gap: 8px;
}

.panel-heading h2 {
  font-size: 16px;
  color: var(--text-primary);
  margin: 0;
}

.hint {
  color: var(--text-secondary);
  font-size: 12px;
  margin-bottom: 12px;
}

.controls {
  display: flex;
  gap: 12px;
  align-items: center;
  margin-bottom: 12px;
}

.controls label {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 12px;
  color: var(--text-secondary);
}

.small-input {
  width: 70px;
  padding: 4px 6px;
  border: 1px solid var(--border);
  border-radius: 4px;
  background: var(--bg-tertiary);
  color: var(--text-primary);
  font-size: 12px;
}

.data-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 12px;
}

.data-table th {
  text-align: left;
  padding: 6px 10px;
  border-bottom: 1px solid var(--border);
  color: var(--text-muted);
  font-weight: 600;
}

.data-table td {
  padding: 6px 10px;
  border-bottom: 1px solid var(--border);
}

.data-table tbody tr:hover {
  background: var(--bg-tertiary);
}

.addr-cell code {
  font-family: 'Cascadia Code', 'Fira Code', monospace;
  font-size: 11px;
  color: var(--accent);
}

.conf-badge {
  display: inline-block;
  padding: 2px 6px;
  border-radius: 4px;
  font-size: 11px;
  font-weight: 600;
}

.conf-badge.good {
  background: rgba(34, 197, 94, 0.15);
  color: #22c55e;
}

.conf-badge.mid {
  background: rgba(234, 179, 8, 0.15);
  color: #eab308;
}

.field-val {
  display: inline-block;
  padding: 1px 4px;
  margin-right: 4px;
  font-size: 11px;
  font-family: 'Cascadia Code', 'Fira Code', monospace;
  color: var(--text-secondary);
  background: var(--bg-tertiary);
  border-radius: 3px;
}

.empty-state {
  padding: 16px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
  color: var(--text-secondary);
  font-size: 12px;
  text-align: center;
}

.scan-stats {
  display: flex;
  gap: 20px;
  padding: 8px 14px;
  margin-top: 8px;
  font-size: 11px;
  color: var(--text-muted);
  background: var(--bg-tertiary);
  border-radius: 4px;
}
</style>
