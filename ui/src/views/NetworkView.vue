<script setup lang="ts">
import { computed, onMounted } from 'vue'
import { useAppStore } from '@/stores/app'
import InfoDot from '@/components/expert/InfoDot.vue'

const store = useAppStore()

const riskModeBlocksNetworkBlock = computed(() => store.settingAutoRiskMode !== 'Trainer')
const isNetworkBlocked = computed(() => store.networkBlockStatus?.blocked === true)
const networkBlockStatusLabel = computed(() => {
  if (!store.networkBlockStatus) return 'Inconnu'
  return isNetworkBlocked.value ? 'Réseau coupé' : 'Réseau normal'
})

function enableTrainerMode() {
  store.settingAutoRiskMode = 'Trainer'
  store.lastRiskBlockReason = ''
}

function toggleNetworkBlock() {
  if (isNetworkBlocked.value) {
    void store.unblockProcessNetwork()
  } else {
    void store.blockProcessNetwork()
  }
}

onMounted(() => {
  void store.refreshProcessNetworkBlockStatus()
})
</script>

<template>
  <div class="network-view">
    <div class="header">
      <div>
        <h1>Réseau</h1>
        <p>{{ store.isAttached ? store.processName : 'Aucun processus attaché' }}</p>
      </div>
      <InfoDot topic="network" align="right" />
    </div>

    <div v-if="!store.isAttached" class="empty-state">
      <p>Attache d'abord un processus autorisé pour couper ou rétablir son accès réseau.</p>
      <button class="btn btn-secondary" @click="store.activeView = 'process'">Aller à Processus</button>
    </div>

    <template v-else>
      <section class="status-band">
        <span>Statut : <strong>{{ networkBlockStatusLabel }}</strong></span>
        <span v-if="store.networkBlockStatus?.exePath">Cible : <strong>{{ store.networkBlockStatus.exePath }}</strong></span>
      </section>

      <section v-if="riskModeBlocksNetworkBlock && !isNetworkBlocked" class="warning-band">
        <div>
          <strong>Blocage réseau bloqué par le mode Auto.</strong>
          <span>Modifier le pare-feu Windows est traité comme une injection : passe en Trainer pour l'autoriser.</span>
        </div>
        <button class="btn btn-secondary compact" @click="enableTrainerMode">
          Passer en Trainer
        </button>
      </section>

      <section v-if="store.lastRiskBlockReason" class="warning-band muted">
        {{ store.lastRiskBlockReason }}
      </section>

      <section class="panel">
        <p class="hint intro">
          Coupe le trafic entrant/sortant de la cible (règle pare-feu Windows dédiée à son exécutable) — utile
          pour vérifier si une valeur en mémoire instable vient d'une synchro serveur en arrière-plan plutôt
          que d'une réallocation purement locale.
        </p>

        <div class="actions-row">
          <button
            class="btn"
            :class="isNetworkBlocked ? 'btn-secondary' : 'btn-primary'"
            :disabled="store.networkBlockBusy"
            :title="riskModeBlocksNetworkBlock && !isNetworkBlocked ? 'Le mode Auto actuel bloque cette action. Passe en Trainer pour couper le réseau.' : ''"
            @click="toggleNetworkBlock"
          >
            {{ isNetworkBlocked ? 'Rétablir le réseau' : 'Couper le réseau' }}
          </button>
          <span class="hint">
            Déclenche une invite UAC (élévation Windows) — jamais silencieux. La règle reste posée même après
            avoir détaché le processus ; utilise "Rétablir le réseau" pour la retirer.
          </span>
        </div>
      </section>
    </template>
  </div>
</template>

<style scoped>
.network-view {
  padding: 24px 32px;
  max-width: 900px;
}

.header {
  display: flex;
  align-items: center;
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

.empty-state,
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
  display: flex;
  gap: 20px;
  padding: 14px;
  margin-bottom: 14px;
  color: var(--text-dim);
}

.warning-band {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 14px;
  border: 1px solid rgba(245, 158, 11, 0.45);
  background: rgba(245, 158, 11, 0.12);
  border-radius: 8px;
  padding: 12px 14px;
  margin-bottom: 14px;
  color: var(--text-primary);
}

.warning-band div {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.warning-band span,
.warning-band.muted {
  color: var(--text-dim);
  font-size: 12px;
}

.panel {
  padding: 18px;
}

.intro {
  margin-bottom: 18px;
}

.actions-row {
  display: flex;
  align-items: center;
  gap: 14px;
}

.hint {
  color: var(--text-dim);
  font-size: 12px;
}
</style>
