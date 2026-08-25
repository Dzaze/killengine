/**
 * Logique pure des dépendances entre features Trainer (`dependsOn`, PHASE 103).
 * Aucune dépendance sur Vue/Pinia/le backend -- ce module ne touche qu'à des
 * tableaux/objets passés en paramètre, ce qui le rend testable directement
 * via `node` (voir `scripts/test-trainer-dependencies.ps1`), sans framework
 * de test JS/TS (aucun n'est configuré dans `ui/` à ce jour).
 */

export interface TrainerFeatureLike {
  id: number
  name: string
  action?: string
  enabled?: boolean
  dependsOn?: number[]
}

export interface ResolveOrderResult {
  success: boolean
  order: number[]
  error?: string
}

/**
 * Tri topologique (Kahn) des features Trainer d'après `dependsOn`, restreint
 * à la clôture (targetIds + toutes leurs dépendances transitives, même hors
 * du set demandé). order[0] n'a aucune dépendance non résolue : à appliquer
 * avant les suivants. Pour une restauration, itérer `order` à l'envers
 * (défaire les dépendants avant leurs dépendances).
 */
export function resolveTrainerFeatureOrder(
  features: TrainerFeatureLike[],
  targetIds: number[],
): ResolveOrderResult {
  const byId = new Map<number, TrainerFeatureLike>()
  for (const feature of features) byId.set(feature.id, feature)

  const closure = new Set<number>()
  const stack = [...targetIds]
  while (stack.length > 0) {
    const id = stack.pop() as number
    if (closure.has(id)) continue
    const feature = byId.get(id)
    if (!feature) continue
    closure.add(id)
    for (const depId of feature.dependsOn ?? []) {
      if (!byId.has(depId)) {
        return { success: false, order: [], error: `"${feature.name}" dépend d'une feature introuvable (id ${depId}).` }
      }
      if (!closure.has(depId)) stack.push(depId)
    }
  }

  const inDegree = new Map<number, number>()
  const dependents = new Map<number, number[]>()
  for (const id of closure) {
    inDegree.set(id, 0)
    dependents.set(id, [])
  }
  for (const id of closure) {
    const feature = byId.get(id) as TrainerFeatureLike
    for (const depId of feature.dependsOn ?? []) {
      inDegree.set(id, (inDegree.get(id) ?? 0) + 1)
      dependents.get(depId)?.push(id)
    }
  }

  const queue: number[] = []
  for (const id of closure) if (inDegree.get(id) === 0) queue.push(id)
  const order: number[] = []
  while (queue.length > 0) {
    const id = queue.shift() as number
    order.push(id)
    for (const dependentId of dependents.get(id) ?? []) {
      const remaining = (inDegree.get(dependentId) ?? 0) - 1
      inDegree.set(dependentId, remaining)
      if (remaining === 0) queue.push(dependentId)
    }
  }

  if (order.length !== closure.size) {
    const cyclic = [...closure].filter((id) => (inDegree.get(id) ?? 0) > 0)
    const names = cyclic.map((id) => byId.get(id)?.name ?? `#${id}`).join(', ')
    return { success: false, order: [], error: `Cycle de dépendances détecté entre : ${names}.` }
  }

  return { success: true, order }
}

/**
 * Ferme `targetIds` sur tous les dépendants transitifs (les features qui
 * dépendent, directement ou indirectement, d'au moins un id de `targetIds`).
 * Utilisé pour la restauration individuelle : désactiver une prérequis
 * implique de désactiver d'abord tout ce qui en dépend encore.
 */
export function collectTrainerFeatureDependents(
  features: TrainerFeatureLike[],
  targetIds: number[],
): number[] {
  const targetSet = new Set(targetIds)
  const stack = [...targetIds]
  while (stack.length > 0) {
    const dependencyId = stack.pop() as number
    for (const feature of features) {
      if (targetSet.has(feature.id)) continue
      if (feature.dependsOn?.includes(dependencyId)) {
        targetSet.add(feature.id)
        stack.push(feature.id)
      }
    }
  }
  return [...targetSet]
}

/**
 * Retourne une nouvelle liste de features où toute référence à `deletedId`
 * dans `dependsOn` a été retirée (le champ devient `undefined` si la liste
 * résultante est vide, jamais un tableau vide -- même convention que
 * `deleteTrainerFeature` dans `ui/src/stores/app.ts`).
 */
export function cleanupDependsOnAfterDelete<T extends TrainerFeatureLike>(
  features: T[],
  deletedId: number,
): T[] {
  return features.map((feature) => {
    if (!feature.dependsOn?.includes(deletedId)) return feature
    const dependsOn = feature.dependsOn.filter((dependencyId) => dependencyId !== deletedId)
    return { ...feature, dependsOn: dependsOn.length === 0 ? undefined : dependsOn }
  })
}

/**
 * Une feature `write`/`clr_write` est un geste one-shot : une fois appliquée,
 * elle ne reste jamais "enabled" (rien à restaurer plus tard). Les autres
 * actions (`freeze_polling`, `freeze_breakpoint`, `patch`) sont togglables :
 * `enabled` doit refléter l'état réel pour qu'Apply all/Restore all sachent
 * quoi (dés)activer. Extrait en fonction pure pour ne pas relire l'anecdote
 * "write ne reste jamais enabled" que dans un commentaire -- voir PHASE 112
 * (les deux `write` de test n'avaient rien à restaurer) vs PHASE 114
 * (`freeze_polling` a un vrai cycle de vie observable).
 */
export function isToggleableTrainerAction(action: string | undefined): boolean {
  return action !== 'write' && action !== 'clr_write'
}
