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
        <h2>{{ $t('injectionPanel.title') }}</h2>
        <InfoDot topic="injection" />
        <RiskBadge level="code" />
      </div>
      <span>{{ $t('injectionPanel.manualTools') }}</span>
    </div>
    <p class="hint">
      {{ $t('injectionPanel.mainHint') }}
    </p>

    <div class="injection-block">
      <h3>{{ $t('injectionPanel.dllTitle') }}</h3>
      <div class="controls injection-controls">
        <input
          v-model="store.injectDllPath"
          class="input"
          :placeholder="$t('injectionPanel.dllPathPlaceholder')"
          :disabled="store.injectionBusy"
          @keyup.enter="store.injectDll()"
        />
        <button class="btn btn-primary compact" type="button" :disabled="store.injectionBusy || !store.injectDllPath.trim()" @click="store.injectDll()">
          {{ $t('injectionPanel.inject') }}
        </button>
      </div>
      <p v-if="store.injectionResult" :class="store.injectionResult.success ? 'hint' : 'error'">
        {{ store.injectionResult.success ? $t('injectionPanel.injectedBase', { base: store.injectionResult.moduleBase }) : store.injectionResult.error }}
      </p>
    </div>

    <div class="injection-block">
      <h3>{{ $t('injectionPanel.inlineHookTitle') }} <InfoDot topic="inlineHook" /></h3>
      <div class="symbol-resolve-row">
        <input v-model="store.symbolModuleName" class="input" :placeholder="$t('injectionPanel.modulePlaceholder')" :disabled="store.injectionBusy" />
        <input v-model="store.symbolFunctionName" class="input" :placeholder="$t('injectionPanel.functionPlaceholder')" :disabled="store.injectionBusy" @keyup.enter="store.resolveSymbol()" />
        <button
          class="btn btn-secondary compact"
          type="button"
          :disabled="store.injectionBusy || !store.symbolModuleName.trim() || !store.symbolFunctionName.trim()"
          @click="store.resolveSymbol()"
        >
          {{ $t('injectionPanel.resolve') }}
        </button>
      </div>
      <p v-if="store.symbolResolveResult" :class="store.symbolResolveResult.success ? 'hint' : 'error'">
        <template v-if="store.symbolResolveResult.success">
          {{ store.symbolModuleName }}!{{ store.symbolFunctionName }} = 0x{{ store.symbolResolveResult.address }}
          <button class="btn btn-secondary compact" type="button" @click="store.applyResolvedSymbolToHookTarget()">
            {{ $t('injectionPanel.useAsTarget') }}
          </button>
        </template>
        <template v-else>{{ store.symbolResolveResult.error }}</template>
      </p>
      <div class="controls injection-controls">
        <input v-model="store.hookTargetAddress" class="input" :placeholder="$t('injectionPanel.targetAddressPlaceholder')" :disabled="store.injectionBusy" />
        <input v-model="store.hookFunctionAddress" class="input" :placeholder="$t('injectionPanel.hookAddressPlaceholder')" :disabled="store.injectionBusy" />
      </div>
      <div class="row-actions">
        <button class="btn btn-primary compact" type="button" :disabled="store.injectionBusy || !store.hookTargetAddress.trim() || !store.hookFunctionAddress.trim()" @click="store.installHook()">
          {{ $t('injectionPanel.installHook') }}
        </button>
        <button class="btn btn-secondary compact" type="button" :disabled="store.injectionBusy || !store.hookTargetAddress.trim()" @click="store.removeHook()">
          {{ $t('injectionPanel.removeHook') }}
        </button>
      </div>
      <p v-if="store.activeFunctionHook" :class="store.activeFunctionHook.success ? 'hint' : 'error'">
        {{ store.activeFunctionHook.success ? $t('injectionPanel.hookActive', { address: store.activeFunctionHook.trampolineAddress }) : store.activeFunctionHook.error }}
      </p>
    </div>

    <div class="injection-block api-hook-block">
      <h3>{{ $t('injectionPanel.apiHookTitle') }} <InfoDot topic="apiHook" /></h3>
      <p class="hint">
        {{ $t('injectionPanel.apiHookHint') }}
      </p>
      <div class="symbol-resolve-row">
        <input v-model="store.apiHookModuleName" class="input" :placeholder="$t('injectionPanel.modulePlaceholder')" :disabled="store.apiHookBusy" />
        <input v-model="store.apiHookFunctionName" class="input" :placeholder="$t('injectionPanel.functionSleepPlaceholder')" :disabled="store.apiHookBusy" @keyup.enter="store.startApiHook()" />
      </div>
      <div class="controls injection-controls">
        <label class="api-hook-mode">
          <input type="radio" :value="0" v-model="store.apiHookMode" :disabled="store.apiHookBusy" /> {{ $t('injectionPanel.countMode') }}
        </label>
        <label class="api-hook-mode">
          <input type="radio" :value="1" v-model="store.apiHookMode" :disabled="store.apiHookBusy" /> {{ $t('injectionPanel.forceReturnMode') }}
        </label>
        <input v-if="store.apiHookMode === 1" v-model.number="store.apiHookForcedReturn" type="number" class="input" :placeholder="$t('injectionPanel.forcedReturnPlaceholder')" :disabled="store.apiHookBusy" />
      </div>
      <div class="row-actions">
        <button class="btn btn-primary compact" type="button" :disabled="store.apiHookBusy || !store.apiHookModuleName.trim() || !store.apiHookFunctionName.trim()" @click="store.startApiHook()">
          {{ $t('injectionPanel.intercept') }}
        </button>
        <button class="btn btn-secondary compact" type="button" :disabled="store.apiHookBusy || !store.apiHookStatus?.active" @click="store.stopApiHook()">
          {{ $t('injectionPanel.remove') }}
        </button>
      </div>
      <p v-if="store.apiHookStatus" :class="store.apiHookStatus.success ? 'hint' : 'error'">
        {{ store.apiHookStatus.active ? $t('injectionPanel.apiHookActive', { count: store.apiHookStatus.callCount ?? 0 }) : (store.apiHookStatus.error || $t('injectionPanel.inactive')) }}
      </p>
    </div>

    <div class="injection-block">
      <h3>{{ $t('injectionPanel.autoAsmTitle') }}</h3>
      <p class="hint">
        {{ $t('injectionPanel.autoAsmHintPrefix') }} <code>{{ $t('injectionPanel.autoAsmMovExample') }}</code>,
        {{ $t('injectionPanel.autoAsmHintMiddle') }} <code>{{ $t('injectionPanel.autoAsmLabelExample') }}</code> (<code>alloc()</code>) {{ $t('injectionPanel.autoAsmHintAnd') }} <code>"module.exe"+offset:</code>.
        {{ $t('injectionPanel.autoAsmHintSuffix') }}
      </p>
      <textarea
        v-model="store.autoAsmScriptText"
        class="input autoasm-textarea"
        rows="6"
        :placeholder="$t('injectionPanel.autoAsmPlaceholder')"
        :disabled="store.injectionBusy"
      ></textarea>
      <div class="row-actions">
        <button class="btn btn-secondary compact" type="button" :disabled="!store.autoAsmScriptText.trim()" @click="store.previewAutoAsmScript()">
          {{ $t('injectionPanel.preview') }}
        </button>
        <button class="btn btn-primary compact" type="button" :disabled="store.injectionBusy || !store.autoAsmScriptText.trim()" @click="store.executeAutoAsmScript()">
          {{ $t('injectionPanel.execute') }}
        </button>
        <button class="btn btn-secondary compact" type="button" :disabled="store.injectionBusy || !store.autoAsmResult" @click="store.restoreAutoAsmScript()">
          {{ $t('injectionPanel.restore') }}
        </button>
      </div>
      <div v-if="store.autoAsmPreview" class="metrics">
        <span>{{ store.autoAsmPreview.parseSuccess ? $t('injectionPanel.instructionsRecognized', { count: store.autoAsmPreview.instructionCount }) : $t('injectionPanel.parseError', { line: store.autoAsmPreview.parseErrorLine, error: store.autoAsmPreview.parseError }) }}</span>
        <span v-if="store.autoAsmPreview.compileSuccess">{{ $t('injectionPanel.compiledBytes', { bytes: store.autoAsmPreview.compiledBytes }) }}</span>
        <span v-else-if="store.autoAsmPreview.compileError">{{ $t('injectionPanel.compilationError', { error: store.autoAsmPreview.compileError }) }}</span>
      </div>
      <p v-if="store.autoAsmResult" :class="store.autoAsmResult.success ? 'hint' : 'error'">
        {{ store.autoAsmResult.success ? $t('injectionPanel.patchActive', { address: store.autoAsmResult.patchAddress }) : store.autoAsmResult.error }}
      </p>

      <div class="autoasm-save-row">
        <input
          v-model="store.autoAsmScriptName"
          class="input"
          :placeholder="$t('injectionPanel.scriptNamePlaceholder')"
          :disabled="store.injectionBusy"
        />
        <button
          class="btn btn-secondary compact"
          type="button"
          :disabled="store.injectionBusy || !store.autoAsmScriptText.trim() || !store.autoAsmScriptName.trim()"
          @click="store.saveAutoAsmScript()"
        >
          {{ $t('injectionPanel.saveToProfile') }}
        </button>
      </div>
      <p v-if="store.autoAsmSaveResult" :class="store.autoAsmSaveResult.success ? 'hint' : 'error'">
        {{ store.autoAsmSaveResult.success ? $t('injectionPanel.savedScriptCount', { count: store.autoAsmSaveResult.scriptCount }) : store.autoAsmSaveResult.error }}
      </p>

      <div class="autoasm-saved-list">
        <div class="autoasm-saved-header">
          <h4>{{ $t('injectionPanel.savedScripts') }}</h4>
          <button class="btn btn-secondary compact" type="button" :disabled="store.autoAsmSavedScriptsBusy" @click="store.refreshSavedAutoAsmScripts()">
            {{ store.autoAsmSavedScriptsBusy ? $t('injectionPanel.loading') : $t('injectionPanel.refresh') }}
          </button>
        </div>
        <p v-if="!store.autoAsmSavedScripts.length" class="hint">{{ $t('injectionPanel.noSavedScripts') }}</p>
        <div v-for="saved in store.autoAsmSavedScripts" :key="String(saved.name)" class="autoasm-saved-entry">
          <span class="autoasm-saved-name">{{ saved.name }}</span>
          <div class="row-actions">
            <button class="btn btn-secondary compact" type="button" @click="store.autoAsmScriptText = String(saved.scriptText ?? '')">
              {{ $t('injectionPanel.load') }}
            </button>
            <button class="btn btn-primary compact" type="button" :disabled="store.injectionBusy" @click="store.applySavedAutoAsmScript(String(saved.name))">
              {{ $t('injectionPanel.execute') }}
            </button>
            <button class="btn btn-secondary compact" type="button" @click="store.deleteSavedAutoAsmScript(String(saved.name))">
              {{ $t('injectionPanel.delete') }}
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
