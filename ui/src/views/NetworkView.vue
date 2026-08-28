<script setup lang="ts">
import { computed, onMounted } from 'vue'
import { useAppStore } from '@/stores/app'
import InfoDot from '@/components/expert/InfoDot.vue'
import PanelIntro from '@/components/common/PanelIntro.vue'

const store = useAppStore()

const isNetworkBlocked = computed(() => store.networkBlockStatus?.blocked === true)
const networkBlockStatusLabel = computed(() => {
  if (!store.networkBlockStatus) return 'Inconnu'
  return isNetworkBlocked.value ? 'Réseau coupé' : 'Réseau normal'
})

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

    <PanelIntro
      what="Le blocage réseau ciblé : coupe le trafic entrant/sortant du processus attaché via une règle pare-feu dédiée."
      purpose="Vérifier si une valeur mémoire instable vient d'une synchro serveur en arrière-plan plutôt que d'un recalcul purement local."
      how="Attache un processus, puis active/désactive le blocage réseau et observe si la valeur se stabilise."
    />

    <div v-if="!store.isAttached" class="empty-state">
      <p>Attache d'abord un processus autorisé pour couper ou rétablir son accès réseau.</p>
      <button class="btn btn-secondary" @click="store.activeView = 'process'">Aller à Processus</button>
    </div>

    <template v-else>
      <section class="status-band">
        <span>Statut : <strong>{{ networkBlockStatusLabel }}</strong></span>
        <span v-if="store.networkBlockStatus?.exePath">Cible : <strong>{{ store.networkBlockStatus.exePath }}</strong></span>
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
