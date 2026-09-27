// UX-PRODUIT-14A (docs/PHASE_TRACKER.md) -- orchestrateur du contrôle de
// contrat backend. Toujours exécute l'extraction TS statique (pure, hors
// application) ; l'inventaire C++ réel est optionnel (fourni par
// scripts/test-backend-contract.ps1 via un appel pipe réel à
// describeBackendContract()) -- absent, ce script rapporte SKIPPED pour la
// partie runtime sans faire échouer les contrôles statiques.
//
// Usage: node scripts/test-backend-contract.mjs <backendTsPath> [<cppContractJsonPath>]

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { extractBackendContract } from './lib/backend-contract-ts.mjs'
import { compareBackendContract } from './lib/backend-contract-compare.mjs'

const scriptDir = path.dirname(fileURLToPath(import.meta.url))
const backendTsPath = process.argv[2]
const cppContractJsonPath = process.argv[3]

if (!backendTsPath) {
  console.error('Usage: node test-backend-contract.mjs <backendTsPath> [<cppContractJsonPath>]')
  process.exit(2)
}

const exceptionsPath = path.join(scriptDir, 'contracts', 'backend-contract-exceptions.json')
let exceptions = {}
if (fs.existsSync(exceptionsPath)) {
  exceptions = JSON.parse(fs.readFileSync(exceptionsPath, 'utf8'))
}

function printList(title, items, formatter) {
  console.log(`\n${title} (${items.length})`)
  for (const item of items) {
    console.log(`  - ${formatter(item)}`)
  }
}

let overallStatus = 'PASS'

console.log('==> [RUN] Extraction statique TypeScript (BackendController + createMockBackend)')
let tsContract
try {
  tsContract = extractBackendContract(backendTsPath)
} catch (error) {
  console.error(`==> [FAIL] Extraction TypeScript : ${error.message}`)
  process.exit(1)
}
console.log(`==> [OK]  ${tsContract.methods.length} méthodes, ${tsContract.properties.length} propriétés, ${tsContract.signals.length} signaux, ${tsContract.mockKeys.length} clés mock`)

if (!cppContractJsonPath || !fs.existsSync(cppContractJsonPath)) {
  console.log('\n==> [SKIPPED] Inventaire C++ réel (aucun contrat backend fourni -- pipe indisponible ou -SkipAutomationPipe).')
  console.log('    Les contrôles statiques TS ci-dessus restent valides ; la comparaison C++<->TS n\'a PAS été faite cette exécution.')
  console.log('\nRésultat global : SKIPPED (partiel -- statique OK, runtime non exécuté)')
  process.exit(0)
}

console.log('\n==> [RUN] Comparaison C++ <-> TypeScript <-> mock')
const rawCpp = JSON.parse(fs.readFileSync(cppContractJsonPath, 'utf8'))
const cppContract = rawCpp.result ?? rawCpp // tolère un fichier brut {methods,...} ou une enveloppe {id,result:{...}}
console.log(`==> [OK]  Inventaire C++ chargé : ${cppContract.methods.length} méthodes, ${cppContract.properties.length} propriétés, ${cppContract.signals.length} signaux`)

const findings = compareBackendContract(cppContract, tsContract, exceptions)

const blockingCount = findings.missingBackend.length + findings.missingTs.length + findings.incompatible.length

if (findings.missingBackend.length > 0) {
  overallStatus = 'FAIL'
  printList('MANQUANT CÔTÉ BACKEND (TS déclare, C++ n\'a pas)', findings.missingBackend, (f) => `[${f.kind}] ${f.name} -- ${f.reason}`)
}
if (findings.missingTs.length > 0) {
  overallStatus = 'FAIL'
  printList('MANQUANT CÔTÉ TS (C++ a, TS ne déclare pas)', findings.missingTs, (f) => `[${f.kind}] ${f.name} -- ${f.reason}`)
}
if (findings.incompatible.length > 0) {
  overallStatus = 'FAIL'
  printList('INCOMPATIBLE (présent des deux côtés, signature/type divergent)', findings.incompatible, (f) => `[${f.kind}] ${f.name} -- ${f.reason}`)
}

// missingMock et warnings : toujours calculés et affichés en entier (jamais
// ignorés, voir la politique documentée dans backend-contract-compare.mjs),
// mais NE font PAS échouer le run par défaut -- volume attendu élevé de
// méthodes optionnelles légitimement non mockées.
if (findings.missingMock.length > 0) {
  printList('SANS MOCK (informationnel, ne fait pas échouer le contrôle)', findings.missingMock, (f) => `[${f.kind}] ${f.name}${f.optional ? ' (optionnel)' : ' (REQUIS -- ne devrait jamais arriver, tsc l\'empêche normalement)'}`)
}
if (findings.warnings.length > 0) {
  printList('AVERTISSEMENTS (précision JS 64 bits, ne fait pas échouer le contrôle)', findings.warnings, (f) => `[${f.kind}] ${f.name} -- ${f.reason}`)
}

console.log(`\n==> Résumé : ${blockingCount} finding(s) bloquant(s), ${findings.missingMock.length} sans mock (info), ${findings.warnings.length} avertissement(s).`)

if (overallStatus === 'FAIL') {
  console.log('\nRésultat global : FAIL')
  process.exit(1)
}
console.log('\nRésultat global : PASS')
process.exit(0)
