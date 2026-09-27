/**
 * KillEngine — logique pure de l'historique du workspace (UX-PRODUIT-13,
 * docs/PHASE_TRACKER.md). Aucune dépendance Pinia/Vue/runtime -- même
 * principe que `workspaceImportValidation.ts` : testable en isolation via
 * esbuild+node (scripts/test-workspace-history-scheduling.ps1/.mjs), le
 * store `workspaceHistory.ts` ne fait qu'appeler ces fonctions depuis de
 * vrais timers/watchers.
 *
 * Trois responsabilités séparées :
 * - `computeFingerprint` : empreinte déterministe d'un snapshot récupérable,
 *   volontairement dépouillée des champs qui changent sans changement réel
 *   de contenu (exportedAt) -- pas de fonction de hash cryptographique,
 *   juste une chaîne canonique comparable par égalité stricte.
 * - `shouldCaptureAutomatic` : la décision "on sauvegarde maintenant ?" une
 *   fois le débounce de 2s (mécanique de timer réel, pas testée ici)
 *   écoulé -- contenu réellement différent + throttle 30s respecté.
 * - `diffRecoverableSnapshots` : comparaison avant/après par IDs internes à
 *   une même série, jamais de correspondance par nom.
 */
import type { InvestigationRun } from './investigation'
import type { TrainerFeature } from './trainer'
import type { StructureTemplate, WorkspaceBookmark } from './workspaceItems'
import type { UserActionLogEntry } from './actionLog'

export type WorkspaceHistorySectionKey = 'investigation' | 'trainer' | 'structures' | 'bookmarks' | 'audit'

export interface RecoverableSnapshot {
  version: number
  lastPresetId: string
  investigation?: { active: InvestigationRun | null, archive: InvestigationRun[] }
  trainer?: { features: TrainerFeature[] }
  structures?: { templates: StructureTemplate[] }
  bookmarks?: { items: WorkspaceBookmark[] }
  audit?: { entries: UserActionLogEntry[] }
}

/// Stringify déterministe (clés triées, récursif) -- deux objets équivalents
/// produisent toujours la même chaîne, indépendamment de l'ordre d'insertion.
function canonicalStringify(value: unknown): string {
  if (value === null || typeof value !== 'object') {
    return JSON.stringify(value)
  }
  if (Array.isArray(value)) {
    return `[${value.map((item) => canonicalStringify(item)).join(',')}]`
  }
  const keys = Object.keys(value as Record<string, unknown>).sort()
  const parts = keys.map((key) => `${JSON.stringify(key)}:${canonicalStringify((value as Record<string, unknown>)[key])}`)
  return `{${parts.join(',')}}`
}

/**
 * Empreinte du contenu récupérable, en excluant les champs volatils qui ne
 * reflètent aucun changement de contenu réel (dates d'export). Prend le JSON
 * texte déjà construit par `buildRecoverableSnapshotJson` (workspaceSession.ts)
 * pour ne pas dupliquer la construction du snapshot ici.
 */
export function computeFingerprint(recoverableSnapshotJson: string): string {
  let parsed: Record<string, unknown>
  try {
    parsed = JSON.parse(recoverableSnapshotJson) as Record<string, unknown>
  } catch {
    // Un JSON invalide ne doit jamais déclencher une sauvegarde automatique
    // silencieuse -- empreinte stable et distincte, jamais égale à une
    // empreinte valide précédente (donc "changement détecté" côté appelant,
    // qui échouera ensuite proprement à la capture réelle).
    return '<invalid-json>'
  }
  delete parsed.exportedAt
  return canonicalStringify(parsed)
}

export interface AutomaticCaptureDecisionInput {
  lastFingerprint: string | null
  currentFingerprint: string
  lastAutoSaveAtMs: number
  nowMs: number
}

/**
 * Décision prise APRÈS que le débounce de 2s (timer réel, côté store) s'est
 * écoulé sans nouvelle mutation : y a-t-il un vrai changement de contenu
 * (pas juste le tick du timer), et le throttle de 30s entre deux
 * automatiques est-il respecté ?
 */
export function shouldCaptureAutomatic(input: AutomaticCaptureDecisionInput): boolean {
  if (input.currentFingerprint === input.lastFingerprint) {
    return false
  }
  if (input.nowMs - input.lastAutoSaveAtMs < 30_000) {
    return false
  }
  return true
}

export interface SectionDiffItem {
  id: string
  kind: 'added' | 'removed' | 'modified'
  label: string
}

export interface SectionDiffResult {
  section: WorkspaceHistorySectionKey
  items: SectionDiffItem[]
}

/// Comparaison générique par ID : jamais de correspondance par nom/libellé.
function diffById<T>(
  before: T[] | undefined,
  after: T[] | undefined,
  idOf: (item: T) => string,
  labelOf: (item: T) => string,
): SectionDiffItem[] {
  const beforeMap = new Map((before ?? []).map((item) => [idOf(item), item]))
  const afterMap = new Map((after ?? []).map((item) => [idOf(item), item]))
  const items: SectionDiffItem[] = []

  for (const [id, item] of afterMap) {
    if (!beforeMap.has(id)) {
      items.push({ id, kind: 'added', label: labelOf(item) })
    } else if (canonicalStringify(beforeMap.get(id)) !== canonicalStringify(item)) {
      items.push({ id, kind: 'modified', label: labelOf(item) })
    }
  }
  for (const [id, item] of beforeMap) {
    if (!afterMap.has(id)) {
      items.push({ id, kind: 'removed', label: labelOf(item) })
    }
  }
  return items
}

/**
 * Comparaison avant/après par section, déterministe. Ne fusionne jamais deux
 * entrées par nom -- une entrée sans ID comparable dans l'autre snapshot est
 * "ajoutée" ou "retirée", jamais réinterprétée comme "modifiée" par
 * ressemblance.
 */
export function diffRecoverableSnapshots(before: RecoverableSnapshot, after: RecoverableSnapshot): SectionDiffResult[] {
  const results: SectionDiffResult[] = []

  results.push({
    section: 'trainer',
    items: diffById(before.trainer?.features, after.trainer?.features, (f) => String(f.id), (f) => f.name),
  })
  results.push({
    section: 'structures',
    items: diffById(before.structures?.templates, after.structures?.templates, (t) => String(t.id), (t) => t.name),
  })
  results.push({
    section: 'bookmarks',
    items: diffById(before.bookmarks?.items, after.bookmarks?.items, (b) => String(b.id), (b) => b.label),
  })
  results.push({
    section: 'audit',
    items: diffById(before.audit?.entries, after.audit?.entries, (e) => String(e.id), (e) => e.title),
  })

  // Investigation : archive comparée par ID comme les autres listes, plus un
  // traitement dédié pour l'enquête active (objet nullable unique, pas un tableau).
  const archiveItems = diffById(
    before.investigation?.archive,
    after.investigation?.archive,
    (r) => String(r.id),
    (r) => r.title,
  )
  const beforeActive = before.investigation?.active ?? null
  const afterActive = after.investigation?.active ?? null
  if (beforeActive === null && afterActive !== null) {
    archiveItems.push({ id: String(afterActive.id), kind: 'added', label: `${afterActive.title} (active)` })
  } else if (beforeActive !== null && afterActive === null) {
    archiveItems.push({ id: String(beforeActive.id), kind: 'removed', label: `${beforeActive.title} (active)` })
  } else if (beforeActive !== null && afterActive !== null) {
    if (canonicalStringify(beforeActive) !== canonicalStringify(afterActive)) {
      archiveItems.push({ id: String(afterActive.id), kind: 'modified', label: `${afterActive.title} (active)` })
    }
  }
  results.push({ section: 'investigation', items: archiveItems })

  return results
}
