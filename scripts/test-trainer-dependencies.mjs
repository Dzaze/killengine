// Automated tests for ui/src/stores/trainerDependencies.ts (PHASE 103/111/112/114).
// Not a reimplementation: imports the real transpiled module so there is no
// risk of the test drifting from production logic. See
// scripts/test-trainer-dependencies.ps1 for how the transpile step works
// (ui/ has no JS/TS test runner configured -- this uses the esbuild binary
// already vendored by Vite instead of adding a new dependency).
//
// Usage: node scripts/test-trainer-dependencies.mjs <path-to-transpiled-mjs>

import { pathToFileURL } from 'node:url'

const modulePath = process.argv[2]
if (!modulePath) {
  console.error('Usage: node test-trainer-dependencies.mjs <path-to-transpiled-mjs>')
  process.exit(1)
}

const {
  resolveTrainerFeatureOrder,
  collectTrainerFeatureDependents,
  cleanupDependsOnAfterDelete,
  isToggleableTrainerAction,
} = await import(pathToFileURL(modulePath).href)

let failures = 0

function check(condition, label) {
  if (condition) {
    console.log('OK: ' + label)
  } else {
    failures += 1
    console.error('FAIL: ' + label)
  }
}

function deepEqual(a, b) {
  return JSON.stringify(a) === JSON.stringify(b)
}

// --- resolveTrainerFeatureOrder --------------------------------------------

{
  // Chaine lineaire A <- B <- C
  const features = [
    { id: 1, name: 'A', dependsOn: [] },
    { id: 2, name: 'B', dependsOn: [1] },
    { id: 3, name: 'C', dependsOn: [2] },
  ]
  const r = resolveTrainerFeatureOrder(features, [3])
  check(r.success, 'chaine lineaire: resout')
  check(deepEqual(r.order, [1, 2, 3]), 'chaine lineaire: ordre Apply all correct (A, B, C)')
  check(deepEqual([...r.order].reverse(), [3, 2, 1]), 'chaine lineaire: ordre Restore all inverse (C, B, A)')
}

{
  // Diamant : D depend de B et C, B et C dependent de A
  const features = [
    { id: 1, name: 'A', dependsOn: [] },
    { id: 2, name: 'B', dependsOn: [1] },
    { id: 3, name: 'C', dependsOn: [1] },
    { id: 4, name: 'D', dependsOn: [2, 3] },
  ]
  const r = resolveTrainerFeatureOrder(features, [4])
  check(r.success, 'diamant: resout')
  check(r.order[0] === 1, 'diamant: A (racine) en premier a Apply all')
  check(r.order[r.order.length - 1] === 4, 'diamant: D (feuille) en dernier a Apply all')
  check(r.order.length === 4, 'diamant: closure complete (A, B, C, D)')
  const restoreOrder = [...r.order].reverse()
  check(restoreOrder[0] === 4, 'diamant: D en premier a Restore all (dependant avant prerequis)')
  check(restoreOrder[restoreOrder.length - 1] === 1, 'diamant: A en dernier a Restore all')
}

{
  // Cycle : X depend de Y, Y depend de X
  const features = [
    { id: 1, name: 'X', dependsOn: [2] },
    { id: 2, name: 'Y', dependsOn: [1] },
  ]
  const r = resolveTrainerFeatureOrder(features, [1])
  check(!r.success, 'cycle: refuse proprement (success=false)')
  check(typeof r.error === 'string' && r.error.includes('Cycle'), 'cycle: message erreur explicite mentionne "Cycle"')
}

{
  // Reference manquante
  const features = [{ id: 1, name: 'Z', dependsOn: [99] }]
  const r = resolveTrainerFeatureOrder(features, [1])
  check(!r.success, 'reference manquante: refusee proprement (success=false)')
  check(typeof r.error === 'string' && r.error.includes('introuvable'), 'reference manquante: message erreur explicite')
}

{
  // targetIds multiples, cible independante
  const features = [
    { id: 1, name: 'A', dependsOn: [] },
    { id: 2, name: 'B', dependsOn: [1] },
    { id: 3, name: 'C', dependsOn: [] },
  ]
  const r = resolveTrainerFeatureOrder(features, [2, 3])
  check(r.success, 'targets multiples: resout')
  check(r.order.indexOf(1) < r.order.indexOf(2), 'targets multiples: A avant B (dependance respectee)')
  check(r.order.includes(3), 'targets multiples: C (independante) presente dans le resultat')
}

// --- collectTrainerFeatureDependents ----------------------------------------

{
  // A <- B <- C : desactiver A doit d'abord desactiver B puis C
  const features = [
    { id: 1, name: 'A', dependsOn: [] },
    { id: 2, name: 'B', dependsOn: [1] },
    { id: 3, name: 'C', dependsOn: [2] },
  ]
  const dependents = collectTrainerFeatureDependents(features, [1])
  check(new Set(dependents).size === 3, 'dependents transitifs: A + B + C toutes incluses (3 ids)')
  check(dependents.includes(1) && dependents.includes(2) && dependents.includes(3), 'dependents transitifs: contient bien A, B et C')
}

{
  // Feature independante : aucun dependant
  const features = [
    { id: 1, name: 'A', dependsOn: [] },
    { id: 2, name: 'B', dependsOn: [] },
  ]
  const dependents = collectTrainerFeatureDependents(features, [1])
  check(deepEqual(dependents, [1]), 'dependents transitifs: feature sans dependant retourne juste elle-meme')
}

// --- cleanupDependsOnAfterDelete --------------------------------------------

{
  // B depend de A (a supprimer) et de C (a garder)
  const features = [
    { id: 2, name: 'B', dependsOn: [1, 3] },
    { id: 3, name: 'C', dependsOn: [] },
  ]
  const cleaned = cleanupDependsOnAfterDelete(features, 1)
  const b = cleaned.find((f) => f.id === 2)
  check(deepEqual(b.dependsOn, [3]), 'suppression: reference morte retiree, reference valide conservee')
}

{
  // B ne depend que de A (a supprimer) -> dependsOn doit devenir undefined, jamais []
  const features = [{ id: 2, name: 'B', dependsOn: [1] }]
  const cleaned = cleanupDependsOnAfterDelete(features, 1)
  check(cleaned[0].dependsOn === undefined, 'suppression: dependsOn devient undefined (pas []) quand plus aucune dependance')
}

{
  // Feature non concernee par la suppression : reference identique (pas de copie inutile)
  const features = [{ id: 5, name: 'Untouched', dependsOn: [99] }]
  const cleaned = cleanupDependsOnAfterDelete(features, 1)
  check(cleaned[0] === features[0], 'suppression: feature non concernee retournee sans copie (reference identique)')
}

// --- isToggleableTrainerAction ----------------------------------------------

{
  check(isToggleableTrainerAction('write') === false, 'action write: one-shot, jamais togglable')
  check(isToggleableTrainerAction('clr_write') === false, 'action clr_write: one-shot, jamais togglable')
  check(isToggleableTrainerAction('freeze_polling') === true, 'action freeze_polling: togglable (PHASE 114)')
  check(isToggleableTrainerAction('freeze_breakpoint') === true, 'action freeze_breakpoint: togglable')
  check(isToggleableTrainerAction('patch') === true, 'action patch: togglable')
}

// --- Bout en bout : simule Apply all puis Restore all avec une feature togglable, comme PHASE 114 ---

{
  const features = [
    { id: 1, name: 'FreezeBase', action: 'freeze_polling', enabled: false, dependsOn: [] },
    { id: 2, name: 'FreezeDependent', action: 'freeze_polling', enabled: false, dependsOn: [1] },
  ]

  // Apply all : cible tout ce qui n'est pas enabled
  const applyTargets = features.filter((f) => !f.enabled).map((f) => f.id)
  const applyResolved = resolveTrainerFeatureOrder(features, applyTargets)
  check(applyResolved.success, 'bout-en-bout Apply all: resolution OK')
  check(deepEqual(applyResolved.order, [1, 2]), 'bout-en-bout Apply all: FreezeBase avant FreezeDependent')
  // Simule l'application reelle (comme doApplyTrainerFeature) : ok=true, action togglable -> enabled=true
  for (const id of applyResolved.order) {
    const feature = features.find((f) => f.id === id)
    feature.enabled = true && isToggleableTrainerAction(feature.action)
  }
  check(features.every((f) => f.enabled === true), 'bout-en-bout Apply all: les deux features passent enabled=true')

  // Restore all : cible tout ce qui est enabled
  const restoreTargets = features.filter((f) => f.enabled).map((f) => f.id)
  const restoreResolved = resolveTrainerFeatureOrder(features, restoreTargets)
  check(restoreResolved.success, 'bout-en-bout Restore all: resolution OK')
  const restoreOrder = [...restoreResolved.order].reverse()
  check(deepEqual(restoreOrder, [2, 1]), 'bout-en-bout Restore all: FreezeDependent avant FreezeBase (ordre inverse)')
  for (const id of restoreOrder) {
    const feature = features.find((f) => f.id === id)
    feature.enabled = false
  }
  check(features.every((f) => f.enabled === false), 'bout-en-bout Restore all: les deux features repassent enabled=false')
}

console.log('')
if (failures > 0) {
  console.error(failures + ' assertion(s) failed.')
  process.exit(1)
}
console.log('All trainer dependency assertions passed.')
