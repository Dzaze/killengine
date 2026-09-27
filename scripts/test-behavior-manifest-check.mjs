// UX-PRODUIT-14B -- tests unitaires purs du vérificateur de champs
// (scripts/lib/behavior-manifest-check.mjs). Pas de transpilation esbuild
// nécessaire ici (le module est déjà du .mjs, pas du .ts) -- invoqué
// directement par scripts/test-behavior-manifest-check.ps1.

import { checkFields, findScenario } from './lib/behavior-manifest-check.mjs'

let failures = 0

function check(condition, label) {
  if (condition) {
    console.log('OK: ' + label)
  } else {
    failures += 1
    console.error('FAIL: ' + label)
  }
}

// --- checkFields --------------------------------------------------------

{
  const violations = checkFields({ success: true }, [{ path: 'success', type: 'boolean' }])
  check(violations.length === 0, 'checkFields: champ present du bon type -- aucune violation')
}

{
  const violations = checkFields({}, [{ path: 'success', type: 'boolean' }])
  check(violations.length === 1 && violations[0].reason === 'champ manquant',
    'checkFields: champ manquant detecte')
}

{
  const violations = checkFields({ success: 'oui' }, [{ path: 'success', type: 'boolean' }])
  check(violations.length === 1 && violations[0].reason.includes('type incorrect'),
    'checkFields: mauvais type detecte')
}

{
  const violations = checkFields({ stats: { totalDataPoints: 42 } }, [
    { path: 'stats.totalDataPoints', type: 'number' },
  ])
  check(violations.length === 0, 'checkFields: chemin imbrique resolu correctement')
}

{
  const violations = checkFields({ series: [{ id: 'a' }, { id: 'b' }] }, [
    { path: 'series.0.id', type: 'string', expected: 'a' },
    { path: 'series.1.id', type: 'string', expected: 'b' },
  ])
  check(violations.length === 0, 'checkFields: index de tableau resolu correctement')
}

{
  const violations = checkFields({ stopReason: 'user_stop' }, [
    { path: 'stopReason', type: 'string', expected: 'duration_reached' },
  ])
  check(violations.length === 1 && violations[0].reason.includes('valeur inattendue'),
    'checkFields: valeur attendue differente detectee')
}

{
  const violations = checkFields({}, [{ path: 'optionalField', type: 'string', optional: true }])
  check(violations.length === 0, 'checkFields: champ optionnel absent -- aucune violation')
}

{
  const violations = checkFields(null, [{ path: 'a.b', type: 'string' }])
  check(violations.length === 1 && violations[0].reason === 'champ manquant',
    'checkFields: reponse null ne plante pas, chemin traite comme manquant')
}

// --- findScenario ---------------------------------------------------------

const manifest = {
  apis: [
    { id: 'getTimelineStatus', scenarios: [{ id: 'default', requiredFields: [] }] },
  ],
}

{
  const scenario = findScenario(manifest, 'getTimelineStatus', 'default')
  check(scenario.id === 'default', 'findScenario: scenario existant retourne')
}

{
  let threw = false
  try {
    findScenario(manifest, 'inconnu', 'default')
  } catch {
    threw = true
  }
  check(threw, 'findScenario: API inconnue leve une erreur explicite')
}

{
  let threw = false
  try {
    findScenario(manifest, 'getTimelineStatus', 'inconnu')
  } catch {
    threw = true
  }
  check(threw, 'findScenario: scenario inconnu leve une erreur explicite')
}

console.log('')
if (failures > 0) {
  console.error(failures + ' assertion(s) failed.')
  process.exit(1)
}
console.log('All behavior-manifest-check assertions passed.')
