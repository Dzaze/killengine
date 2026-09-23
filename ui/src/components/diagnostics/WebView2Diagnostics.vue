<script setup lang="ts">
import { useAppStore } from '@/stores/app'

const store = useAppStore()
</script>

<template>
  <div class="webview2-diagnostics">
    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.webview2CdpTitle') }}</h2>
        <span>{{ store.webView2CdpDebugFlagStatus?.enabled ? $t('settings.activeState') : $t('settings.inactive') }}</span>
      </div>
      <p class="hint">
        {{ $t('settings.webview2CdpHintPrefix') }} <strong>{{ $t('settings.webview2CdpHintStrong') }}</strong> {{ $t('settings.webview2CdpHintSuffix') }}
      </p>
      <div class="panel-actions">
        <button
          v-if="!store.webView2CdpDebugFlagStatus?.enabled"
          class="btn btn-secondary compact"
          :disabled="store.webView2CdpDebugFlagBusy"
          @click="store.enableWebView2CdpDebugFlag()"
        >
          {{ $t('settings.enableCdpDebug') }}
        </button>
        <button
          v-else
          class="btn btn-secondary compact"
          :disabled="store.webView2CdpDebugFlagBusy"
          @click="store.disableWebView2CdpDebugFlag()"
        >
          {{ $t('settings.disableCdpDebug') }}
        </button>
        <button
          class="btn btn-secondary compact"
          :disabled="store.webView2CdpDebugFlagBusy"
          @click="store.refreshWebView2CdpDebugFlagStatus()"
        >
          {{ $t('settings.refreshStatus') }}
        </button>
      </div>
      <p v-if="store.webView2CdpDebugFlagStatus?.value" class="status-line">
        {{ $t('settings.variableSetLabel') }} <code>{{ store.webView2CdpDebugFlagStatus.value }}</code>
      </p>
      <p v-if="store.webView2CdpDebugFlagStatus?.error" class="error">
        {{ store.webView2CdpDebugFlagStatus.error }}
      </p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.webview2PrepTitle') }}</h2>
        <span>{{ store.webView2SystemPrepStatus?.capabilityInstalled ? $t('settings.ready') : $t('settings.toPrepare') }}</span>
      </div>
      <p class="hint">
        {{ $t('settings.webview2PrepHint') }}
      </p>
      <div class="panel-actions">
        <button
          class="btn btn-secondary compact"
          :disabled="store.webView2SystemPrepBusy"
          @click="store.refreshWebView2SystemPrepStatus()"
        >
          {{ store.webView2SystemPrepBusy ? $t('settings.diagnosing') : $t('settings.runDiagnostic') }}
        </button>
        <button
          class="btn btn-secondary compact"
          :disabled="store.webView2SystemPrepBusy || store.webView2SystemPrepStatus?.capabilityInstalled"
          @click="store.installWebView2DeveloperModeCapability()"
        >
          {{ $t('settings.installCapability') }}
        </button>
      </div>
      <div v-if="store.webView2SystemPrepStatus" class="settings-grid compact-grid">
        <div>
          <strong>{{ $t('settings.developerMode') }}</strong>
          <span>{{ store.webView2SystemPrepStatus.developerModeEnabled ? $t('settings.enabledState') : $t('settings.disabledState') }}</span>
        </div>
        <div>
          <strong>{{ $t('settings.capabilityLabel') }}</strong>
          <span>{{ store.webView2SystemPrepStatus.capabilityState || '—' }}</span>
        </div>
      </div>
      <p v-if="store.webView2CapabilityInstallResult?.message" class="status-line">
        {{ store.webView2CapabilityInstallResult.message }}
      </p>
      <p v-else-if="store.webView2CapabilityInstallResult?.error" class="error">
        {{ store.webView2CapabilityInstallResult.error }}
      </p>
    </section>
  </div>
</template>
<style scoped>
.settings-view {
  max-width: 980px;
  padding: 24px 32px;
}

.header,
.panel-title,
.setting-row,
.ping-line,
.actions-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.panel-title span,
.empty-line {
  color: var(--text-dim);
  font-size: 12px;
}

.panel-actions {
  display: flex;
  align-items: center;
  gap: 8px;
}

.workspace-actions {
  flex-wrap: wrap;
  justify-content: flex-start;
  margin-top: 10px;
}

.workspace-export {
  margin-top: 12px;
}

.workspace-import {
  margin-top: 12px;
  padding-top: 10px;
  border-top: 1px solid var(--border);
}

.project-panel,
.bookmark-list,
.audit-panel {
  margin-top: 12px;
  padding-top: 10px;
  border-top: 1px solid var(--border);
}

.project-name-input {
  min-width: 180px;
}

.bookmark-create {
  display: grid;
  grid-template-columns: 1fr 1fr 120px 140px 1.5fr auto;
  gap: 8px;
  padding: 10px 0;
}

.bookmark-note-input {
  min-width: 0;
}

.project-row,
.bookmark-row,
.audit-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
  padding: 8px 0;
  border-top: 1px solid rgba(255, 255, 255, 0.06);
}

.project-row:first-of-type,
.bookmark-row:first-of-type,
.audit-row:first-of-type {
  border-top: none;
}

.project-row div,
.bookmark-row div,
.audit-row div {
  display: flex;
  min-width: 0;
  flex-direction: column;
  gap: 3px;
}

.project-row span,
.bookmark-row span,
.audit-row span {
  overflow: hidden;
  color: var(--text-dim);
  font-size: 12px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

@media (max-width: 980px) {
  .bookmark-create {
    grid-template-columns: 1fr 1fr;
  }
}

.workspace-import-input {
  width: 100%;
  min-height: 120px;
  resize: vertical;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
  line-height: 1.45;
  padding: 10px;
}

.import-preview {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin-top: 8px;
}

.import-preview span {
  border: 1px solid var(--border);
  border-radius: 999px;
  color: var(--text-dim);
  font-size: 11px;
  padding: 4px 8px;
}

.template-list {
  margin-top: 12px;
  padding-top: 10px;
  border-top: 1px solid var(--border);
}

.template-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
  padding: 8px 0;
  border-top: 1px solid rgba(255, 255, 255, 0.06);
}

.template-row:first-of-type {
  border-top: none;
}

.template-row div {
  display: flex;
  min-width: 0;
  flex-direction: column;
  gap: 3px;
}

.template-row span {
  overflow: hidden;
  color: var(--text-dim);
  font-size: 12px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.template-detail {
  margin-top: 10px;
  padding: 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.template-field-row {
  display: grid;
  grid-template-columns: 58px minmax(70px, 0.5fr) minmax(90px, 1fr) minmax(90px, 1fr) minmax(140px, 1.4fr);
  gap: 8px;
  align-items: center;
  min-height: 28px;
  padding: 5px 0;
  border-top: 1px solid rgba(255, 255, 255, 0.06);
  color: var(--text-dim);
  font-size: 12px;
}

.template-field-row:first-of-type {
  border-top: none;
}

.template-field-row code,
.template-field-row strong,
.template-field-row span {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.workspace-export pre {
  max-height: 320px;
  overflow: auto;
  padding: 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
  line-height: 1.45;
  white-space: pre-wrap;
}

.header {
  margin-bottom: 18px;
}

.header h1 {
  color: var(--text-primary);
  font-size: 22px;
}

.header p {
  margin-top: 4px;
  color: var(--text-dim);
  font-size: 13px;
}

.panel {
  margin-bottom: 12px;
  padding: 14px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
}

.panel-title {
  margin-bottom: 12px;
}

.panel-title h2 {
  color: var(--text-primary);
  font-size: 15px;
}

.panel-title h3 {
  color: var(--text-primary);
  font-size: 13px;
}

.setting-row strong,
.runtime-cell strong {
  display: block;
  color: var(--text-primary);
  font-size: 14px;
}

.settings-grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 10px;
}

.settings-grid label,
.toggle-row {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.settings-grid label span,
.toggle-row span,
.hint,
.status-line {
  color: var(--text-dim);
  font-size: 12px;
}

.status-line.error {
  color: var(--error);
  font-weight: 600;
}

.input {
  min-width: 0;
  width: 100%;
  min-height: 34px;
  padding: 8px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-size: 13px;
  outline: none;
  transition: border-color 0.15s, box-shadow 0.15s, background 0.15s;
}

.list-filter-input {
  margin: 8px 0;
}

.list-show-more {
  align-self: center;
  margin: 8px auto 0;
  display: block;
}

.input::placeholder {
  color: var(--text-dim);
}

.input:focus {
  border-color: rgba(122, 162, 247, 0.8);
  box-shadow: 0 0 0 2px rgba(122, 162, 247, 0.15);
}

.input:disabled {
  cursor: not-allowed;
  opacity: 0.45;
}

.input[type="number"] {
  appearance: textfield;
  -moz-appearance: textfield;
}

.input[type="number"]::-webkit-outer-spin-button,
.input[type="number"]::-webkit-inner-spin-button {
  margin: 0;
  appearance: none;
  -webkit-appearance: none;
}

.select {
  cursor: pointer;
}

.select option {
  background: var(--bg-primary);
  color: var(--text-primary);
}

.settings-grid .wide {
  grid-column: span 2;
}

.model-path-row {
  display: flex;
  gap: 8px;
}

.model-path-row .input {
  flex: 1;
  min-width: 0;
}

.toggle-row {
  align-items: flex-start;
  justify-content: center;
}

.settings-grid .toggle-row,
.inline-setting .toggle-row {
  flex-direction: row;
  align-items: center;
  justify-content: flex-start;
  min-height: 34px;
}

.toggle-row input {
  width: 15px;
  height: 15px;
  margin: 0;
  accent-color: var(--accent);
}

.inline-setting {
  padding-top: 10px;
  border-top: 1px solid var(--border);
}

.short-input {
  max-width: 120px;
}

.hint,
.status-line {
  margin-top: 10px;
}

.setting-row span,
.runtime-cell span,
.path-row span {
  display: block;
  color: var(--text-dim);
  font-size: 12px;
}

.segmented {
  display: inline-flex;
  padding: 3px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.segmented button {
  min-width: 42px;
  padding: 6px 10px;
  border: none;
  border-radius: 4px;
  background: transparent;
  color: var(--text-dim);
  cursor: pointer;
}

.segmented button.active {
  background: var(--bg-accent);
  color: var(--accent);
}

.runtime-grid {
  display: grid;
  grid-template-columns: repeat(4, minmax(120px, 1fr));
  gap: 8px;
}

.model-status-grid {
  display: grid;
  grid-template-columns: repeat(4, minmax(120px, 1fr));
  gap: 8px;
  margin-top: 12px;
}

.runtime-cell {
  min-height: 72px;
  padding: 11px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.runtime-cell strong {
  overflow: hidden;
  margin-top: 8px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.status-pill {
  border: 1px solid var(--border);
  border-radius: 999px;
  font-size: 11px;
  padding: 4px 8px;
}

.status-pill.ok {
  border-color: rgba(158, 206, 106, 0.45);
  color: var(--success);
}

.status-pill.warn {
  border-color: rgba(224, 175, 104, 0.45);
  color: var(--warning);
}

.model-candidates {
  margin-top: 10px;
  color: var(--text-dim);
  font-size: 12px;
}

.embedded-agent-list {
  margin-top: 10px;
  color: var(--text-dim);
  font-size: 12px;
}

.embedded-agent-list > strong {
  color: var(--text-primary);
}

.model-candidates summary {
  cursor: pointer;
}

.candidate-columns {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 12px;
  margin-top: 8px;
}

.candidate-path {
  display: grid;
  grid-template-columns: 28px minmax(0, 1fr);
  gap: 6px;
  align-items: center;
  margin-top: 5px;
}

.candidate-path code {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.ok-text {
  color: var(--success);
}

.dim-text {
  color: var(--text-dim);
}

.ping-line {
  margin-top: 12px;
}

.path-row {
  display: grid;
  grid-template-columns: 150px minmax(0, 1fr);
  gap: 12px;
  align-items: center;
  padding: 8px 0;
  border-top: 1px solid var(--border);
}

.path-row:first-of-type {
  border-top: none;
}

.remembered-patterns {
  margin-top: 12px;
}

.remembered-pattern-row {
  display: grid;
  grid-template-columns: minmax(160px, 1fr) 80px 50px minmax(0, 1.5fr) auto;
  gap: 10px;
  align-items: center;
  padding: 6px 8px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
  margin-bottom: 4px;
  font-size: 12px;
}

.remembered-pattern-row code {
  overflow: hidden;
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.remembered-pattern-label {
  display: flex;
  flex-direction: column;
  gap: 2px;
  min-width: 0;
}

.remembered-pattern-label strong {
  overflow: hidden;
  color: var(--text-primary);
  font-size: 12px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.debug-list {
  display: flex;
  max-height: 360px;
  flex-direction: column;
  gap: 6px;
  overflow-y: auto;
}

.kernel-learning {
  display: grid;
  gap: 10px;
  margin: 12px 0;
  padding: 10px;
  border: 1px solid color-mix(in srgb, var(--accent) 28%, var(--border));
  border-radius: 6px;
  background: color-mix(in srgb, var(--accent) 7%, var(--bg-secondary));
}

.kernel-learning-head,
.kernel-learning-step {
  display: flex;
  justify-content: space-between;
  gap: 10px;
}

.kernel-learning-head strong {
  color: var(--text-primary);
}

.kernel-learning-head span,
.kernel-learning-step span {
  color: var(--text-dim);
  font-size: 12px;
}

.kernel-learning-steps {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 8px;
}

.kernel-learning-step {
  min-height: 72px;
  flex-direction: column;
  justify-content: flex-start;
  padding: 8px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.kernel-learning-step strong {
  color: var(--text-primary);
  font-size: 12px;
}

.log-viewer {
  max-height: 320px;
  overflow: auto;
  padding: 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-secondary);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
  line-height: 1.45;
  white-space: pre-wrap;
}

.debug-row {
  padding: 9px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.debug-head {
  display: flex;
  justify-content: space-between;
  gap: 10px;
  margin-bottom: 5px;
}

.debug-head strong {
  color: var(--text-primary);
  font-size: 13px;
}

.debug-head span,
.debug-row p {
  color: var(--text-dim);
  font-size: 12px;
}

.debug-row p {
  margin-top: 5px;
}

.error {
  color: var(--error) !important;
}

code {
  overflow-wrap: anywhere;
  color: var(--text-secondary);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
}

.btn {
  padding: 8px 14px;
  border: none;
  border-radius: 6px;
  cursor: pointer;
  font-size: 13px;
  transition: all 0.15s;
}

.compact {
  padding: 5px 9px;
  font-size: 12px;
}

.btn:disabled {
  cursor: not-allowed;
  opacity: 0.45;
}

.btn-secondary {
  background: var(--bg-accent);
  color: var(--text-secondary);
}

.danger-action {
  color: var(--error);
}

.btn-primary {
  background: var(--accent);
  color: var(--bg-primary);
  font-weight: 600;
}

.btn-primary:hover:not(:disabled) {
  background: var(--accent-hover);
}

@media (max-width: 850px) {
  .runtime-grid,
  .model-status-grid,
  .candidate-columns,
  .path-row,
  .kernel-learning-steps,
  .settings-grid {
    grid-template-columns: 1fr;
  }

  .setting-row,
  .ping-line,
  .actions-row {
    align-items: stretch;
    flex-direction: column;
  }
}

.stealth-analysis {
  margin-top: 12px;
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.stealth-risk-line {
  display: flex;
  align-items: center;
  gap: 10px;
}

.risk-badge {
  padding: 3px 10px;
  border-radius: 999px;
  font-size: 12px;
  font-weight: 600;
  text-transform: uppercase;
  border: 1px solid var(--border);
}

.risk-badge.risk-low {
  color: var(--success);
  border-color: color-mix(in srgb, var(--success) 50%, var(--border));
}

.risk-badge.risk-medium {
  color: var(--warning);
  border-color: color-mix(in srgb, var(--warning) 50%, var(--border));
}

.risk-badge.risk-high {
  color: var(--error);
  border-color: color-mix(in srgb, var(--error) 50%, var(--border));
}

.stealth-threat-list,
.stealth-recommendation-list {
  margin: 0;
  padding-left: 18px;
  font-size: 13px;
  color: var(--text-secondary);
}

.stealth-threat-list li,
.stealth-recommendation-list li {
  margin-bottom: 4px;
}
</style>
