// Tests synthétiques purs de scripts/lib/backend-contract-compare.mjs
// (UX-PRODUIT-14A). Fixtures minimales, pas de vrai backend.ts/C++ ici --
// voir scripts/test-backend-contract.ps1 pour l'exécution contre le code réel.
import { compareBackendContract, qtTypeBucket, tsTypeMatchesBucket, unwrapPromise } from './lib/backend-contract-compare.mjs'

let failures = 0
function check(condition, label) {
  if (condition) console.log('OK: ' + label)
  else { failures += 1; console.error('FAIL: ' + label) }
}

check(qtTypeBucket('QString') === 'string', 'qtTypeBucket: QString -> string')
check(qtTypeBucket('qint64') === 'number64', 'qtTypeBucket: qint64 -> number64 (precision bucket)')
check(qtTypeBucket('QVariantMap') === 'object', 'qtTypeBucket: QVariantMap -> object')
check(qtTypeBucket('SomeCustomEnum') === 'unverified', 'qtTypeBucket: unknown type -> unverified (never flagged)')

check(unwrapPromise('Promise<string>') === 'string', 'unwrapPromise: strips Promise<>')
check(unwrapPromise('string') === 'string', 'unwrapPromise: passthrough when not wrapped')

check(tsTypeMatchesBucket('string', 'string') === true, 'tsTypeMatchesBucket: string/string matches')
check(tsTypeMatchesBucket('number', 'string') === false, 'tsTypeMatchesBucket: number/string does not match')
check(tsTypeMatchesBucket('Record<string, unknown>', 'object') === true, 'tsTypeMatchesBucket: Record<> matches object bucket')
check(tsTypeMatchesBucket('ProcessInfo[]', 'array') === true, 'tsTypeMatchesBucket: named array type matches array bucket')
check(tsTypeMatchesBucket('{ success: boolean; error?: string }', 'object') === true, 'tsTypeMatchesBucket: inline object literal type matches object bucket')
check(tsTypeMatchesBucket('number', 'number64') === true, 'tsTypeMatchesBucket: number64 accepts plain number (warning, not incompatible)')

function cppMethod(name, arity, returnType, parameterTypes = []) {
  return { name, arity, returnType, parameterTypes }
}
function tsMethod(name, minArity, maxArity, returnType, optional = false) {
  return { name, minArity, maxArity, returnType, paramTypes: [], optional }
}

// --- missingBackend: TS declares something C++ doesn't have ---
{
  const cpp = { methods: [], properties: [], signals: [] }
  const ts = { methods: [tsMethod('ghostMethod', 0, 0, 'Promise<void>')], properties: [], signals: [], mockKeys: [] }
  const findings = compareBackendContract(cpp, ts)
  check(findings.missingBackend.some((f) => f.name === 'ghostMethod'), 'missingBackend: TS-only method detected')
}

// --- missingTs: C++ has something TS doesn't declare ---
{
  const cpp = { methods: [cppMethod('secretMethod', 0, 'void')], properties: [], signals: [] }
  const ts = { methods: [], properties: [], signals: [], mockKeys: [] }
  const findings = compareBackendContract(cpp, ts)
  check(findings.missingTs.some((f) => f.name === 'secretMethod'), 'missingTs: C++-only method detected')
}

// --- incompatible: arity out of range ---
{
  const cpp = { methods: [cppMethod('doThing', 3, 'void')], properties: [], signals: [] }
  const ts = { methods: [tsMethod('doThing', 0, 1, 'Promise<void>')], properties: [], signals: [], mockKeys: ['doThing'] }
  const findings = compareBackendContract(cpp, ts)
  check(findings.incompatible.some((f) => f.name === 'doThing'), 'incompatible: arity range mismatch detected')
}

// --- incompatible: return type bucket mismatch ---
{
  const cpp = { methods: [cppMethod('isReady', 0, 'bool')], properties: [], signals: [] }
  const ts = { methods: [tsMethod('isReady', 0, 0, 'Promise<string>')], properties: [], signals: [], mockKeys: ['isReady'] }
  const findings = compareBackendContract(cpp, ts)
  check(findings.incompatible.some((f) => f.name === 'isReady'), 'incompatible: bool vs string return type detected')
}

// --- compatible: default-arg overload (2 C++ arities), TS declares only the smaller one ---
{
  const cpp = { methods: [cppMethod('withDefault', 1, 'QVariantMap'), cppMethod('withDefault', 2, 'QVariantMap')], properties: [], signals: [] }
  const ts = { methods: [tsMethod('withDefault', 1, 1, 'Promise<Record<string, unknown>>')], properties: [], signals: [], mockKeys: ['withDefault'] }
  const findings = compareBackendContract(cpp, ts)
  check(!findings.incompatible.some((f) => f.name === 'withDefault'), 'compatible: TS matching one of several C++ overload arities is not incompatible')
}

// --- missingMock: computed and reported, never silently dropped ---
{
  const cpp = { methods: [cppMethod('optionalThing', 0, 'void')], properties: [], signals: [] }
  const ts = { methods: [tsMethod('optionalThing', 0, 0, 'Promise<void>', true)], properties: [], signals: [], mockKeys: [] }
  const findings = compareBackendContract(cpp, ts)
  check(findings.missingMock.some((f) => f.name === 'optionalThing'), 'missingMock: unmocked optional method is reported, not ignored')
}

// --- exceptions: a documented exception suppresses a specific finding ---
{
  const cpp = { methods: [], properties: [], signals: [] }
  const ts = { methods: [tsMethod('legacyGhost', 0, 0, 'Promise<void>')], properties: [], signals: [], mockKeys: [] }
  const exceptions = { missingBackend: [{ name: 'legacyGhost', reason: 'test exception' }] }
  const findings = compareBackendContract(cpp, ts, exceptions)
  check(!findings.missingBackend.some((f) => f.name === 'legacyGhost'), 'exceptions: a listed exception suppresses its specific finding')
}

// --- warnings: 64-bit precision flagged without failing the check ---
{
  const cpp = { methods: [cppMethod('bigNumber', 0, 'qint64')], properties: [], signals: [] }
  const ts = { methods: [tsMethod('bigNumber', 0, 0, 'Promise<number>')], properties: [], signals: [], mockKeys: ['bigNumber'] }
  const findings = compareBackendContract(cpp, ts)
  check(findings.warnings.some((f) => f.name === 'bigNumber'), 'warnings: qint64-as-number flagged as a warning')
  check(!findings.incompatible.some((f) => f.name === 'bigNumber'), 'warnings: qint64-as-number does not fail as incompatible')
}

if (failures > 0) {
  console.error(`\n${failures} failure(s).`)
  process.exit(1)
}
console.log('\nAll backend contract compare assertions passed.')
