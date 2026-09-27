// UX-PRODUIT-15C -- prédicats de vérification du guide interactif du tutoriel,
// sans dépendance Pinia/backend pour rester testables isolément (voir
// scripts/test-tutorial-steps.ps1). Une étape du guide n'avance jamais sur un
// simple clic "suivant" : elle avance uniquement quand un de ces prédicats
// retourne vrai contre l'état réel observé (candidats de scan, résultat
// d'écriture, valeur relue depuis la cible démo, entrées de profil).
//
// Règle dure respectée par tout appelant de ce module : les champs de
// TutorialGroundTruth ne doivent jamais être interpolés tels quels dans un
// texte affiché à l'utilisateur -- seuls les booléens/raisons retournés ici
// peuvent l'être. La cible démo affiche déjà sa propre valeur dans sa
// fenêtre (voir apps/demo/demo_target_main.cpp) : c'est là que l'utilisateur
// doit la lire, pas dans le guide.

export interface TutorialGroundTruth {
  healthAddress: string
  decoyAlphaAddress: string
  decoyBetaAddress: string
  health: number
}

export interface TutorialWriteResultLike {
  success: boolean
  address?: string
  results?: Array<{ success: boolean; address?: string }>
}

export type CandidateMatchReason = 'empty' | 'decoyPresent' | 'notFound' | 'tooMany'

const kMaxPlausibleCandidates = 25

/** Normalise une adresse hex pour comparaison : minuscules, sans préfixe 0x, sans zéros de tête. */
export function normalizeAddress(addr: string): string {
  const stripped = addr.trim().toLowerCase().replace(/^0x/, '')
  const withoutLeadingZeros = stripped.replace(/^0+(?=.)/, '')
  return withoutLeadingZeros
}

/**
 * Étape "Chercher puis varier" : le scan (exact puis suivant) doit avoir
 * convergé vers l'adresse santé, sans que les leurres (qui ne changent
 * jamais) ne soient encore présents dans le résultat.
 */
export function candidatesMatchHealthOnly(
  candidateAddresses: string[],
  truth: TutorialGroundTruth,
): { ok: boolean; reason?: CandidateMatchReason } {
  if (candidateAddresses.length === 0) {
    return { ok: false, reason: 'empty' }
  }
  const normalized = candidateAddresses.map(normalizeAddress)
  const health = normalizeAddress(truth.healthAddress)
  const decoyAlpha = normalizeAddress(truth.decoyAlphaAddress)
  const decoyBeta = normalizeAddress(truth.decoyBetaAddress)
  if (normalized.includes(decoyAlpha) || normalized.includes(decoyBeta)) {
    return { ok: false, reason: 'decoyPresent' }
  }
  if (!normalized.includes(health)) {
    return { ok: false, reason: 'notFound' }
  }
  if (normalized.length > kMaxPlausibleCandidates) {
    return { ok: false, reason: 'tooMany' }
  }
  return { ok: true }
}

/** Étape "Affiner puis sélectionner" : réutilise le même critère de convergence. */
export function selectionMatchesHealthOnly(
  selectedAddresses: string[],
  truth: TutorialGroundTruth,
): { ok: boolean; reason?: CandidateMatchReason } {
  return candidatesMatchHealthOnly(selectedAddresses, truth)
}

/** Étape "Écrire" : au moins une écriture réussie a bien ciblé l'adresse santé. */
export function writeTargetsHealthAddress(
  writeResult: TutorialWriteResultLike | null | undefined,
  truth: TutorialGroundTruth,
): boolean {
  if (!writeResult) return false
  const writtenAddresses: string[] = []
  if (writeResult.results && writeResult.results.length > 0) {
    for (const entry of writeResult.results) {
      if (entry.success && entry.address) writtenAddresses.push(entry.address)
    }
  } else if (writeResult.success && writeResult.address) {
    writtenAddresses.push(writeResult.address)
  }
  if (writtenAddresses.length === 0) return false
  const health = normalizeAddress(truth.healthAddress)
  return writtenAddresses.map(normalizeAddress).includes(health)
}

/** Étape "Vérifier l'effet" : la valeur relue sur la cible correspond à ce qui a été écrit. */
export function effectMatchesWrite(observedHealth: number, expectedHealth: number): boolean {
  return Number.isFinite(observedHealth) && Number.isFinite(expectedHealth) && observedHealth === expectedHealth
}

/**
 * Étape "Sauvegarder" : le profil rechargé contient bien une cible portant
 * le nom donné par l'utilisateur au moment de la sauvegarde. Le profil
 * stocke un locator (pas forcément une adresse brute lisible -- absolu,
 * module+offset ou chaîne de pointeurs selon ce que le backend choisit),
 * donc on vérifie par nom plutôt que par ré-analyse du format du locator.
 */
export function profileEntrySavedByName(
  entries: Array<{ name: string }>,
  targetName: string,
): boolean {
  return entries.some((entry) => entry.name === targetName)
}

/**
 * Étape "Retrouver" : après redémarrage de la cible démo, le profil rechargé
 * doit s'être résolu vers une adresse plausible (non vide) -- l'adresse peut
 * changer d'un lancement à l'autre (ASLR), ce n'est pas comparé à l'ancienne.
 */
export function profileResolvedAfterRestart(resolvedAddressHex: string | null | undefined): boolean {
  return !!resolvedAddressHex && resolvedAddressHex.trim().length > 0
}
