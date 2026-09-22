<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { useAppStore } from '@/stores/app'
import { useRiskGateStore } from '@/stores/riskGate'
import { backend } from '@/services/backend'

const { t } = useI18n()
const store = useAppStore()
const riskGate = useRiskGateStore()

const statusLabel = (status: string) => {
  if (status === 'ok') return t('modules.status.ok')
  if (status === 'missing') return t('modules.status.missing')
  if (status === 'provisional') return t('modules.status.provisional')
  if (status === 'blocked') return t('modules.status.blocked')
  return status
}

const statusClass = (status: string) => {
  if (status === 'ok') return 'ok'
  if (status === 'missing') return 'missing'
  if (status === 'provisional') return 'provisional'
  if (status === 'blocked') return 'blocked'
  return 'unknown'
}

const installLabel = (moduleId: string) => {
  switch (moduleId) {
    case 'lua_runtime': return t('modules.install.lua')
    case 'ai_model': return t('modules.install.model')
    case 'clr_inspector': return t('modules.install.clr')
    case 'kernel_driver': return t('modules.install.kernel')
    case 'edr_exclusion': return t('modules.install.edr')
    case 'debug_privilege': return t('modules.install.debugPriv')
    case 'stealth_sc2_profile': return t('modules.install.stealth')
    case 'handle_hider': return t('modules.install.handleHider')
    default: return t('modules.install.generic')
  }
}

const isInstallTarget = (moduleId: string) => {
  return store.moduleInstallBusy && store.moduleInstallModuleId === moduleId
}

// MODULES-V2 : état des diagnostics
const edrResult = ref<Record<string, unknown> | null>(null)
const debugPrivResult = ref<Record<string, unknown> | null>(null)
const edrBusy = ref(false)
const debugPrivBusy = ref(false)
const showEdrManualFix = ref(false)
const stealthBusy = ref(false)
const stealthResult = ref<Record<string, unknown> | null>(null)

// applyStealthMode/restoreStealthMode ne renvoient jamais de champ `message`
// (voir application_controller.cpp) : sur un succès sans warning, il n'y a ni
// message ni error, et le template affichait un encadré vide (bug UI signalé
// par l'utilisateur). Reconstruit un texte lisible à partir des champs
// réellement présents (profile/modulesActivated/restored/warnings/error).
function stealthResultText(result: Record<string, unknown> | null): string {
  if (!result) return ''
  if (result.error) return String(result.error)

  const parts: string[] = []
  if (result.restored) {
    parts.push(t('modules.stealth.restored'))
  } else if (result.profile) {
    const count = typeof result.modulesActivated === 'number' ? result.modulesActivated : undefined
    parts.push(count !== undefined
      ? t('modules.stealth.activeWithCount', { profile: String(result.profile), count })
      : t('modules.stealth.active', { profile: String(result.profile) }))
  } else if (result.success) {
    parts.push(t('modules.status.operationSuccess'))
  }
  if (Array.isArray(result.warnings) && result.warnings.length > 0) {
    parts.push(t('modules.status.warnings', { warnings: result.warnings.join(', ') }))
  }
  return parts.join(' ')
}
const handleHiderBusy = ref(false)
const handleHiderOwnerPid = ref('')
const handleHiderHandleValue = ref('')
const handleHiderResult = ref<Record<string, unknown> | null>(null)
const copiedBtn = ref<string | null>(null)

// Registre Defender (étapes 2/3 des solutions manuelles) : géré par le backend
// (élévation UAC + RiskGate), plus du simple copier-coller — état local car il
// n'y a pas d'appel "check" côté backend, seulement l'action elle-même.
const defenderDisabled = ref(false)
const defenderDisableBusy = ref(false)
const defenderDisableResult = ref<Record<string, unknown> | null>(null)
const behaviorMonitoringDisabled = ref(false)
const behaviorMonitoringBusy = ref(false)
const behaviorMonitoringResult = ref<Record<string, unknown> | null>(null)

// Test Signing (KillEngineKernel.sys n'est pas signé WHQL/EV — voir
// docs/KILLENGINE_KERNEL_DRIVER_ARCHITECTURE.md) : prérequis pour que le
// driver noyau se charge. État lu au montage (bcdedit, lecture seule, pas
// d'élévation), bascule via un thread élevé côté backend comme le toggle
// Defender ci-dessus.
const testSigningEnabled = ref(false)
const testSigningBusy = ref(false)
const testSigningResult = ref<Record<string, unknown> | null>(null)

async function refreshTestSigningStatus() {
  try {
    const result = await backend.getController().getTestSigningStatus?.()
    if (result?.success) {
      testSigningEnabled.value = Boolean(result.enabled)
    }
  } catch {
    // Best-effort : reste sur l'état par défaut (désactivé) en cas d'échec.
  }
}

async function toggleTestSigning() {
  const next = !testSigningEnabled.value
  const accepted = await riskGate.confirmRiskAction(
    'debug',
    next ? t('modules.kernel.confirmEnableTitle') : t('modules.kernel.confirmDisableTitle'),
    next ? t('modules.kernel.confirmEnableDesc') : t('modules.kernel.confirmDisableDesc'),
  )
  if (!accepted) return
  testSigningBusy.value = true
  testSigningResult.value = null
  try {
    const result = await backend.getController().setTestSigningEnabledAsync?.(next)
    if (!result?.started) {
      testSigningResult.value = result ?? { error: t('modules.errors.backendMissing') }
      testSigningBusy.value = false
    }
  } catch (e) {
    testSigningResult.value = { success: false, error: String(e) }
    testSigningBusy.value = false
  }
}

// setWindowsDefenderDisabledAsync/setDefenderBehaviorMonitoringDisabledAsync/
// addEdrExclusionAsync ne bloquent plus le thread GUI : elles démarrent
// l'élévation UAC + la commande sur un thread séparé côté backend et
// renvoient juste {started:true} immédiatement. Le vrai résultat arrive plus
// tard via un signal (voir onMounted) — busy reste true jusque-là.
async function toggleDefenderDisabled() {
  const next = !defenderDisabled.value
  const accepted = await riskGate.confirmRiskAction(
    'debug',
    next ? t('modules.edr.confirmDisableTitle') : t('modules.edr.confirmReenableTitle'),
    next ? t('modules.edr.confirmDisableDesc') : t('modules.edr.confirmReenableDesc'),
  )
  if (!accepted) return
  defenderDisableBusy.value = true
  defenderDisableResult.value = null
  try {
    const result = await backend.getController().setWindowsDefenderDisabledAsync?.(next)
    if (!result?.started) {
      // Échec immédiat (mock, ou erreur avant même de lancer le thread) — pas de signal à attendre.
      defenderDisableResult.value = result ?? { error: t('modules.errors.backendMissing') }
      defenderDisableBusy.value = false
    }
  } catch (e) {
    defenderDisableResult.value = { success: false, error: String(e) }
    defenderDisableBusy.value = false
  }
}

async function toggleBehaviorMonitoringDisabled() {
  const next = !behaviorMonitoringDisabled.value
  const accepted = await riskGate.confirmRiskAction(
    'debug',
    next ? t('modules.edr.confirmDisableBehaviorTitle') : t('modules.edr.confirmReenableBehaviorTitle'),
    next ? t('modules.edr.confirmDisableBehaviorDesc') : t('modules.edr.confirmReenableBehaviorDesc'),
  )
  if (!accepted) return
  behaviorMonitoringBusy.value = true
  behaviorMonitoringResult.value = null
  try {
    const result = await backend.getController().setDefenderBehaviorMonitoringDisabledAsync?.(next)
    if (!result?.started) {
      behaviorMonitoringResult.value = result ?? { error: t('modules.errors.backendMissing') }
      behaviorMonitoringBusy.value = false
    }
  } catch (e) {
    behaviorMonitoringResult.value = { success: false, error: String(e) }
    behaviorMonitoringBusy.value = false
  }
}

// Build directory path for exclusion commands
const buildDir = typeof window !== 'undefined' && window.location
  ? 'c:\\MES APPS DEV\\killengine\\build\\bin'
  : 'build\\bin'

function copyToClipboard(text: string, btnId: string) {
  navigator.clipboard.writeText(text).then(() => {
    copiedBtn.value = btnId
    setTimeout(() => { copiedBtn.value = null }, 1500)
  }).catch(() => {
    const ta = document.createElement('textarea')
    ta.value = text
    document.body.appendChild(ta)
    ta.select()
    document.execCommand('copy')
    document.body.removeChild(ta)
    copiedBtn.value = btnId
    setTimeout(() => { copiedBtn.value = null }, 1500)
  })
}

function copyEdrExclusionCmd() {
  copyToClipboard(`powershell -Command "Add-MpPreference -ExclusionPath '${buildDir}'"`, 'edr1')
}

function copyRegDisableSpyware() {
  copyToClipboard('reg add "HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows Defender" /v DisableAntiSpyware /t REG_DWORD /d 1 /f', 'edr2')
}

function copyRegDisableBehavior() {
  copyToClipboard('reg add "HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows Defender\\Real-Time Protection" /v DisableBehaviorMonitoring /t REG_DWORD /d 1 /f', 'edr3')
}

// Rafraîchir tous les diagnostics
async function refreshAllDiagnostics() {
  await Promise.all([runEdrCheck(), runDebugPrivCheck()])
  await store.refreshModuleCatalog()
}

async function runEdrCheck() {
  if (edrBusy.value) return
  edrBusy.value = true
  edrResult.value = null
  try {
    const result = await backend.getController().checkEdrBlocking?.()
    edrResult.value = result ?? { error: t('modules.errors.backendMissing') }
  } catch (e) {
    edrResult.value = { success: false, error: String(e) }
  } finally {
    edrBusy.value = false
  }
}

async function runDebugPrivCheck() {
  if (debugPrivBusy.value) return
  debugPrivBusy.value = true
  debugPrivResult.value = null
  try {
    const result = await backend.getController().checkDebugPrivilege?.()
    debugPrivResult.value = result ?? { error: t('modules.errors.backendMissing') }
  } catch (e) {
    debugPrivResult.value = { success: false, error: String(e) }
  } finally {
    debugPrivBusy.value = false
  }
}

// Retourne le statut effectif d'un module (priorité aux résultats de diagnostic)
function moduleEffectiveStatus(mod: { id: string; status: string }) {
  if (mod.id === 'debug_privilege' && debugPrivResult.value?.enabled) return 'ok'
  if (mod.id === 'edr_exclusion' && edrResult.value) {
    if (edrResult.value.blocked) return 'blocked'
    if (edrResult.value.provisional) return 'provisional' // Résultat testé mais pas fiable (rate limiting EDR)
    return 'ok'
  }
  return mod.status
}

async function handleInstall(modId: string) {
  if (modId === 'edr_exclusion') {
    if (!store.isAttached) {
      edrResult.value = { success: false, error: t('modules.errors.attachProcessToTest') }
      return
    }
    const accepted = await riskGate.confirmRiskAction('debug', t('modules.edr.confirmTitle'), t('modules.edr.confirmDesc'))
    if (!accepted) return
    await runEdrCheck()
    if (edrResult.value?.blocked) {
      const addAccepted = await riskGate.confirmRiskAction('debug', t('modules.edr.addExclusionTitle'), t('modules.edr.addExclusionDesc'))
      if (addAccepted) {
        edrBusy.value = true
        try {
          const result = await backend.getController().addEdrExclusionAsync?.('')
          if (!result?.started) {
            edrResult.value = result ?? { error: t('modules.errors.backendMissing') }
            edrBusy.value = false
          }
          // sinon : edrExclusionAddedFinished (voir onMounted) mettra à jour edrResult + edrBusy
        } catch (e) {
          edrResult.value = { success: false, error: String(e) }
          edrBusy.value = false
        }
      }
    }
    return
  }

  if (modId === 'debug_privilege') {
    const accepted = await riskGate.confirmRiskAction('debug', t('modules.debugPriv.confirmTitle'), t('modules.debugPriv.confirmDesc'))
    if (!accepted) return
    // Activer le privilège, pas juste vérifier
    debugPrivBusy.value = true
    debugPrivResult.value = null
    try {
      const result = await backend.getController().enableDebugPrivilege?.()
      debugPrivResult.value = result ?? { error: t('modules.errors.backendMissing') }
    } catch (e) {
      debugPrivResult.value = { success: false, error: String(e) }
    } finally {
      debugPrivBusy.value = false
    }
    // enableDebugPrivilege() ne renvoie pas de champ `enabled` (juste success/message) —
    // re-vérifier via checkDebugPrivilege() pour que le badge reflète l'état réel sans
    // attendre un retour sur cette vue.
    await runDebugPrivCheck()
    return
  }

  if (modId === 'stealth_sc2_profile') {
    // Réutilise store.applyStealthMode (app.ts) au lieu d'appeler le backend
    // directement : cette action gère déjà la confirmation RiskGate ET
    // rafraîchit store.stealthStatus après coup — sans ça, le bouton
    // "Restaurer" ci-dessous (qui lit store.stealthStatus?.active) ne
    // s'affichait jamais après un "Appliquer" réussi depuis cet écran.
    stealthBusy.value = true
    try {
      const result = await store.applyStealthMode('sc2')
      if (result) {
        stealthResult.value = result as unknown as Record<string, unknown>
        if (result.success) {
          await store.refreshModuleCatalog()
        }
      }
    } catch (e) {
      stealthResult.value = { success: false, error: String(e) }
    } finally {
      stealthBusy.value = false
    }
    return
  }

  if (modId === 'restore_stealth') {
    stealthBusy.value = true
    try {
      const result = await store.restoreStealthMode()
      if (result) {
        stealthResult.value = result as unknown as Record<string, unknown>
        if (result.success) {
          await store.refreshModuleCatalog()
        }
      }
    } catch (e) {
      stealthResult.value = { success: false, error: String(e) }
    } finally {
      stealthBusy.value = false
    }
    return
  }

  if (modId === 'handle_hider') {
    // handleHiderOwnerPid est lié à un <input type="number"> : Vue convertit
    // automatiquement sa valeur en Number dès qu'on interagit avec (y compris
    // les flèches ↑↓ natives du champ), même si le ref est initialisé comme
    // une chaîne vide — .trim() plante sur un Number (bug UI signalé par
    // l'utilisateur : bandeau d'erreur fatale rouge). String(...) le protège
    // dans les deux cas.
    if (!String(handleHiderOwnerPid.value).trim() || !handleHiderHandleValue.value.trim()) {
      handleHiderResult.value = { success: false, error: 'Remplis le PID et la valeur du handle.' }
      return
    }
    handleHiderBusy.value = true
    handleHiderResult.value = null
    try {
      const ownerPid = parseInt(handleHiderOwnerPid.value, 10)
      const handleValue = parseInt(handleHiderHandleValue.value, 16) || parseInt(handleHiderHandleValue.value, 10)
      const result = await backend.getController().hideHandle?.(ownerPid, handleValue)
      handleHiderResult.value = result ?? { error: t('modules.errors.backendMissing') }
    } catch (e) {
      handleHiderResult.value = { success: false, error: String(e) }
    } finally {
      handleHiderBusy.value = false
    }
    return
  }

  // PORT-5 : lua_runtime/clr_inspector s'installent depuis une archive locale
  // choisie via un sélecteur de fichier natif, pas un simple clic (voir
  // docs/PORTABILITY_ROADMAP.md#port-5).
  const catalogEntry = store.moduleCatalog.find((m) => m.id === modId)
  if (catalogEntry?.installKind === 'archive') {
    store.browseAndInstallModuleFromArchive(modId)
    return
  }

  // Modules classiques (ai_model, kernel_driver)
  store.installModule(modId)
}

onMounted(() => {
  void store.refreshModuleCatalog()

  // Résultats différés des actions Defender/EDR non bloquantes (voir
  // addEdrExclusionAsync/setWindowsDefenderDisabledAsync/
  // setDefenderBehaviorMonitoringDisabledAsync) — le backend démarre le
  // travail sur un thread séparé et notifie ici une fois terminé.
  const controller = backend.getController()
  controller.edrExclusionAddedFinished?.connect((result: Record<string, unknown>) => {
    edrResult.value = result
    edrBusy.value = false
  })
  controller.windowsDefenderDisabledFinished?.connect((result: Record<string, unknown>) => {
    defenderDisableResult.value = result
    if (result?.success) defenderDisabled.value = Boolean(result.disabled)
    defenderDisableBusy.value = false
  })
  controller.defenderBehaviorMonitoringDisabledFinished?.connect((result: Record<string, unknown>) => {
    behaviorMonitoringResult.value = result
    if (result?.success) behaviorMonitoringDisabled.value = Boolean(result.disabled)
    behaviorMonitoringBusy.value = false
  })
  controller.testSigningEnabledFinished?.connect((result: Record<string, unknown>) => {
    testSigningResult.value = result
    if (result?.success) testSigningEnabled.value = Boolean(result.enabled)
    testSigningBusy.value = false
  })
  void refreshTestSigningStatus()
})
</script>

<template>
  <div class="modules-view">
    <div class="header">
      <div>
        <h1>{{ $t('nav.modules') }}</h1>
        <p>{{ $t('modules.intro') }}</p>
      </div>
      <button
        class="btn btn-secondary refresh-btn"
        :disabled="store.moduleCatalogBusy || store.moduleInstallBusy || edrBusy || debugPrivBusy"
        @click="refreshAllDiagnostics()"
      >
        {{ (store.moduleCatalogBusy || edrBusy || debugPrivBusy) ? $t('modules.refreshing') : $t('modules.refreshAll') }}
      </button>
    </div>

    <div v-if="store.moduleCatalog.length === 0 && !store.moduleCatalogBusy" class="empty">
      {{ $t('modules.empty') }}
    </div>

    <!-- Section 1 : Dépendances -->
    <section class="module-section">
      <h2>{{ $t('modules.sections.dependencies') }}</h2>
      <div v-for="mod in store.moduleCatalog.filter(m => !m.section || m.section === 'dependencies')" :key="mod.id" class="module-card" :class="{ busy: isInstallTarget(mod.id) }">
        <div class="module-head">
          <div class="module-title">
            <span class="module-name">{{ mod.displayName }}</span>
            <span class="module-status" :class="statusClass(moduleEffectiveStatus(mod))">{{ statusLabel(moduleEffectiveStatus(mod)) }}</span>
          </div>
          <button
            v-if="mod.installable && !store.moduleInstallBusy"
            class="btn btn-primary install-btn"
            @click="handleInstall(mod.id)"
          >
            {{ installLabel(mod.id) }}
          </button>
        </div>
        <p class="module-desc">{{ mod.description }}</p>
        <p v-if="mod.detail" class="module-detail">{{ mod.detail }}</p>
        <p v-if="mod.path" class="module-path">{{ mod.path }}</p>

        <div v-if="isInstallTarget(mod.id)" class="install-progress">
          <div class="progress-bar">
            <div class="progress-fill" />
          </div>
          <p class="progress-text">{{ store.moduleInstallProgress || $t('modules.progress') }}</p>
          <button class="btn btn-secondary cancel-btn" @click="store.cancelModuleInstall()">
            {{ $t('modules.cancel') }}
          </button>
        </div>

        <!-- Prérequis Test Signing pour le driver noyau (non signé WHQL/EV) -->
        <div v-if="mod.id === 'kernel_driver'" class="edr-guide">
          <p class="guide-title">📖 {{ $t('modules.kernel.guideTitle') }}</p>
          <p>{{ $t('modules.kernel.guideDesc') }}</p>
          <div class="guide-step">
            <strong>{{ $t('modules.kernel.testSigningLabel') }}</strong>
            <span class="module-status" :class="testSigningEnabled ? 'ok' : 'missing'">
              {{ testSigningEnabled ? $t('settings.enabled') : $t('settings.disabled') }}
            </span>
            <button
              class="btn compact"
              :class="testSigningEnabled ? 'btn-secondary' : 'btn-primary'"
              :disabled="testSigningBusy"
              @click="toggleTestSigning()"
            >
              {{ testSigningBusy ? '...' : (testSigningEnabled ? $t('modules.kernel.disable') : $t('modules.kernel.enable')) }}
            </button>
          </div>
          <p v-if="testSigningResult?.message" class="status-line">{{ testSigningResult.message }}</p>
          <p v-else-if="testSigningResult?.cancelled" class="warning">{{ $t('settings.elevationRefused') }}</p>
          <p v-else-if="testSigningResult?.error" class="error">{{ testSigningResult.error }}</p>
          <p class="manual-warning">{{ $t('modules.kernel.rebootWarning') }}</p>
        </div>
      </div>
    </section>

    <!-- Section 2 : Environnement de test -->
    <section class="module-section">
      <h2>🔧 {{ $t('modules.sections.testEnv') }}</h2>
      <div v-for="mod in store.moduleCatalog.filter(m => m.section === 'test_env')" :key="mod.id" class="module-card" :class="{ busy: isInstallTarget(mod.id) }">
        <div class="module-head">
          <div class="module-title">
            <span class="module-name">{{ mod.displayName }}</span>
            <span class="module-status" :class="statusClass(moduleEffectiveStatus(mod))">{{ statusLabel(moduleEffectiveStatus(mod)) }}</span>
          </div>
          <div class="module-actions">
            <button
              v-if="mod.id === 'edr_exclusion' && !edrBusy"
              class="btn btn-secondary check-btn"
              :disabled="!store.isAttached"
              :title="!store.isAttached ? $t('modules.edr.attachToTest') : ''"
              @click="runEdrCheck()"
            >
              {{ $t('modules.edr.check') }}
            </button>
            <button
              v-if="mod.id === 'debug_privilege' && !debugPrivBusy"
              class="btn btn-secondary check-btn"
              @click="runDebugPrivCheck()"
            >
              {{ $t('modules.debugPriv.check') }}
            </button>
          </div>
        </div>
        <p class="module-desc">{{ mod.description }}</p>
        <p v-if="mod.detail" class="module-detail">{{ mod.detail }}</p>

        <!-- Guide EDR / Tamper Protection -->
        <div v-if="mod.id === 'edr_exclusion'" class="edr-guide">
          <p class="guide-title">📖 {{ $t('modules.edr.guide.title') }}</p>
          <div class="guide-step">
            <strong>{{ $t('modules.edr.guide.step1Title') }}</strong>
            <p>{{ $t('modules.edr.guide.step1Desc') }}</p>
            <p class="guide-action">{{ $t('modules.edr.guide.step1ActionPrefix') }} <strong>{{ $t('modules.edr.addExclusion') }}</strong> {{ $t('modules.edr.guide.step1ActionSuffix') }}</p>
            <pre class="manual-code">powershell -Command "Add-MpPreference -ExclusionPath '{{ buildDir }}'"</pre>
          </div>
          <div class="guide-step">
            <strong>{{ $t('modules.edr.guide.step2Title') }}</strong>
            <p>{{ $t('modules.edr.guide.step2Desc') }}</p>
            <ol class="guide-list">
              <li>{{ $t('modules.edr.guide.step2List1Prefix') }} <strong>Windows Security</strong> {{ $t('modules.edr.guide.step2List1Suffix') }}</li>
              <li>{{ $t('modules.edr.guide.step2List2Prefix') }} <strong>{{ $t('modules.edr.guide.threatSettings') }}</strong></li>
              <li>{{ $t('modules.edr.guide.step2List3Prefix') }} <strong>Tamper Protection</strong></li>
              <li>{{ $t('modules.edr.guide.step2List4Prefix') }} <strong>{{ $t('modules.edr.execute') }}</strong> {{ $t('modules.edr.guide.step2List4Suffix') }}</li>
              <li><strong>{{ $t('modules.edr.guide.restartWindows') }}</strong> {{ $t('modules.edr.guide.step2List5Suffix') }}</li>
            </ol>
          </div>
          <div class="guide-step">
            <strong>{{ $t('modules.edr.guide.step3Title') }}</strong>
            <p>{{ $t('modules.edr.guide.step3Desc') }}</p>
            <p class="guide-action">{{ $t('modules.edr.guide.step3ActionPrefix') }} <strong>{{ $t('modules.edr.reactivate') }}</strong> {{ $t('modules.edr.guide.step3ActionSuffix') }}</p>
          </div>
          <div class="guide-step">
            <strong>{{ $t('modules.edr.guide.manualSearchTitle') }}</strong>
            <p>{{ $t('modules.edr.guide.manualSearchDesc') }}</p>
          </div>
        </div>

        <!-- Résultats de diagnostic -->
        <div v-if="mod.id === 'edr_exclusion' && edrResult" class="diag-result" :class="edrResult.blocked ? 'blocked' : (edrResult.provisional ? 'provisional' : 'ok')">
          <p v-if="edrResult.attemptCount" class="attempt-count">{{ $t('modules.edr.attempt', { count: edrResult.attemptCount }) }}</p>
          <p>{{ String(edrResult.message ?? edrResult.error ?? '') }}</p>
          <div v-if="edrResult.blocked || edrResult.provisional" class="edr-fix-actions">
            <button
              class="btn btn-primary fix-btn"
              @click="handleInstall('edr_exclusion')"
            >
              {{ edrResult.provisional ? $t('modules.edr.addExclusionPreventive') : $t('modules.edr.addExclusion') }}
            </button>
            <button
              class="btn btn-secondary fix-btn"
              @click="showEdrManualFix = !showEdrManualFix"
            >
              {{ showEdrManualFix ? $t('modules.edr.hideManual') : $t('modules.edr.showManual') }}
            </button>
          </div>
          <!-- Solutions manuelles (clés registre + PowerShell) -->
          <div v-if="edrResult.blocked && showEdrManualFix" class="edr-manual-fix">
            <h4>{{ $t('modules.edr.manualTitle') }}</h4>
            <div class="manual-step">
              <strong>{{ $t('modules.edr.step1Title') }}</strong>
              <p>{{ $t('modules.edr.step1Desc') }}</p>
              <pre class="manual-code">powershell -Command "Add-MpPreference -ExclusionPath '{{ buildDir }}'"</pre>
              <button class="btn btn-secondary compact" @click="copyEdrExclusionCmd" :title="$t('modules.edr.copy')">
                {{ $t('modules.edr.copy') }}
              </button>
            </div>
            <div class="manual-step">
              <strong>{{ $t('modules.edr.step2Title') }}</strong>
              <p>{{ $t('modules.edr.step2Desc') }}</p>
              <pre class="manual-code">reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows Defender" /v DisableAntiSpyware /t REG_DWORD /d 1 /f</pre>
              <div class="manual-step-actions">
                <button class="btn btn-secondary compact" @click="copyRegDisableSpyware" :title="$t('modules.edr.copy')">
                  {{ $t('modules.edr.copy') }}
                </button>
                <button
                  class="btn compact"
                  :class="defenderDisabled ? 'btn-secondary' : 'btn-primary'"
                  :disabled="defenderDisableBusy"
                  @click="toggleDefenderDisabled()"
                >
                  {{ defenderDisableBusy ? '...' : (defenderDisabled ? $t('modules.edr.reactivate') : $t('modules.edr.execute')) }}
                </button>
              </div>
              <p v-if="defenderDisableResult" class="manual-step-result" :class="defenderDisableResult.success ? 'warning' : 'error'">
                {{ String(defenderDisableResult.message ?? defenderDisableResult.error ?? '') }}
              </p>
            </div>
            <div class="manual-step">
              <strong>{{ $t('modules.edr.step3Title') }}</strong>
              <p>{{ $t('modules.edr.step3Desc') }}</p>
              <pre class="manual-code">reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows Defender\Real-Time Protection" /v DisableBehaviorMonitoring /t REG_DWORD /d 1 /f</pre>
              <div class="manual-step-actions">
                <button class="btn btn-secondary compact" @click="copyRegDisableBehavior" :title="$t('modules.edr.copy')">
                  {{ $t('modules.edr.copy') }}
                </button>
                <button
                  class="btn compact"
                  :class="behaviorMonitoringDisabled ? 'btn-secondary' : 'btn-primary'"
                  :disabled="behaviorMonitoringBusy"
                  @click="toggleBehaviorMonitoringDisabled()"
                >
                  {{ behaviorMonitoringBusy ? '...' : (behaviorMonitoringDisabled ? $t('modules.edr.reactivate') : $t('modules.edr.execute')) }}
                </button>
              </div>
              <p v-if="behaviorMonitoringResult" class="manual-step-result" :class="behaviorMonitoringResult.success ? 'warning' : 'error'">
                {{ String(behaviorMonitoringResult.message ?? behaviorMonitoringResult.error ?? '') }}
              </p>
            </div>
            <p class="manual-warning">{{ $t('modules.edr.manualWarning') }}</p>
          </div>
        </div>
        <div v-if="mod.id === 'debug_privilege' && debugPrivResult" class="diag-result" :class="debugPrivResult.enabled ? 'ok' : 'disabled'">
          <p>{{ String(debugPrivResult.message ?? debugPrivResult.error ?? '') }}</p>
          <button
            v-if="!debugPrivResult.enabled"
            class="btn btn-primary fix-btn"
            @click="handleInstall('debug_privilege')"
          >
            {{ $t('modules.debugPriv.enable') }}
          </button>
        </div>
      </div>
    </section>

    <!-- Section 3 : Sécurité / Stealth -->
    <section class="module-section">
      <h2>🛡️ {{ $t('modules.sections.stealth') }}</h2>
      <div v-for="mod in store.moduleCatalog.filter(m => m.section === 'stealth')" :key="mod.id" class="module-card" :class="{ busy: isInstallTarget(mod.id) }">
        <div class="module-head">
          <div class="module-title">
            <span class="module-name">{{ mod.displayName }}</span>
            <span class="module-status" :class="statusClass(moduleEffectiveStatus(mod))">{{ statusLabel(moduleEffectiveStatus(mod)) }}</span>
          </div>
          <div class="module-actions">
            <button
              v-if="mod.id === 'stealth_sc2_profile' && !stealthBusy"
              class="btn btn-primary install-btn"
              @click="handleInstall(mod.id)"
            >
              {{ installLabel(mod.id) }}
            </button>
            <button
              v-if="mod.id === 'stealth_sc2_profile' && store.stealthStatus?.active && !stealthBusy"
              class="btn btn-secondary install-btn"
              @click="handleInstall('restore_stealth')"
            >
              {{ $t('modules.install.restore') }}
            </button>
          </div>
        </div>
        <p class="module-desc">{{ mod.description }}</p>
        <p v-if="mod.detail" class="module-detail">{{ mod.detail }}</p>

        <!-- Résultat Appliquer/Restaurer Stealth -->
        <div v-if="mod.id === 'stealth_sc2_profile' && stealthResult" class="diag-result" :class="stealthResult.success ? 'ok' : 'blocked'">
          <p>{{ stealthResultText(stealthResult) }}</p>
        </div>

        <!-- Handle Hider UI -->
        <div v-if="mod.id === 'handle_hider'" class="handle-hider-ui">
          <div class="handle-hider-inputs">
            <input v-model="handleHiderOwnerPid" class="input compact-input" :placeholder="$t('modules.handleHider.ownerPid')" type="number" min="0" />
            <input v-model="handleHiderHandleValue" class="input compact-input" :placeholder="$t('modules.handleHider.handleValue')" />
            <button class="btn btn-primary compact" :disabled="handleHiderBusy || !String(handleHiderOwnerPid).trim() || !handleHiderHandleValue.trim()" @click="handleInstall('handle_hider')">
              {{ handleHiderBusy ? $t('modules.handleHider.hiding') : $t('modules.handleHider.hide') }}
            </button>
          </div>
          <div v-if="handleHiderResult" class="diag-result" :class="handleHiderResult.success ? 'ok' : 'blocked'">
            <p>{{ String(handleHiderResult.message ?? handleHiderResult.error ?? '') }}</p>
          </div>
        </div>
      </div>
    </section>

    <!-- Résultat global d'installation -->
    <div
      v-if="store.moduleInstallResult && !store.moduleInstallBusy"
      class="install-result"
      :class="store.moduleInstallResult.success ? 'success' : 'error'"
    >
      <p>{{ String(store.moduleInstallResult.message ?? store.moduleInstallResult.error ?? '') }}</p>
      <button class="btn btn-secondary" @click="store.refreshModuleCatalog()">
        {{ $t('modules.refresh') }}
      </button>
    </div>
  </div>
</template>

<style scoped>
.modules-view {
  padding: 24px 32px;
  max-width: 860px;
}

.header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 16px;
  margin-bottom: 20px;
}

.header h1 {
  font-size: 22px;
  color: var(--text-primary);
  margin: 0 0 6px;
}

.header p {
  margin: 0;
  color: var(--text-secondary);
  font-size: 13px;
  max-width: 640px;
}

.refresh-btn {
  flex-shrink: 0;
  border: 1px solid var(--success);
  background: color-mix(in srgb, var(--success) 18%, var(--bg-accent));
  color: var(--success);
  font-weight: 600;
}

.refresh-btn:hover:not(:disabled) {
  background: color-mix(in srgb, var(--success) 30%, var(--bg-accent));
  border-color: var(--success);
}

.empty {
  padding: 24px;
  border: 1px dashed var(--border);
  border-radius: 6px;
  color: var(--text-dim);
  text-align: center;
}

.module-section {
  margin-bottom: 28px;
}

.module-section h2 {
  font-size: 16px;
  color: var(--text-primary);
  margin: 0 0 12px;
  padding-bottom: 6px;
  border-bottom: 1px solid var(--border);
}

.module-card {
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
  padding: 14px 16px;
  margin-bottom: 12px;
}

.module-card.busy {
  border-color: color-mix(in srgb, var(--accent) 50%, var(--border));
}

.module-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.module-title {
  display: flex;
  align-items: center;
  gap: 10px;
}

.module-name {
  font-weight: 600;
  color: var(--text-primary);
}

.module-status {
  font-size: 11px;
  font-weight: 600;
  padding: 2px 8px;
  border-radius: 10px;
  border: 1px solid var(--border);
}

.module-status.ok {
  color: var(--success);
  border-color: color-mix(in srgb, var(--success) 50%, var(--border));
}

.module-status.provisional {
  color: var(--warning);
  border-color: color-mix(in srgb, var(--warning) 50%, var(--border));
  background: color-mix(in srgb, var(--warning) 10%, transparent);
}

.module-status.blocked {
  color: var(--danger);
  border-color: color-mix(in srgb, var(--danger) 50%, var(--border));
  background: color-mix(in srgb, var(--danger) 10%, transparent);
}

.module-status.missing {
  color: var(--warning);
  border-color: color-mix(in srgb, var(--warning) 50%, var(--border));
}

.module-desc {
  margin: 10px 0 0;
  font-size: 13px;
  color: var(--text-secondary);
}

.module-detail {
  margin: 8px 0 0;
  font-size: 12px;
  color: var(--text-dim);
}

.module-path {
  margin: 6px 0 0;
  font-size: 11px;
  color: var(--text-dim);
  font-family: 'Consolas', monospace;
  word-break: break-all;
}

.module-actions {
  display: flex;
  gap: 8px;
}

.check-btn {
  font-size: 12px;
  padding: 4px 10px;
}

.install-btn {
  font-size: 12px;
  padding: 4px 10px;
}

.fix-btn {
  font-size: 12px;
  padding: 4px 10px;
}

.cancel-btn {
  font-size: 12px;
  padding: 4px 10px;
}

.diag-result {
  margin-top: 12px;
  padding: 10px 12px;
  border-radius: 6px;
  border: 1px solid var(--border);
}

.diag-result .attempt-count {
  font-size: 11px;
  color: var(--text-dim);
  margin: 0 0 4px;
  font-weight: 600;
}

.diag-result p {
  margin: 0 0 8px;
  font-size: 12px;
}

.diag-result.ok {
  border-color: color-mix(in srgb, var(--success) 40%, var(--border));
  color: var(--success);
}

.diag-result.provisional {
  border-color: color-mix(in srgb, var(--warning) 40%, var(--border));
  background: color-mix(in srgb, var(--warning) 8%, transparent);
  color: var(--warning);
}

.diag-result.blocked {
  border-color: color-mix(in srgb, var(--error) 40%, var(--border));
  background: color-mix(in srgb, var(--error) 8%, transparent);
  color: var(--error);
}

.diag-result.disabled {
  border-color: color-mix(in srgb, var(--warning) 40%, var(--border));
  color: var(--warning);
}

.edr-guide {
  margin-top: 12px;
  padding: 12px;
  border: 1px solid color-mix(in srgb, var(--warning) 30%, var(--border));
  border-radius: 6px;
  background: color-mix(in srgb, var(--warning) 5%, var(--bg-secondary));
}

.edr-guide .guide-title {
  margin: 0 0 10px;
  color: var(--warning);
  font-size: 13px;
  font-weight: 600;
}

.edr-guide .guide-step {
  margin-bottom: 10px;
  padding-bottom: 10px;
  border-bottom: 1px solid var(--border);
}

.edr-guide .guide-step:last-child {
  border-bottom: none;
  margin-bottom: 0;
  padding-bottom: 0;
}

.edr-guide .guide-step strong {
  color: var(--text-primary);
  font-size: 12px;
}

.edr-guide .guide-step p {
  margin: 4px 0;
  font-size: 12px;
  color: var(--text-secondary);
}

.edr-guide .guide-action {
  color: var(--text-primary);
  font-size: 12px;
  font-weight: 500;
}

.edr-guide .guide-list {
  margin: 4px 0;
  padding-left: 20px;
  font-size: 12px;
  color: var(--text-secondary);
}

.edr-manual-fix {
  margin-top: 10px;
  padding-top: 10px;
  border-top: 1px solid var(--border);
}

.edr-manual-fix h4 {
  margin: 0 0 8px;
  font-size: 12px;
  color: var(--text-primary);
}

.manual-step {
  margin-bottom: 12px;
}

.manual-step strong {
  display: block;
  font-size: 12px;
  color: var(--text-primary);
  margin-bottom: 2px;
}

.manual-step p {
  margin: 0 0 6px;
  font-size: 11px;
  color: var(--text-secondary);
}

.manual-code {
  display: block;
  margin: 0 0 6px;
  padding: 8px 10px;
  border-radius: 6px;
  border: 1px solid var(--border);
  background: var(--bg-primary);
  color: var(--text-dim);
  font-family: 'Cascadia Code', 'Fira Code', monospace;
  font-size: 11px;
  white-space: pre-wrap;
  word-break: break-all;
  overflow-wrap: anywhere;
}

.manual-step-actions {
  display: flex;
  gap: 8px;
}

.manual-step-result {
  margin: 6px 0 0;
  font-size: 11px;
}

.manual-step-result.ok {
  color: var(--success);
}

.manual-step-result.warning {
  color: var(--warning);
}

.manual-step-result.error {
  color: var(--error);
}

.manual-warning {
  margin-top: 4px;
  font-size: 11px;
  color: var(--warning);
}

.handle-hider-ui {
  margin-top: 12px;
}

.handle-hider-inputs {
  display: flex;
  gap: 8px;
  align-items: center;
  flex-wrap: wrap;
}

.handle-hider-inputs .input {
  flex: 1;
  min-width: 100px;
}

/* Flèches natives du champ PID (type="number") : moches, non thémables de
   façon fiable entre navigateurs, et leur clic a déjà déclenché un vrai crash
   (voir le fix .trim()) — même choix que SettingsView.vue, un champ nombre
   propre sans spinner plutôt qu'un spinner customisé. */
.handle-hider-inputs input[type="number"] {
  appearance: textfield;
  -moz-appearance: textfield;
}

.handle-hider-inputs input[type="number"]::-webkit-outer-spin-button,
.handle-hider-inputs input[type="number"]::-webkit-inner-spin-button {
  margin: 0;
  appearance: none;
  -webkit-appearance: none;
}

.copied-feedback {
  color: var(--success);
  font-size: 11px;
  margin-left: 4px;
}

.install-progress {
  margin-top: 12px;
}

.progress-bar {
  height: 6px;
  border-radius: 3px;
  background: var(--bg-secondary);
  overflow: hidden;
}

.progress-fill {
  height: 100%;
  width: 40%;
  background: var(--accent);
  animation: module-progress 1.4s ease-in-out infinite alternate;
}

@keyframes module-progress {
  from { width: 15%; }
  to { width: 85%; }
}

.progress-text {
  margin: 8px 0;
  font-size: 12px;
  color: var(--text-secondary);
  font-family: 'Consolas', monospace;
  word-break: break-all;
}

.install-result {
  margin-top: 16px;
  padding: 12px 14px;
  border-radius: 6px;
  border: 1px solid var(--border);
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.install-result p {
  margin: 0;
  font-size: 13px;
  word-break: break-word;
}

.install-result.success {
  border-color: color-mix(in srgb, var(--success) 40%, var(--border));
  color: var(--success);
}

.install-result.error {
  border-color: color-mix(in srgb, var(--error) 40%, var(--border));
  color: var(--error);
}
</style>
