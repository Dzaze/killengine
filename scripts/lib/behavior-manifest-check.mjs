// UX-PRODUIT-14B -- vérificateur pur de champs pour le manifeste de
// comportements (scripts/contracts/behavior-manifest.json). Ne fait aucun
// appel réseau/pipe : reçoit une réponse JSON déjà obtenue (par
// test-behavior-manifest.ps1, via un vrai appel pipe, ou par tout autre
// appelant) et vérifie qu'elle satisfait les champs requis déclarés dans le
// manifeste. Réutilisé par test-behavior-manifest.ps1 (réponses réelles) et
// testable isolément (voir scripts/test-behavior-manifest-check.mjs).

/**
 * @param {string} path Chemin pointé, ex. "stats.totalDataPoints" ou
 *   "series.0.id" (un segment numérique est traité comme un index de tableau).
 */
function getPath(obj, path) {
  const segments = path.split('.')
  let current = obj
  for (const segment of segments) {
    if (current === null || current === undefined) return undefined
    current = current[segment]
  }
  return current
}

function actualType(value) {
  if (value === null) return 'null'
  if (Array.isArray(value)) return 'array'
  return typeof value
}

function matchesType(value, type) {
  switch (type) {
    case 'string': return typeof value === 'string'
    case 'number': return typeof value === 'number' && Number.isFinite(value)
    case 'boolean': return typeof value === 'boolean'
    case 'array': return Array.isArray(value)
    case 'object': return typeof value === 'object' && value !== null && !Array.isArray(value)
    case 'null': return value === null
    default: return true // type inconnu du checker -- ne bloque pas, laisse passer explicitement.
  }
}

function deepEqual(a, b) {
  return JSON.stringify(a) === JSON.stringify(b)
}

/**
 * @param {unknown} response La réponse JSON réelle (déjà parsée) à vérifier.
 * @param {Array<{path: string, type?: string, expected?: unknown, optional?: boolean}>} requiredFields
 * @returns {Array<{path: string, reason: string}>} Liste des violations -- vide si tout est conforme.
 */
export function checkFields(response, requiredFields) {
  const violations = []
  for (const field of requiredFields) {
    const { path, type, expected, optional } = field
    const value = getPath(response, path)
    if (value === undefined) {
      if (!optional) {
        violations.push({ path, reason: 'champ manquant' })
      }
      continue
    }
    if (type && !matchesType(value, type)) {
      violations.push({ path, reason: `type incorrect : attendu ${type}, reçu ${actualType(value)}` })
      continue
    }
    if (expected !== undefined && !deepEqual(value, expected)) {
      violations.push({ path, reason: `valeur inattendue : attendu ${JSON.stringify(expected)}, reçu ${JSON.stringify(value)}` })
    }
  }
  return violations
}

/**
 * Charge un scénario nommé depuis un manifeste déjà parsé
 * (scripts/contracts/behavior-manifest.json), lève une erreur explicite s'il
 * n'existe pas -- jamais un scénario introuvable silencieusement ignoré.
 */
export function findScenario(manifest, apiId, scenarioId) {
  const api = manifest.apis.find((a) => a.id === apiId)
  if (!api) {
    throw new Error(`API inconnue dans le manifeste : ${apiId}`)
  }
  const scenario = api.scenarios.find((s) => s.id === scenarioId)
  if (!scenario) {
    throw new Error(`Scénario inconnu dans le manifeste : ${apiId}/${scenarioId}`)
  }
  return scenario
}
