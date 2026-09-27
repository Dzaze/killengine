// Automated tests for ui/src/stores/tutorialSteps.ts (UX-PRODUIT-15C).
// Imports the real transpiled module so there is no risk of the test
// drifting from production logic. See scripts/test-tutorial-steps.ps1 for
// the transpile step (same esbuild-via-Vite pattern as
// scripts/test-trainer-dependencies.ps1).
//
// Usage: node scripts/test-tutorial-steps.mjs <path-to-transpiled-mjs>

import { pathToFileURL } from 'node:url'

const modulePath = process.argv[2]
if (!modulePath) {
  console.error('Usage: node test-tutorial-steps.mjs <path-to-transpiled-mjs>')
  process.exit(1)
}

const {
  normalizeAddress,
  candidatesMatchHealthOnly,
  selectionMatchesHealthOnly,
  writeTargetsHealthAddress,
  effectMatchesWrite,
  profileEntrySavedByName,
  profileResolvedAfterRestart,
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

const truth = {
  healthAddress: '0x00007FF6A1B2C3D0',
  decoyAlphaAddress: '0x00007FF6A1B2C400',
  decoyBetaAddress: '0x00007FF6A1B2C420',
  health: 100,
}

// --- normalizeAddress -------------------------------------------------------

{
  check(normalizeAddress('0x00007FF6A1B2C3D0') === normalizeAddress('7ff6a1b2c3d0'),
    'normalizeAddress: prefixe 0x et casse ignores')
  check(normalizeAddress('0x0000ABC') === normalizeAddress('ABC'),
    'normalizeAddress: zeros de tete ignores')
}

// --- candidatesMatchHealthOnly ----------------------------------------------

{
  const r = candidatesMatchHealthOnly([], truth)
  check(r.ok === false && r.reason === 'empty', 'candidatesMatchHealthOnly: liste vide refusee (empty)')
}

{
  const r = candidatesMatchHealthOnly([truth.healthAddress, truth.decoyAlphaAddress], truth)
  check(r.ok === false && r.reason === 'decoyPresent', 'candidatesMatchHealthOnly: leurre encore present refuse')
}

{
  const r = candidatesMatchHealthOnly(['0x1234'], truth)
  check(r.ok === false && r.reason === 'notFound', 'candidatesMatchHealthOnly: adresse sante absente refusee')
}

{
  const many = Array.from({ length: 30 }, (_, i) => '0x' + (1000 + i).toString(16))
  many.push(truth.healthAddress)
  const r = candidatesMatchHealthOnly(many, truth)
  check(r.ok === false && r.reason === 'tooMany', 'candidatesMatchHealthOnly: trop de candidats refuse (tooMany)')
}

{
  const r = candidatesMatchHealthOnly([truth.healthAddress], truth)
  check(r.ok === true, 'candidatesMatchHealthOnly: convergence exacte vers la sante acceptee')
}

{
  // Casse/prefixe differents entre backend et frontend -- doit quand meme matcher.
  const r = candidatesMatchHealthOnly(['7ff6a1b2c3d0'], truth)
  check(r.ok === true, 'candidatesMatchHealthOnly: match malgre casse/prefixe differents')
}

// --- selectionMatchesHealthOnly (meme critere, alias pour l'etape Affiner) -

{
  const r = selectionMatchesHealthOnly([truth.healthAddress], truth)
  check(r.ok === true, 'selectionMatchesHealthOnly: reutilise le meme critere de convergence')
}

// --- writeTargetsHealthAddress ----------------------------------------------

{
  check(writeTargetsHealthAddress(null, truth) === false, 'writeTargetsHealthAddress: resultat nul refuse')
  check(writeTargetsHealthAddress(undefined, truth) === false, 'writeTargetsHealthAddress: resultat undefined refuse')
}

{
  const result = { success: true, address: truth.healthAddress }
  check(writeTargetsHealthAddress(result, truth) === true, 'writeTargetsHealthAddress: ecriture simple sur la bonne adresse acceptee')
}

{
  const result = { success: true, address: truth.decoyAlphaAddress }
  check(writeTargetsHealthAddress(result, truth) === false, 'writeTargetsHealthAddress: ecriture simple sur un leurre refusee')
}

{
  const result = { success: false, address: truth.healthAddress }
  check(writeTargetsHealthAddress(result, truth) === false, 'writeTargetsHealthAddress: ecriture non reussie refusee')
}

{
  // Ecriture par lot : seule une des entrees cible reellement la sante.
  const result = {
    success: true,
    results: [
      { success: true, address: truth.decoyAlphaAddress },
      { success: true, address: truth.healthAddress },
      { success: false, address: truth.decoyBetaAddress },
    ],
  }
  check(writeTargetsHealthAddress(result, truth) === true, 'writeTargetsHealthAddress: ecriture par lot avec une entree sante reussie acceptee')
}

{
  const result = { success: true, results: [{ success: false, address: truth.healthAddress }] }
  check(writeTargetsHealthAddress(result, truth) === false, 'writeTargetsHealthAddress: entree sante presente mais non reussie refusee')
}

// --- effectMatchesWrite ------------------------------------------------------

{
  check(effectMatchesWrite(90, 90) === true, 'effectMatchesWrite: valeur relue egale a la valeur attendue acceptee')
  check(effectMatchesWrite(100, 90) === false, 'effectMatchesWrite: valeur pas encore a jour refusee')
  check(effectMatchesWrite(Number.NaN, 90) === false, 'effectMatchesWrite: valeur observee non finie refusee')
}

// --- profileEntrySavedByName --------------------------------------------------

{
  check(profileEntrySavedByName([], 'Sante demo') === false, 'profileEntrySavedByName: aucune entree refusee')
  check(profileEntrySavedByName([{ name: 'Autre cible' }], 'Sante demo') === false,
    'profileEntrySavedByName: nom different refuse')
  check(profileEntrySavedByName([{ name: 'Autre cible' }, { name: 'Sante demo' }], 'Sante demo') === true,
    'profileEntrySavedByName: une entree parmi plusieurs porte bien le bon nom')
}

// --- profileResolvedAfterRestart ---------------------------------------------

{
  check(profileResolvedAfterRestart(null) === false, 'profileResolvedAfterRestart: null refuse')
  check(profileResolvedAfterRestart('') === false, 'profileResolvedAfterRestart: chaine vide refusee')
  check(profileResolvedAfterRestart('  ') === false, 'profileResolvedAfterRestart: chaine blanche refusee')
  check(profileResolvedAfterRestart('0x00007FF7B2C3D4E0') === true, 'profileResolvedAfterRestart: nouvelle adresse post-redemarrage acceptee')
}

console.log('')
if (failures > 0) {
  console.error(failures + ' assertion(s) failed.')
  process.exit(1)
}
console.log('All tutorial step assertions passed.')
