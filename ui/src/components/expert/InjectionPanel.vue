<script setup lang="ts">
import { onMounted } from 'vue'
import { useAppStore } from '@/stores/app'
import InfoDot from '@/components/expert/InfoDot.vue'
import RiskBadge from '@/components/expert/RiskBadge.vue'

const store = useAppStore()

onMounted(() => {
  if (store.isAttached) {
    void store.refreshSavedAutoAsmScripts()
  }
})
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
      <div class="symbol-resolve-row">
        <input v-model="store.symbolModuleName" class="input" placeholder="Module (ex. kernel32.dll)" :disabled="store.injectionBusy" />
        <input v-model="store.symbolFunctionName" class="input" placeholder="Fonction (ex. CreateFileW)" :disabled="store.injectionBusy" @keyup.enter="store.resolveSymbol()" />
        <button
          class="btn btn-secondary compact"
          type="button"
          :disabled="store.injectionBusy || !store.symbolModuleName.trim() || !store.symbolFunctionName.trim()"
          @click="store.resolveSymbol()"
        >
          Résoudre
        </button>
      </div>
      <p v-if="store.symbolResolveResult" :class="store.symbolResolveResult.success ? 'hint' : 'error'">
        <template v-if="store.symbolResolveResult.success">
          {{ store.symbolModuleName }}!{{ store.symbolFunctionName }} = 0x{{ store.symbolResolveResult.address }}
          <button class="btn btn-secondary compact" type="button" @click="store.applyResolvedSymbolToHookTarget()">
            Utiliser comme cible
          </button>
        </template>
        <template v-else>{{ store.symbolResolveResult.error }}</template>
      </p>
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

    <div class="injection-block api-hook-block">
      <h3>Interception de fonctions (MinHook)</h3>
      <p class="hint">
        Injecte un composant qui pose un inline hook MinHook sur module!fonction dans le processus cible.
        Mode Compter : interception passive (compteur d appels, l original reste appelé) pour confirmer qu une fonction
        est réellement utilisée avant de patcher. Mode Forcer retour : la fonction retourne la valeur donnée sans
        s exécuter (QA/simulation de pannes) — adapté aux retours entier/pointeur/bool uniquement.
      </p>
      <div class="symbol-resolve-row">
        <input v-model="store.apiHookModuleName" class="input" placeholder="Module (ex. kernel32.dll)" :disabled="store.apiHookBusy" />
        <input v-model="store.apiHookFunctionName" class="input" placeholder="Fonction (ex. Sleep)" :disabled="store.apiHookBusy" @keyup.enter="store.startApiHook()" />
      </div>
      <div class="controls injection-controls">
        <label class="api-hook-mode">
          <input type="radio" :value="0" v-model="store.apiHookMode" :disabled="store.apiHookBusy" /> Compter
        </label>
        <label class="api-hook-mode">
          <input type="radio" :value="1" v-model="store.apiHookMode" :disabled="store.apiHookBusy" /> Forcer retour
        </label>
        <input v-if="store.apiHookMode === 1" v-model.number="store.apiHookForcedReturn" type="number" class="input" placeholder="Valeur de retour forcée" :disabled="store.apiHookBusy" />
      </div>
      <div class="row-actions">
        <button class="btn btn-primary compact" type="button" :disabled="store.apiHookBusy || !store.apiHookModuleName.trim() || !store.apiHookFunctionName.trim()" @click="store.startApiHook()">
          Intercepter
        </button>
        <button class="btn btn-secondary compact" type="button" :disabled="store.apiHookBusy || !store.apiHookStatus?.active" @click="store.stopApiHook()">
          Retirer
        </button>
      </div>
      <p v-if="store.apiHookStatus" :class="store.apiHookStatus.success ? 'hint' : 'error'">
        {{ store.apiHookStatus.active ? `Actif, ${store.apiHookStatus.callCount ?? 0} appel(s) intercepté(s)` : (store.apiHookStatus.error || 'Inactif') }}
      </p>
    </div>

    <div class="injection-block">
      <h3>Script auto-assembler</h3>
      <p class="hint">
        Sous-ensemble volontairement borné : nop/ret/int3/db/jmp/call/je/jne, <code>mov [registre+déplacement], immédiat</code>,
        blocs <code>"nom:"</code> (lié à un <code>alloc()</code>) et <code>"module.exe"+offset:</code> (site existant, pattern
        Cheat Engine classique — voir le bouton "Forcer valeur (hook)" sur une capture Écrit par pour un raccourci qui
        génère ce script automatiquement, sans avoir à l'écrire à la main). Adressage indexé/RIP-relatif et le reste des
        mnémoniques refusés proprement plutôt que mal exécutés.
      </p>
      <textarea
        v-model="store.autoAsmScriptText"
        class="input autoasm-textarea"
        rows="6"
        placeholder="alloc(newmem, 256)&#10;label(returnhere)&#10;&#10;newmem:&#10;mov [rax+8], 9999&#10;jmp returnhere&#10;&#10;&quot;monjeu.exe&quot;+0x12345:&#10;jmp newmem&#10;nop&#10;returnhere:"
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

      <div class="autoasm-save-row">
        <input
          v-model="store.autoAsmScriptName"
          class="input"
          placeholder="Nom du script (pour le sauvegarder)"
          :disabled="store.injectionBusy"
        />
        <button
          class="btn btn-secondary compact"
          type="button"
          :disabled="store.injectionBusy || !store.autoAsmScriptText.trim() || !store.autoAsmScriptName.trim()"
          @click="store.saveAutoAsmScript()"
        >
          Sauvegarder dans le profil
        </button>
      </div>
      <p v-if="store.autoAsmSaveResult" :class="store.autoAsmSaveResult.success ? 'hint' : 'error'">
        {{ store.autoAsmSaveResult.success ? `Sauvegardé (${store.autoAsmSaveResult.scriptCount} script(s) dans ce profil)` : store.autoAsmSaveResult.error }}
      </p>

      <div class="autoasm-saved-list">
        <div class="autoasm-saved-header">
          <h4>Scripts sauvegardés</h4>
          <button class="btn btn-secondary compact" type="button" :disabled="store.autoAsmSavedScriptsBusy" @click="store.refreshSavedAutoAsmScripts()">
            {{ store.autoAsmSavedScriptsBusy ? 'Chargement...' : 'Rafraîchir' }}
          </button>
        </div>
        <p v-if="!store.autoAsmSavedScripts.length" class="hint">Aucun script sauvegardé pour ce profil.</p>
        <div v-for="saved in store.autoAsmSavedScripts" :key="String(saved.name)" class="autoasm-saved-entry">
          <span class="autoasm-saved-name">{{ saved.name }}</span>
          <div class="row-actions">
            <button class="btn btn-secondary compact" type="button" @click="store.autoAsmScriptText = String(saved.scriptText ?? '')">
              Charger
            </button>
            <button class="btn btn-primary compact" type="button" :disabled="store.injectionBusy" @click="store.applySavedAutoAsmScript(String(saved.name))">
              Exécuter
            </button>
            <button class="btn btn-secondary compact" type="button" @click="store.deleteSavedAutoAsmScript(String(saved.name))">
              Supprimer
            </button>
          </div>
        </div>
      </div>
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

.api-hook-mode {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  color: var(--text-primary);
  font-size: 12px;
  white-space: nowrap;
}

.symbol-resolve-row {
  display: grid;
  grid-template-columns: 1fr 1fr auto;
  gap: 8px;
  margin-bottom: 8px;
}

.autoasm-textarea {
  width: 100%;
  margin-top: 6px;
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
  resize: vertical;
}

.autoasm-save-row {
  display: grid;
  grid-template-columns: 1fr auto;
  gap: 8px;
  margin-top: 10px;
}

.autoasm-saved-list {
  margin-top: 10px;
}

.autoasm-saved-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 6px;
}

.autoasm-saved-header h4 {
  color: var(--text-secondary);
  font-size: 12px;
}

.autoasm-saved-entry {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
  padding: 6px 8px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
  margin-bottom: 4px;
}

.autoasm-saved-name {
  overflow: hidden;
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
  text-overflow: ellipsis;
  white-space: nowrap;
}
</style>
