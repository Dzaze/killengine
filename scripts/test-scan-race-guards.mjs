// UX-PRODUIT-14B -- scenarios deterministes/simules pour la garde anti-course
// requestId de BackendService.startExactScanAsync (voir
// scripts/test-scan-race-guards.ps1 pour comment ce module est prepare).
// Un faux controller scriptable remplace le vrai QWebChannel : ce script
// prouve la logique de correlation/desabonnement elle-meme (deterministe,
// sans dependance Win32), jamais un remplacement de la preuve live
// (scripts/test-behavior-manifest.ps1).
//
// Usage: node scripts/test-scan-race-guards.mjs <path-to-transpiled-mjs>

import { pathToFileURL } from 'node:url'

const modulePath = process.argv[2]
if (!modulePath) {
  console.error('Usage: node test-scan-race-guards.mjs <path-to-transpiled-mjs>')
  process.exit(1)
}

// startExactScanAsync utilise window.setTimeout/clearTimeout directement --
// polyfill minimal avant tout appel (le module peut etre importe sans, mais
// pas appele sans).
const clearTimeoutCalls = []
global.window = {
  setTimeout: (...args) => setTimeout(...args),
  clearTimeout: (id) => { clearTimeoutCalls.push(id); clearTimeout(id) },
}

const { backend } = await import(pathToFileURL(modulePath).href)

let failures = 0

function check(condition, label) {
  if (condition) {
    console.log('OK: ' + label)
  } else {
    failures += 1
    console.error('FAIL: ' + label)
  }
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms))
}

function baseExactScanFields(overrides) {
  return {
    success: true,
    partial: false,
    cancelled: false,
    regionsScanned: 1,
    bytesScanned: 100,
    matchesFound: 1,
    matchesReturned: 1,
    error: '',
    matches: [{ address: '0x1000', type: 'Int32' }],
    candidateStoreSize: 1,
    ...overrides,
  }
}

/** Faux controller QWebChannel-like, entierement scriptable par le test. */
function createFakeController() {
  let handler = null
  let connectCount = 0
  let disconnectCount = 0
  let startImpl = async () => ({ success: false, started: false, error: 'not configured' })

  return {
    scanFinished: {
      connect(h) {
        handler = h
        connectCount += 1
      },
      disconnect(h) {
        if (handler === h) {
          handler = null
        }
        disconnectCount += 1
      },
    },
    async startExactScanAsync(value, valueType, options) {
      return startImpl(value, valueType, options)
    },
    // -- Points de controle du test, pas des membres de BackendController --
    setStartImpl(fn) { startImpl = fn },
    fireScanFinished(payload) {
      if (handler) handler(payload)
    },
    getConnectCount() { return connectCount },
    getDisconnectCount() { return disconnectCount },
  }
}

function installFakeController() {
  const fake = createFakeController()
  // `controller` est privé en TypeScript, mais esbuild ne fait que retirer
  // les annotations de type -- c'est un champ JS normal a l'exécution.
  backend.controller = fake
  return fake
}

// --- Scenario 1 : resultat immediat (scanFinished arrive AVANT que la
// promesse de démarrage ne soit résolue côté appelant) --------------------

{
  const fake = installFakeController()
  fake.setStartImpl(async () => {
    // Simule un backend qui émet scanFinished avant même que cet appel de
    // démarrage ne retourne -- doit être mis en attente (earlyPayloads) puis
    // rejoué une fois le requestId connu, pas perdu.
    fake.fireScanFinished({ requestId: 42, ...baseExactScanFields({ matchesFound: 7 }) })
    return { success: true, started: true, requestId: 42 }
  })

  const result = await backend.startExactScanAsync('100', 'Int32', {})
  check(result.success === true && result.requestId === 42 && result.matchesFound === 7,
    'resultat immediat : signal emis avant resolution du start est mis en attente puis correctement resolu')
  check(fake.getDisconnectCount() === 1, 'resultat immediat : handler desabonne exactement une fois')
}

// --- Scenario 2 : erreur de demarrage --------------------------------------

{
  const fake = installFakeController()
  fake.setStartImpl(async () => ({ success: false, started: false, error: 'Un scan est déjà en cours.' }))

  const result = await backend.startExactScanAsync('100', 'Int32', {})
  check(result.success === false, 'erreur de demarrage : resout avec success=false')
  check(result.error === 'Un scan est déjà en cours.', 'erreur de demarrage : message d\'erreur reel transmis tel quel')
  check(result.matchesFound === 0 && result.matches.length === 0,
    'erreur de demarrage : resultat vide sur les champs numeriques/tableaux, jamais une valeur fabriquee')
  check(fake.getDisconnectCount() === 1, 'erreur de demarrage : handler desabonne (pas de fuite d\'abonnement)')
}

// --- Scenario 3 : signal perime ignore, puis le vrai signal resout --------

{
  const fake = installFakeController()
  fake.setStartImpl(async () => {
    await sleep(5)
    return { success: true, started: true, requestId: 7 }
  })

  const resultPromise = backend.startExactScanAsync('100', 'Int32', {})
  await sleep(20) // laisse le start se resoudre et le requestId=7 se fixer cote appelant.

  // Signal perime : mauvais requestId, doit etre completement ignore.
  fake.fireScanFinished({ requestId: 999, ...baseExactScanFields({ matchesFound: 111 }) })
  // Vrai signal.
  fake.fireScanFinished({ requestId: 7, ...baseExactScanFields({ matchesFound: 3 }) })

  const result = await resultPromise
  check(result.requestId === 7 && result.matchesFound === 3,
    'signal perime : le signal a mauvais requestId est ignore, seul le vrai signal resout la promesse')
}

// --- Scenario 4 : signal duplique, resolu une seule fois -------------------

{
  const fake = installFakeController()
  fake.setStartImpl(async () => {
    await sleep(5)
    return { success: true, started: true, requestId: 11 }
  })

  const resultPromise = backend.startExactScanAsync('100', 'Int32', {})
  await sleep(20)

  fake.fireScanFinished({ requestId: 11, ...baseExactScanFields({ matchesFound: 5 }) })
  // Duplique du meme requestId avec des donnees differentes -- ne doit
  // jamais remplacer la premiere resolution (les Promise ne se resolvent
  // qu'une fois, mais on verifie ici que le handler ne plante pas non plus
  // sur ce second appel et que la donnee retenue est bien la premiere).
  fake.fireScanFinished({ requestId: 11, ...baseExactScanFields({ matchesFound: 999 }) })

  const result = await resultPromise
  check(result.matchesFound === 5,
    'signal duplique : la premiere resolution est retenue, le doublon ne l\'ecrase pas')
  check(fake.getDisconnectCount() === 1, 'signal duplique : desabonnement toujours effectue une seule fois')
}

console.log('')
if (failures > 0) {
  console.error(failures + ' assertion(s) failed.')
  process.exit(1)
}
console.log('All scan race-guard assertions passed.')
