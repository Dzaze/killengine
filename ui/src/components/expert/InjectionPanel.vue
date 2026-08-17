<script setup lang="ts">
import { useAppStore } from '@/stores/app'
import InfoDot from '@/components/expert/InfoDot.vue'
import RiskBadge from '@/components/expert/RiskBadge.vue'

const store = useAppStore()
</script>

<template>
  <section class="panel">
    <div class="panel-title">
      <div class="panel-heading">
        <h2>Injection / Hooking / Auto-assembler</h2>
        <InfoDot topic="injection" />
        <RiskBadge level="code" />
      </div>
      <span>Outils Expert manuels — confirmation requise à chaque action</span>
    </div>
    <p class="hint">
      Modifie activement le processus cible. Chaque action ci-dessous demande une confirmation explicite,
      et n'est possible que si le niveau de risque Auto est réglé sur Trainer dans Paramètres.
    </p>

    <div class="injection-block">
      <h3>Injection DLL</h3>
      <div class="controls injection-controls">
        <input
          v-model="store.injectDllPath"
          class="input"
          placeholder="Chemin absolu de la DLL"
          :disabled="store.injectionBusy"
          @keyup.enter="store.injectDll()"
        />
        <button class="btn btn-primary compact" type="button" :disabled="store.injectionBusy || !store.injectDllPath.trim()" @click="store.injectDll()">
          Injecter
        </button>
      </div>
      <p v-if="store.injectionResult" :class="store.injectionResult.success ? 'hint' : 'error'">
        {{ store.injectionResult.success ? `Injectée, base 0x${store.injectionResult.moduleBase}` : store.injectionResult.error }}
      </p>
    </div>

    <div class="injection-block">
      <h3>Inline hook</h3>
      <div class="controls injection-controls">
        <input v-model="store.hookTargetAddress" class="input" placeholder="Adresse cible (0x...)" :disabled="store.injectionBusy" />
        <input v-model="store.hookFunctionAddress" class="input" placeholder="Adresse hook (0x...)" :disabled="store.injectionBusy" />
      </div>
      <div class="row-actions">
        <button class="btn btn-primary compact" type="button" :disabled="store.injectionBusy || !store.hookTargetAddress.trim() || !store.hookFunctionAddress.trim()" @click="store.installHook()">
          Installer hook
        </button>
        <button class="btn btn-secondary compact" type="button" :disabled="store.injectionBusy || !store.hookTargetAddress.trim()" @click="store.removeHook()">
          Retirer hook
        </button>
      </div>
      <p v-if="store.activeFunctionHook" :class="store.activeFunctionHook.success ? 'hint' : 'error'">
        {{ store.activeFunctionHook.success ? `Hook actif, trampoline 0x${store.activeFunctionHook.trampolineAddress}` : store.activeFunctionHook.error }}
      </p>
    </div>

    <div class="injection-block">
      <h3>Script auto-assembler</h3>
      <p class="hint">
        Sous-ensemble volontairement borné : nop/ret/int3/db/jmp/call/je/jne. Les instructions mémoire
        complexes sont refusées proprement plutôt que mal exécutées.
      </p>
      <textarea
        v-model="store.autoAsmScriptText"
        class="input autoasm-textarea"
        rows="6"
        placeholder="alloc(newmem, 256)&#10;label(returnhere)&#10;..."
        :disabled="store.injectionBusy"
      ></textarea>
      <div class="row-actions">
        <button class="btn btn-secondary compact" type="button" :disabled="!store.autoAsmScriptText.trim()" @click="store.previewAutoAsmScript()">
          Aperçu
        </button>
        <button class="btn btn-primary compact" type="button" :disabled="store.injectionBusy || !store.autoAsmScriptText.trim()" @click="store.executeAutoAsmScript()">
          Exécuter
        </button>
        <button class="btn btn-secondary compact" type="button" :disabled="store.injectionBusy || !store.autoAsmResult" @click="store.restoreAutoAsmScript()">
          Restaurer
        </button>
      </div>
      <div v-if="store.autoAsmPreview" class="metrics">
        <span>{{ store.autoAsmPreview.parseSuccess ? `${store.autoAsmPreview.instructionCount} instruction(s) reconnue(s)` : `Erreur ligne ${store.autoAsmPreview.parseErrorLine}: ${store.autoAsmPreview.parseError}` }}</span>
        <span v-if="store.autoAsmPreview.compileSuccess">Bytes compilés: {{ store.autoAsmPreview.compiledBytes }}</span>
        <span v-else-if="store.autoAsmPreview.compileError">Compilation: {{ store.autoAsmPreview.compileError }}</span>
      </div>
      <p v-if="store.autoAsmResult" :class="store.autoAsmResult.success ? 'hint' : 'error'">
        {{ store.autoAsmResult.success ? `Actif, patch à 0x${store.autoAsmResult.patchAddress}` : store.autoAsmResult.error }}
      </p>
    </div>
  </section>
</template>

<style scoped>
.injection-block {
  margin-top: 12px;
  padding-top: 12px;
  border-top: 1px solid var(--border);
}

.injection-block:first-of-type {
  margin-top: 8px;
  padding-top: 0;
  border-top: none;
}

.injection-block h3 {
  margin-bottom: 6px;
  color: var(--text-primary);
  font-size: 13px;
}

.injection-controls {
  grid-template-columns: 1fr auto;
}

.autoasm-textarea {
  width: 100%;
  margin-top: 6px;
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
  resize: vertical;
}
</style>
