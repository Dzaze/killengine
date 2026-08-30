/**
 * KillEngine — store Write / Freeze / Checkpoint (extrait de app.ts,
 * candidat S7 de docs/REFACTOR_ROADMAP.md, PHASE 229, 30/08/2026).
 *
 * S7 est annoté couplage "Haut" avec S8 (Trainer)/S9 (Profils/Session) dans
 * le roadmap — vérifié en pratique : la mécanique d'écriture/freeze/rollback
 * elle-même est isolable, mais quasiment chaque fonction retombe sur des
 * domaines pas encore extraits et volontairement non touchés ici (Session/
 * Trainer via `upsertSessionEntry`/`markSessionEntryEnabled`, Watch via
 * `addAddressToWatch`/`refreshWatchedAddress`, Chat/SmartSearch via
 * `pushMessage`/`refreshActiveChatMemoryTargets`/`refreshSmartSearchContext`,
 * Memory/kernel-mode via `writeMemoryValueByMode`/`kernelMemoryModeActive`,
 * audit via `logAiAudit`, RiskGate via `confirmRiskAction`). Plutôt que de
 * dupliquer ou de refactorer ces domaines (interdit par la consigne de ce
 * chantier), ils sont injectés UNE SEULE FOIS via `configureWriteFreezeContext`
 * (même patron "réglé une fois" que `configureCandidateWatchNotifier` dans
 * `scanning.ts`), appelé depuis `app.ts` juste après la création de ce store.
 *
 * `exactScanType`/`exactScanValue`/`selectedCandidateAddress`/`scanStatusText`
 * viennent directement de `scanning.ts` (déjà une feuille sans dépendance
 * vers `./app`) — dépendance store-vers-store à sens unique, pas circulaire.
 * `addActionLog`/`addInvestigationStep` sont importés directement depuis les
 * fondations `actionLog.ts`/`investigation.ts`, même principe.
 *
 * `checkpointAddress`/`checkpointType`/`checkpointValue`/
 * `buildCheckpointActionPlan` sont aussi partagés par des exécuteurs de
 * checkpoint qui restent dans `app.ts` (Find What Writes/AOB/disassemble/
 * forcer valeur — domaine debug/patch, pas write/freeze) : `app.ts` les
 * relit depuis ce store plutôt que de dupliquer ces helpers purs.
 */
import { defineStore, storeToRefs } from 'pinia'
import { computed, ref, watch } from 'vue'
import {
  backend,
  type AtomicWriteTarget,
  type MemoryWriteBatchResult,
  type MemoryWriteResult,
  type MemoryWriteTarget,
} from '@/services/backend'
import { useActionLogStore } from './actionLog'
import { useInvestigationStore } from './investigation'
import { useScanningStore } from './scanning'

export interface RuntimeActionPlanItem {
  id: 'watch' | 'write' | 'freeze_polling' | 'find_writes' | 'aob_patch' | 'force_value' | 'bookmark' | 'trainer'
  label: string
  risk: 'safe' | 'write' | 'debug' | 'patch'
  enabled: boolean
  reason: string
}

export interface RuntimeActionPlan {
  label: string
  address: string
  type: string
  value: string
  kind: string
  isCode: boolean
  safeCount: number
  riskyCount: number
  actions: RuntimeActionPlanItem[]
}

type SessionEntryKind = 'freeze_polling' | 'freeze_breakpoint' | 'write'
type RiskLevel = 'safe' | 'write' | 'debug' | 'patch' | 'injection'

export interface WriteFreezeExternalDeps {
  confirmRiskAction: (risk: RiskLevel, title: string, detail: string) => Promise<boolean>
  logAiAudit: (event: string, payload: Record<string, unknown>) => void
  addAddressToWatch: (address: string, type?: string) => void
  upsertSessionEntry: (address: string, valueType: string, kind: SessionEntryKind, enabled?: boolean) => void
  markSessionEntryEnabled: (address: string, valueType: string, kind: SessionEntryKind, enabled: boolean) => void
  writeMemoryValueByMode: (address: string, type: string, value: string) => Promise<MemoryWriteResult>
  kernelMemoryModeActive: { readonly value: boolean }
  pushMessage: (role: 'user' | 'assistant', text: string, extras?: { isError?: boolean }) => void
  refreshWatchedAddress: (address: string) => Promise<{ value: string, error: string } | null>
  refreshActiveChatMemoryTargets: () => Promise<void>
  refreshSmartSearchContext: () => Promise<void>
  regionForAddress: (address: string) => Record<string, unknown> | undefined
}

export const useWriteFreezeStore = defineStore('writeFreeze', () => {
  const actionLogStore = useActionLogStore()
  const { addActionLog } = actionLogStore
  const investigationStore = useInvestigationStore()
  const { addInvestigationStep } = investigationStore
  const scanningStore = useScanningStore()
  const { exactScanType, exactScanValue, selectedCandidateAddress, scanStatusText } = storeToRefs(scanningStore)

  // Dépendances transversales pas encore extraites (voir commentaire d'en-tête) --
  // null tant qu'app.ts n'a pas appelé configureWriteFreezeContext (usage isolé
  // de ce store, ex. tests, resterait fonctionnel pour les chemins qui ne les
  // utilisent pas, juste sans ces effets de bord).
  let externalDeps: WriteFreezeExternalDeps | null = null
  function configureWriteFreezeContext(newDeps: WriteFreezeExternalDeps) {
    externalDeps = newDeps
  }
  function deps(): WriteFreezeExternalDeps {
    if (!externalDeps) throw new Error('writeFreeze store used before configureWriteFreezeContext')
    return externalDeps
  }

  const writeValue = ref('')
  const writeResult = ref<MemoryWriteResult | null>(null)
  const writeSafetyWarning = ref('')
  const writeSafetyAcknowledged = ref(false)
  const freezeEnabled = ref(false)
  const breakpointFreezeEnabled = ref(false)
  const freezeIntervalMs = ref(100)
  const freezeIntervalResult = ref<Record<string, unknown> | null>(null)
  // Sequence ordonnee (ordre + doublons conserves) des dernieres ecritures
  // confirmees, persistee par executable — roadmap I, replay inter-session.
  const writeHistorySequence = ref<Array<Record<string, unknown>>>([])

  const canWriteSelectedValue = computed(() => {
    if (!selectedCandidateAddress.value || !writeValue.value.trim()) return false
    return !writeSafetyWarning.value || writeSafetyAcknowledged.value
  })

  watch([selectedCandidateAddress, writeValue, writeSafetyWarning], () => {
    writeSafetyAcknowledged.value = false
  })

  function updateWriteSafetyWarning() {
    writeSafetyWarning.value = ''
    const address = selectedCandidateAddress.value.trim()
    if (!address) return
    const region = deps().regionForAddress(address)
    if (!region) {
      writeSafetyWarning.value = 'Région inconnue : actualise la carte mémoire avant écriture.'
      return
    }
    if (region.writable !== true) {
      writeSafetyWarning.value = `Attention : la région 0x${region.baseAddress} n'est pas marquée writable (${region.protection ?? '?'}).`
      return
    }
    if (String(region.state ?? '').toLowerCase() !== 'committed') {
      writeSafetyWarning.value = `Attention : état mémoire ${String(region.state ?? '?')}, écriture risquée.`
    }
  }

  function checkpointAddress(checkpoint: Record<string, unknown>): string {
    return String(checkpoint.address ?? checkpoint.instructionPointer ?? checkpoint.rip ?? '')
      .replace(/^0x/i, '')
      .trim()
  }

  function checkpointType(checkpoint: Record<string, unknown>): string {
    return String(checkpoint.type ?? checkpoint.valueType ?? exactScanType.value ?? 'Int32')
  }

  function checkpointValue(checkpoint: Record<string, unknown>): string {
    return String(checkpoint.value ?? checkpoint.targetValue ?? writeValue.value ?? exactScanValue.value ?? '')
  }

  function buildCheckpointActionPlan(checkpoint: Record<string, unknown>): RuntimeActionPlan {
    const address = checkpointAddress(checkpoint)
    const type = checkpointType(checkpoint)
    const value = checkpointValue(checkpoint).trim()
    const kind = String(checkpoint.kind ?? 'checkpoint')
    const isCode =
      kind.toLowerCase().includes('code') ||
      kind.toLowerCase().includes('aob') ||
      Boolean(checkpoint.patchBytes || checkpoint.aobPattern || checkpoint.instructionPointer || checkpoint.rip)
    const hasAddress = Boolean(address)
    const hasWritableValue = Boolean(hasAddress && value && !isCode)
    const hasCodeTarget = Boolean(hasAddress && (isCode || checkpoint.sourceAddress))
    const actions: RuntimeActionPlanItem[] = [
      {
        id: 'watch',
        label: 'Watch',
        risk: 'safe',
        enabled: hasAddress && !isCode,
        reason: hasAddress && !isCode ? 'Surveiller la valeur live sans écrire.' : 'Réservé aux checkpoints mémoire avec adresse.',
      },
      {
        id: 'write',
        label: 'Préparer write',
        risk: 'write',
        enabled: hasWritableValue,
        reason: hasWritableValue ? 'Tester la valeur sous confirmation explicite.' : 'Adresse mémoire et valeur cible requises.',
      },
      {
        id: 'freeze_polling',
        label: 'Freeze',
        risk: 'write',
        enabled: hasWritableValue,
        reason: hasWritableValue ? 'Stabiliser par freeze polling sous confirmation.' : 'Adresse mémoire et valeur cible requises.',
      },
      {
        id: 'find_writes',
        label: 'Find What Writes',
        risk: 'debug',
        enabled: hasAddress && !isCode,
        reason: hasAddress && !isCode ? 'Capturer l’instruction qui modifie cette adresse.' : 'Le debugger part d’une adresse mémoire, pas d’un RIP déjà capturé.',
      },
      {
        id: 'aob_patch',
        label: 'AOB/Patch',
        risk: 'patch',
        enabled: hasCodeTarget,
        reason: hasCodeTarget ? 'Générer une signature et proposer un patch réversible.' : 'Nécessite un RIP, une signature ou une source code.',
      },
      {
        id: 'force_value',
        label: 'Forcer valeur (hook)',
        risk: 'patch',
        enabled: kind === 'code_writer' && hasAddress,
        reason: kind === 'code_writer' && hasAddress
          ? 'Installer un trampoline sur ce RIP pour forcer une valeur, même si la source est un registre.'
          : 'Réservé aux checkpoints Find What Writes (RIP capturé).',
      },
      {
        id: 'bookmark',
        label: 'Bookmark',
        risk: 'safe',
        enabled: true,
        reason: 'Conserver la piste dans le workspace avec ses preuves.',
      },
      {
        id: 'trainer',
        label: 'Créer Trainer',
        risk: isCode ? 'patch' : 'write',
        enabled: hasAddress,
        reason: hasAddress ? 'Transformer la piste en feature réutilisable.' : 'Une feature Trainer nécessite une adresse ou signature.',
      },
    ]
    return {
      label: String(checkpoint.label ?? checkpoint.name ?? checkpoint.address ?? checkpoint.id ?? 'Checkpoint'),
      address,
      type,
      value,
      kind,
      isCode,
      safeCount: actions.filter((action) => action.enabled && action.risk === 'safe').length,
      riskyCount: actions.filter((action) => action.enabled && action.risk !== 'safe').length,
      actions,
    }
  }

  async function executeCheckpointWrite(checkpoint: Record<string, unknown>, freeze = false) {
    const address = checkpointAddress(checkpoint)
    const type = checkpointType(checkpoint)
    const value = checkpointValue(checkpoint)
    if (!address || !value.trim()) {
      addActionLog('checkpoint', 'Checkpoint incomplet', 'Adresse ou valeur manquante.', 'warning')
      return null
    }
    const title = freeze ? 'Checkpoint freeze polling' : 'Checkpoint écriture'
    const kernelActive = deps().kernelMemoryModeActive.value === true
    const risk: RiskLevel = !freeze && kernelActive ? 'injection' : 'write'
    const route = !freeze && kernelActive ? ' via driver kernel' : ''
    if (!await deps().confirmRiskAction(risk, title, `0x${address} ${type} = ${value}${route}.`)) return null

    try {
      const controller = backend.getController()
      const result = freeze
        ? await controller.setFreezeValue(address, type, value, true)
        : await deps().writeMemoryValueByMode(address, type, value)
      writeResult.value = result as MemoryWriteResult
      if (result.success === true) {
        deps().addAddressToWatch(address, type)
        deps().upsertSessionEntry(address, type, freeze ? 'freeze_polling' : 'write', freeze)
      }
      addActionLog(
        'checkpoint',
        result.success === true ? `${title} OK` : `${title} échoué`,
        String(result.error || `0x${address}`),
        result.success === true ? 'success' : 'error',
      )
      addInvestigationStep({
        title: result.success === true ? `${title} exécuté` : `${title} échoué`,
        detail: String(result.error || `0x${address} ${type} = ${value}`),
        status: result.success === true ? 'success' : 'error',
        tool: freeze ? 'setFreezeValue' : (kernelActive ? 'writeMemoryValueKernel' : 'writeMemoryValue'),
        risk,
        payload: result as unknown as Record<string, unknown>,
      })
      deps().logAiAudit(freeze ? 'checkpoint_freeze_executed' : 'checkpoint_write_executed', {
        success: result.success === true,
        address,
        type,
        value,
        error: result.error ?? '',
      })
      return result
    } catch (e) {
      addActionLog('checkpoint', `${title} échoué`, String(e), 'error')
      return null
    }
  }

  async function executeCheckpointKernelWrite(checkpoint: Record<string, unknown>) {
    const address = checkpointAddress(checkpoint)
    const type = checkpointType(checkpoint)
    const value = checkpointValue(checkpoint)
    if (!address || !value.trim()) {
      addActionLog('checkpoint', 'Écriture kernel impossible', 'Adresse ou valeur manquante.', 'warning')
      return null
    }
    if (!await deps().confirmRiskAction('injection', 'Écriture mémoire via driver noyau', `0x${address} ${type} = ${value} (contourne les protections mémoire usermode).`)) return null
    const controller = backend.getController()
    if (!controller.writeMemoryValueKernel) {
      addActionLog('checkpoint', 'Écriture kernel indisponible', 'Backend non exposé.', 'warning')
      return null
    }
    try {
      const result = await controller.writeMemoryValueKernel(address, type, value)
      writeResult.value = result as unknown as MemoryWriteResult
      if (result.success === true) deps().addAddressToWatch(address, type)
      addActionLog(
        'checkpoint',
        result.success === true ? 'Écriture kernel OK' : 'Écriture kernel échouée',
        String(result.error || `0x${address}`),
        result.success === true ? 'success' : 'error',
      )
      addInvestigationStep({
        title: result.success === true ? 'Écriture kernel exécutée' : 'Écriture kernel échouée',
        detail: String(result.error || `0x${address} ${type} = ${value} via driver noyau`),
        status: result.success === true ? 'success' : 'error',
        tool: 'writeMemoryValueKernel',
        risk: 'injection',
        payload: result as unknown as Record<string, unknown>,
      })
      deps().logAiAudit('checkpoint_kernel_write_executed', {
        success: result.success === true,
        address,
        type,
        value,
        error: result.error ?? '',
      })
      return result
    } catch (e) {
      addActionLog('checkpoint', 'Écriture kernel échouée', String(e), 'error')
      return null
    }
  }

  async function writeSelectedAddresses(addresses: string[], type: string, value: string) {
    if (addresses.length === 0) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: 'Aucune adresse sélectionnée.' }
      return
    }
    if (!value.trim()) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: 'Entre une valeur à écrire.' }
      return
    }
    const kernelActive = deps().kernelMemoryModeActive.value === true
    const risk: RiskLevel = kernelActive ? 'injection' : 'write'
    if (!await deps().confirmRiskAction(risk, 'Ecriture memoire multiple', `${addresses.length} adresse(s), type ${type}, valeur ${value}${kernelActive ? ' via driver kernel' : ''}.`)) return
    try {
      const results: MemoryWriteResult[] = []
      for (const address of addresses) {
        const result = await deps().writeMemoryValueByMode(address, type, value)
        results.push(result)
      }
      writeResult.value = results[results.length - 1]
      scanStatusText.value = results.every((r) => r.success)
        ? `${results.length} adresse(s) écrite(s).`
        : `Écriture partielle: ${results.filter((r) => r.success).length}/${results.length} réussie(s).`
      addActionLog('write', `Écriture multiple ${value}`, `${results.filter((r) => r.success).length}/${results.length} réussie(s)${kernelActive ? ' via kernel' : ''}.`, results.every((r) => r.success) ? 'success' : 'warning')
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e) }
      scanStatusText.value = 'Écriture multiple échouée.'
      addActionLog('write', 'Écriture multiple échouée', String(e), 'error')
    }
  }

  async function writeSelectedTargets(targets: MemoryWriteTarget[], value: string) {
    if (targets.length === 0) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: 'Aucune cible sélectionnée.' }
      return
    }
    if (!value.trim()) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: 'Entre une valeur à écrire.' }
      return
    }
    const kernelActive = deps().kernelMemoryModeActive.value === true
    const risk: RiskLevel = kernelActive ? 'injection' : 'write'
    if (!await deps().confirmRiskAction(risk, 'Ecriture memoire avec variants', `${targets.length} cible(s), valeur affichee ${value}${kernelActive ? ' via driver kernel' : ''}.`)) return
    try {
      const controller = backend.getController()
      if (kernelActive) {
        const results: MemoryWriteResult[] = []
        for (const target of targets) {
          results.push(await deps().writeMemoryValueByMode(target.address, target.type, value))
        }
        const written = results.filter((result) => result.success).length
        writeResult.value = {
          success: written === targets.length,
          verified: results.every((result) => result.verified),
          bytesWritten: results.reduce((sum, result) => sum + (result.bytesWritten ?? 0), 0),
          written,
          total: targets.length,
          results,
          error: written === targets.length ? '' : `Écriture kernel partielle: ${written}/${targets.length}.`,
        } as MemoryWriteBatchResult
        scanStatusText.value = written === targets.length
          ? `${written} adresse(s) écrite(s) via kernel.`
          : `Écriture kernel partielle: ${written}/${targets.length} réussie(s).`
        for (const target of targets) deps().addAddressToWatch(target.address, target.type)
        addActionLog('write', `Écriture auto kernel ${value}`, `${written}/${targets.length} réussie(s).`, written === targets.length ? 'success' : 'warning')
        return
      }
      if (controller.writeMemoryValuesWithVariants) {
        const result: MemoryWriteBatchResult = await controller.writeMemoryValuesWithVariants(targets, value)
        writeResult.value = result
        const written = result.written ?? result.results?.filter((r) => r.success).length ?? 0
        scanStatusText.value = result.success
          ? `${written} adresse(s) écrite(s) avec encodage auto.`
          : `Écriture auto partielle: ${written}/${targets.length} réussie(s).`
        for (const target of targets) {
          deps().addAddressToWatch(target.address, target.type)
        }
        addActionLog('write', `Écriture auto ${value}`, `${written}/${targets.length} réussie(s).`, result.success ? 'success' : 'warning')
        return
      }
      await writeSelectedAddresses(targets.map((target) => target.address), targets[0]?.type ?? exactScanType.value, value)
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e) }
      scanStatusText.value = 'Écriture auto échouée.'
      addActionLog('write', 'Écriture auto échouée', String(e), 'error')
    }
  }

  // H3 (docs/STRATEGY_ROOM.md, session Solitaire du 19/08/2026) : contrairement
  // à writeSelectedAddresses (une écriture à la fois, l'une après l'autre),
  // écrit toutes les adresses dans la même fenêtre critique (threads de la
  // cible suspendues) — pour les cibles qui maintiennent des copies
  // redondantes d'une même valeur et resynchronisent une écriture isolée.
  async function writeSelectedAtomic(addresses: string[], type: string, value: string) {
    if (addresses.length === 0) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: 'Aucune adresse sélectionnée.' }
      return
    }
    if (!value.trim()) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: 'Entre une valeur à écrire.' }
      return
    }
    if (!await deps().confirmRiskAction('write', 'Écriture atomique multi-adresses', `${addresses.length} adresse(s) en même temps (threads de la cible suspendues), type ${type}, valeur ${value}.`)) return
    const controller = backend.getController()
    if (!controller.writeMemoryValuesAtomic) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: 'Écriture atomique indisponible sur ce backend.' }
      return
    }
    try {
      const targets: AtomicWriteTarget[] = addresses.map((address) => ({ address, type, value }))
      const result: MemoryWriteBatchResult = await controller.writeMemoryValuesAtomic(targets, {})
      writeResult.value = result
      const written = result.written ?? result.results?.filter((r) => r.success).length ?? 0
      scanStatusText.value = result.success
        ? `${written} adresse(s) écrite(s) ensemble (atomique).`
        : `Écriture atomique partielle: ${written}/${addresses.length} réussie(s).`
      for (const address of addresses) {
        deps().addAddressToWatch(address, type)
      }
      addActionLog('write', `Écriture atomique ${value}`, `${written}/${addresses.length} réussie(s), ${result.suspendedThreadCount ?? 0} thread(s) suspendue(s).`, result.success ? 'success' : 'warning')
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e) }
      scanStatusText.value = 'Écriture atomique échouée.'
      addActionLog('write', 'Écriture atomique échouée', String(e), 'error')
    }
  }

  // Automatisation IA : après une écriture réussie sur une adresse unique,
  // cherche silencieusement (lecture seule, bornée) une chaîne de pointeurs
  // stable, sans que l'utilisateur ait besoin de savoir que ce bouton existe
  // dans Expert. Ne notifie que si une chaîne est réellement trouvée — pas de
  // bruit pour chaque écriture. Dédupliqué par adresse pour la session en
  // cours pour ne pas ressasher la même suggestion à chaque nouvelle écriture
  // sur la même adresse (freeze, retest, etc.).
  const stableLocatorSuggested = new Set<string>()

  async function autoSuggestStableLocatorIfWorthwhile(addressHex: string) {
    const key = addressHex.toLowerCase()
    if (!addressHex || stableLocatorSuggested.has(key)) return
    stableLocatorSuggested.add(key)
    try {
      const controller = backend.getController()
      if (!controller.suggestStableLocatorForAddress) return
      const result = await controller.suggestStableLocatorForAddress(addressHex, {})
      if (result.success && result.bestChain) {
        deps().pushMessage(
          'assistant',
          `🔗 J'ai trouvé une chaîne de pointeurs stable pour 0x${addressHex} (${result.message ?? 'profondeur ' + (result.bestChain.depth ?? '?')}). ` +
            `Elle survivra à un redémarrage du jeu — ouvre Expert > Write et clique "Sauvegarder dans un profil" pour la garder.`,
        )
      }
    } catch {
      // Suggestion best-effort : ne doit jamais interrompre le flux d'écriture principal.
    }
  }

  async function writeSelectedValue() {
    // Avant : retour silencieux si rien n'est sélectionné/rempli — l'utilisateur
    // clique Écrire, rien ne se passe, aucun indice pourquoi. Message explicite
    // à la place, affiché au même endroit que les autres erreurs d'écriture.
    if (!selectedCandidateAddress.value) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: 'Aucune adresse sélectionnée : clique une adresse dans Candidats avant d\'écrire.' }
      return
    }
    if (!writeValue.value.trim()) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: 'Entre une valeur à écrire avant de cliquer sur Écrire.' }
      return
    }
    updateWriteSafetyWarning()
    if (writeSafetyWarning.value && !writeSafetyAcknowledged.value) {
      addActionLog('write_guard', 'Écriture bloquée', writeSafetyWarning.value, 'warning')
      return
    }
    const kernelActive = deps().kernelMemoryModeActive.value === true
    const risk: RiskLevel = kernelActive ? 'injection' : 'write'
    if (!await deps().confirmRiskAction(risk, 'Ecriture memoire', `0x${selectedCandidateAddress.value} ${exactScanType.value} = ${writeValue.value}${kernelActive ? ' via driver kernel' : ''}.`)) return
    try {
      writeResult.value = await deps().writeMemoryValueByMode(selectedCandidateAddress.value, exactScanType.value, writeValue.value)
      deps().addAddressToWatch(selectedCandidateAddress.value, exactScanType.value)
      addActionLog(
        'write',
        `Écriture 0x${selectedCandidateAddress.value}`,
        `${exactScanType.value} = ${writeValue.value}${kernelActive ? ' via kernel' : ''}${writeSafetyWarning.value ? ` · ${writeSafetyWarning.value}` : ''}.`,
        writeResult.value.success ? 'success' : 'error',
      )
      if (writeResult.value.success && writeResult.value.verified) {
        void autoSuggestStableLocatorIfWorthwhile(selectedCandidateAddress.value)
      }
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e) }
      addActionLog('write', `Écriture échouée 0x${selectedCandidateAddress.value}`, String(e), 'error')
    }
  }

  async function rollbackLastWrite() {
    try {
      writeResult.value = await backend.getController().rollbackLastWrite()
      addActionLog('rollback', 'Rollback dernière écriture', writeResult.value.success ? 'Adresse restaurée.' : writeResult.value.error, writeResult.value.success ? 'success' : 'warning')
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e) }
      addActionLog('rollback', 'Rollback échoué', String(e), 'error')
    }
  }

  async function rollbackLastWriteBatch() {
    try {
      const result = await backend.getController().rollbackLastWriteBatch()
      const restored = Array.isArray(result.restoredWrites)
        ? result.restoredWrites
          .map((item: unknown) => {
            const record = item as Record<string, unknown>
            const prefix = record.success === true ? '✓' : '✗'
            return `${prefix} 0x${record.address}: ${String(record.from ?? '?')} -> ${String(record.to ?? '?')}`
          })
          .join('\n')
        : ''
      deps().pushMessage('assistant', result.success
        ? `Rollback batch réussi : ${result.rolledBack}/${result.total} écritures restaurées.${restored ? `\n${restored}` : ''}`
        : `Rollback batch partiel : ${String(result.rolledBack ?? 0)}/${String(result.total ?? 0)} restaurées.${restored ? `\n${restored}` : ''}`)
      addActionLog('rollback', 'Rollback batch', `${String(result.rolledBack ?? 0)}/${String(result.total ?? 0)} restaurée(s).`, result.success ? 'success' : 'warning')
      await deps().refreshActiveChatMemoryTargets()
      await deps().refreshSmartSearchContext()
      return result
    } catch (e) {
      deps().pushMessage('assistant', 'Rollback batch échoué : ' + String(e), { isError: true })
      return { success: false, error: String(e) }
    }
  }

  async function freezeCandidateCurrent(address: string, type: string) {
    const normalized = address.trim().replace(/^0x/i, '')
    if (!normalized) return

    selectedCandidateAddress.value = normalized
    exactScanType.value = type
    deps().addAddressToWatch(normalized, type)

    const watched = await deps().refreshWatchedAddress(normalized)
    const currentValue = watched?.value.trim() ?? ''
    if (!currentValue || currentValue === '-') {
      writeResult.value = {
        success: false,
        verified: false,
        bytesWritten: 0,
        error: watched?.error || 'Valeur actuelle illisible.',
        enabled: freezeEnabled.value,
      }
      addActionLog('freeze', `Freeze impossible 0x${normalized}`, writeResult.value.error, 'error')
      return
    }

    writeValue.value = currentValue
    updateWriteSafetyWarning()
    if (writeSafetyWarning.value && !writeSafetyAcknowledged.value) {
      addActionLog('write_guard', 'Freeze bloqué', writeSafetyWarning.value, 'warning')
      return
    }
    if (!await deps().confirmRiskAction('write', 'Freeze memoire', `0x${normalized} ${type} = ${currentValue}.`)) return

    try {
      writeResult.value = await backend
        .getController()
        .setFreezeValue(normalized, type, currentValue, true)
      if (writeResult.value.success) {
        freezeEnabled.value = true
        deps().upsertSessionEntry(normalized, type, 'freeze_polling', true)
        await deps().refreshWatchedAddress(normalized)
      }
      addActionLog(
        'freeze',
        writeResult.value.success ? 'Freeze actuel activé' : 'Freeze actuel échoué',
        `0x${normalized} ${type} = ${currentValue}.`,
        writeResult.value.success ? 'success' : 'error',
      )
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e), enabled: freezeEnabled.value }
      addActionLog('freeze', `Freeze échoué 0x${normalized}`, String(e), 'error')
    }
  }

  async function toggleFreeze() {
    if (!selectedCandidateAddress.value || !writeValue.value.trim()) return
    const nextState = !freezeEnabled.value
    updateWriteSafetyWarning()
    if (nextState && writeSafetyWarning.value && !writeSafetyAcknowledged.value) {
      addActionLog('write_guard', 'Freeze bloqué', writeSafetyWarning.value, 'warning')
      return
    }
    if (nextState && !await deps().confirmRiskAction('write', 'Freeze memoire', `0x${selectedCandidateAddress.value} ${exactScanType.value} = ${writeValue.value}.`)) return
    try {
      writeResult.value = await backend
        .getController()
        .setFreezeValue(selectedCandidateAddress.value, exactScanType.value, writeValue.value, nextState)
      if (writeResult.value.success) {
        freezeEnabled.value = nextState
        deps().addAddressToWatch(selectedCandidateAddress.value, exactScanType.value)
        deps().upsertSessionEntry(selectedCandidateAddress.value, exactScanType.value, 'freeze_polling', nextState)
      }
      addActionLog('freeze', nextState ? 'Freeze activé' : 'Freeze arrêté', `0x${selectedCandidateAddress.value} = ${writeValue.value}.`, writeResult.value.success ? 'success' : 'error')
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e), enabled: freezeEnabled.value }
      addActionLog('freeze', 'Freeze échoué', String(e), 'error')
    }
  }

  async function startBreakpointFreeze() {
    if (!selectedCandidateAddress.value || !writeValue.value.trim()) return
    updateWriteSafetyWarning()
    if (writeSafetyWarning.value && !writeSafetyAcknowledged.value) {
      addActionLog('write_guard', 'Freeze BP bloqué', writeSafetyWarning.value, 'warning')
      return
    }
    if (!await deps().confirmRiskAction('debug', 'Freeze par hardware breakpoint', `0x${selectedCandidateAddress.value} ${exactScanType.value} = ${writeValue.value}. Debug registers/attach requis.`)) return

    const controller = backend.getController()
    if (!controller.freezeWithBreakpoint) {
      writeResult.value = {
        success: false,
        verified: false,
        bytesWritten: 0,
        error: 'Freeze par breakpoint non exposé par ce backend.',
        enabled: breakpointFreezeEnabled.value,
      }
      addActionLog('freeze', 'Freeze BP indisponible', writeResult.value.error, 'warning')
      return
    }

    try {
      writeResult.value = await controller.freezeWithBreakpoint(
        selectedCandidateAddress.value,
        exactScanType.value,
        writeValue.value,
        { mode: 'rewrite' },
      )
      breakpointFreezeEnabled.value = writeResult.value.success === true
      if (breakpointFreezeEnabled.value) {
        deps().addAddressToWatch(selectedCandidateAddress.value, exactScanType.value)
        deps().upsertSessionEntry(selectedCandidateAddress.value, exactScanType.value, 'freeze_breakpoint', true)
      }
      addActionLog(
        'freeze',
        breakpointFreezeEnabled.value ? 'Freeze BP activé' : 'Freeze BP échoué',
        `0x${selectedCandidateAddress.value} = ${writeValue.value}.`,
        breakpointFreezeEnabled.value ? 'success' : 'error',
      )
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e), enabled: breakpointFreezeEnabled.value }
      addActionLog('freeze', 'Freeze BP échoué', String(e), 'error')
    }
  }

  // Escalade proposee par le chat Assistant apres freezeInstabilityDetected
  // (freeze polling qui derive) : contrairement a startBreakpointFreeze(),
  // l'utilisateur n'a que l'adresse en main, pas le type/la valeur — le
  // backend reutilise directement la FreezeEntry polling existante.
  async function escalateFreezeToBreakpoint(address: string) {
    if (!address.trim()) return
    if (!await deps().confirmRiskAction('debug', 'Freeze par hardware breakpoint', `0x${address} : le freeze polling ne tient pas, passage en Freeze BP. Debug registers/attach requis.`)) return

    const controller = backend.getController()
    if (!controller.escalatePollingFreezeToBreakpoint) {
      deps().pushMessage('assistant', 'Freeze par breakpoint non exposé par ce backend.')
      addActionLog('freeze', 'Freeze BP indisponible', 'escalatePollingFreezeToBreakpoint absent du backend.', 'warning')
      return
    }

    try {
      const result = await controller.escalatePollingFreezeToBreakpoint(address)
      const ok = result.success === true
      breakpointFreezeEnabled.value = ok || breakpointFreezeEnabled.value
      if (ok) {
        selectedCandidateAddress.value = address
        if (result.type) exactScanType.value = String(result.type)
        deps().addAddressToWatch(address, String(result.type ?? exactScanType.value))
        deps().upsertSessionEntry(address, String(result.type ?? exactScanType.value), 'freeze_breakpoint', true)
        deps().markSessionEntryEnabled(address, String(result.type ?? exactScanType.value), 'freeze_polling', false)
      }
      addActionLog(
        'freeze',
        ok ? 'Freeze BP activé (escalade)' : 'Freeze BP échoué (escalade)',
        `0x${address}. ${String(result.error ?? '')}`.trim(),
        ok ? 'success' : 'error',
      )
      deps().pushMessage(
        'assistant',
        ok
          ? `Freeze BP actif sur 0x${address} : l'écriture est maintenant bloquée à la source, ça devrait tenir même si la cible réécrit vite.`
          : `Échec du passage en Freeze BP sur 0x${address}${result.error ? ` : ${String(result.error)}` : '.'}`,
      )
    } catch (e) {
      addActionLog('freeze', 'Freeze BP échoué (escalade)', String(e), 'error')
      deps().pushMessage('assistant', `Échec du passage en Freeze BP sur 0x${address} : ${String(e)}`)
    }
  }

  async function stopBreakpointFreeze() {
    const controller = backend.getController()
    if (!controller.stopBreakpointFreeze) {
      writeResult.value = {
        success: false,
        verified: false,
        bytesWritten: 0,
        error: 'Arrêt du freeze par breakpoint non exposé par ce backend.',
        enabled: breakpointFreezeEnabled.value,
      }
      return
    }

    try {
      writeResult.value = await controller.stopBreakpointFreeze()
      if (writeResult.value.success) {
        breakpointFreezeEnabled.value = false
      }
      const hits = writeResult.value.hits !== undefined ? ` hits=${writeResult.value.hits}` : ''
      const rewrites = writeResult.value.rewrites !== undefined ? ` rewrites=${writeResult.value.rewrites}` : ''
      addActionLog('freeze', 'Freeze BP arrêté', `${hits}${rewrites}`.trim() || 'Session arrêtée.', writeResult.value.success ? 'success' : 'warning')
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e), enabled: breakpointFreezeEnabled.value }
      addActionLog('freeze', 'Arrêt Freeze BP échoué', String(e), 'error')
    }
  }

  async function setFreezeInterval(intervalMs: number) {
    const requested = Math.round(Number(intervalMs))
    const clamped = Math.min(2000, Math.max(10, Number.isFinite(requested) ? requested : 100))
    freezeIntervalMs.value = clamped
    try {
      const result = await backend.getController().setFreezeInterval(clamped)
      freezeIntervalResult.value = result
      if (result.success === false) {
        addActionLog('freeze', 'Intervalle freeze refusé', String(result.error ?? 'Erreur inconnue.'), 'warning')
        return
      }
      const applied = Math.round(Number(result.intervalMs ?? clamped))
      if (Number.isFinite(applied)) freezeIntervalMs.value = applied
      addActionLog('freeze', 'Intervalle freeze', `${freezeIntervalMs.value} ms.`, 'success')
    } catch (e) {
      freezeIntervalResult.value = { success: false, error: String(e) }
      addActionLog('freeze', 'Intervalle freeze échoué', String(e), 'error')
    }
  }

  async function refreshWriteHistorySequence() {
    const controller = backend.getController()
    if (!controller.getWriteHistorySequence) {
      writeHistorySequence.value = []
      return
    }
    const result = await controller.getWriteHistorySequence()
    writeHistorySequence.value = result.success
      ? ((result.sequence as Array<Record<string, unknown>>) ?? [])
      : []
  }

  async function replayWriteHistorySequence() {
    const controller = backend.getController()
    if (!controller.replayWriteHistorySequence) {
      addActionLog('write-history', 'Replay indisponible', 'Backend non exposé.', 'warning')
      return null
    }
    if (!await deps().confirmRiskAction('write', 'Rejouer la séquence d\'écritures', `Rejouer ${writeHistorySequence.value.length} écriture(s) confirmée(s) dans l'ordre pour ce processus.`)) return null
    try {
      const result = await controller.replayWriteHistorySequence()
      addActionLog(
        'write-history',
        result.success ? 'Replay terminé' : 'Replay échoué',
        `${result.replayedCount ?? 0} rejouée(s), ${result.skippedCount ?? 0} ignorée(s), ${result.failedCount ?? 0} échouée(s).`,
        result.success ? 'success' : 'warning',
      )
      return result
    } catch (e) {
      addActionLog('write-history', 'Replay échoué', String(e), 'error')
      return null
    }
  }

  async function clearWriteHistorySequence() {
    const controller = backend.getController()
    if (!controller.clearWriteHistorySequence) return null
    const result = await controller.clearWriteHistorySequence()
    await refreshWriteHistorySequence()
    return result
  }

  return {
    configureWriteFreezeContext,
    writeValue,
    writeResult,
    writeSafetyWarning,
    writeSafetyAcknowledged,
    freezeEnabled,
    breakpointFreezeEnabled,
    freezeIntervalMs,
    freezeIntervalResult,
    writeHistorySequence,
    canWriteSelectedValue,
    updateWriteSafetyWarning,
    checkpointAddress,
    checkpointType,
    checkpointValue,
    buildCheckpointActionPlan,
    executeCheckpointWrite,
    executeCheckpointKernelWrite,
    writeSelectedAddresses,
    writeSelectedTargets,
    writeSelectedAtomic,
    autoSuggestStableLocatorIfWorthwhile,
    writeSelectedValue,
    rollbackLastWrite,
    rollbackLastWriteBatch,
    freezeCandidateCurrent,
    toggleFreeze,
    startBreakpointFreeze,
    escalateFreezeToBreakpoint,
    stopBreakpointFreeze,
    setFreezeInterval,
    refreshWriteHistorySequence,
    replayWriteHistorySequence,
    clearWriteHistorySequence,
  }
})
