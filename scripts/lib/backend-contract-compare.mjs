// UX-PRODUIT-14A (docs/PHASE_TRACKER.md) -- comparaison pure entre
// l'inventaire réel C++ (ApplicationController::describeBackendContract(),
// voir apps/desktop/application_controller.cpp) et l'extraction TS
// (scripts/lib/backend-contract-ts.mjs). Zéro dépendance runtime : deux
// structures de données en entrée, un rapport de diff en sortie.
//
// Catégories produites (fiche point 3) : missingBackend / missingTs /
// missingMock / incompatible, plus warnings (précision 64 bits) séparés des
// catégories bloquantes.
//
// Politique volontairement explicite sur missingMock (fiche point 3 : "pas un
// motif générique qui ignorerait toutes les méthodes optionnelles") : CE
// MODULE calcule et retourne la liste COMPLÈTE des membres TS sans mock, il
// ne l'ignore jamais. C'est l'appelant (scripts/test-backend-contract.mjs)
// qui décide de ne pas faire échouer le run sur cette seule catégorie par
// défaut (volume attendu élevé -- des dizaines de méthodes optionnelles
// légitimement non mockées) tout en la gardant intégralement visible dans le
// rapport, distinguée de missingBackend/missingTs/incompatible qui, elles,
// font échouer le run. Voir scripts/contracts/backend-contract-exceptions.json
// pour les exceptions motivées de ces catégories bloquantes.

const QT_TYPE_BUCKETS = {
  string: new Set(['QString', 'QByteArray']),
  boolean: new Set(['bool']),
  number: new Set(['int', 'uint', 'float', 'double', 'short', 'ushort', 'long', 'ulong']),
  number64: new Set(['qint64', 'quint64', 'qlonglong', 'qulonglong']),
  void: new Set(['void']),
  object: new Set(['QVariantMap']),
  array: new Set(['QVariantList', 'QStringList']),
}

/** @param {string} qtType */
export function qtTypeBucket(qtType) {
  const t = qtType.trim()
  for (const [bucket, names] of Object.entries(QT_TYPE_BUCKETS)) {
    if (names.has(t)) return bucket
  }
  return 'unverified'
}

/** @param {string} tsType */
export function unwrapPromise(tsType) {
  const m = /^Promise<([\s\S]*)>$/.exec(tsType.trim())
  return m ? m[1].trim() : tsType.trim()
}

/**
 * "Structure seule contrôlée" pour object/array (fiche point 2) : on vérifie
 * la FORME (objet vs tableau vs primitif), jamais les champs internes d'un
 * QVariantMap/QVariantList, impossibles à inférer côté C++ à ce niveau.
 * @param {string} tsTypeText
 * @param {string} bucket
 */
export function tsTypeMatchesBucket(tsTypeText, bucket) {
  const t = tsTypeText.trim()
  switch (bucket) {
    case 'string': return t === 'string'
    case 'boolean': return t === 'boolean'
    case 'number': return t === 'number'
    // number64 : la fiche demande de SIGNALER (pas bloquer) une précision JS
    // non garantie -- 'number' est accepté ici, le signalement se fait via
    // le champ warnings du rapport, pas via une incompatibilité.
    case 'number64': return t === 'number'
    case 'void': return t === 'void'
    // 'object' accepte Record<...>, un type nommé (PascalCase, convention du
    // dépôt), OU un littéral d'objet inline `{ success: boolean; ... }` --
    // très utilisé dans backend.ts au lieu de Record<string, unknown> pour
    // documenter la forme attendue. "Structure seule contrôlée" (fiche
    // point 2) : on vérifie juste que ça RESSEMBLE à un objet, jamais les
    // champs eux-mêmes.
    case 'object': return t.startsWith('Record<') || t.startsWith('{') || (/^[A-Z]/.test(t) && !t.endsWith('[]') && !t.startsWith('Array<'))
    case 'array': return t.endsWith('[]') || t.startsWith('Array<')
    default: return true // 'unverified' : jamais signalé comme incompatible
  }
}

function groupByName(entries) {
  const map = new Map()
  for (const entry of entries) {
    const list = map.get(entry.name) ?? []
    list.push(entry)
    map.set(entry.name, list)
  }
  return map
}

function isExcepted(exceptions, category, name) {
  return (exceptions[category] ?? []).some((e) => e.name === name)
}

/**
 * @param {{methods: Array, properties: Array, signals: Array}} cppContract
 * @param {{methods: Array, properties: Array, signals: Array, mockKeys: string[]}} tsContract
 * @param {Record<string, Array<{name: string, reason: string}>>} exceptions
 */
export function compareBackendContract(cppContract, tsContract, exceptions = {}) {
  const findings = { missingBackend: [], missingTs: [], missingMock: [], incompatible: [], warnings: [] }

  const cppMethodsByName = groupByName(cppContract.methods)
  const cppSignalsByName = groupByName(cppContract.signals)
  const cppPropertiesByName = new Map(cppContract.properties.map((p) => [p.name, p]))
  const tsSignalNames = new Set(tsContract.signals.map((s) => s.name))
  const tsPropertyNames = new Set(tsContract.properties.map((p) => p.name))
  const tsMockKeys = new Set(tsContract.mockKeys)

  // --- Méthodes : TS -> C++ ---
  for (const tsMethod of tsContract.methods) {
    const { name } = tsMethod
    const cppVariants = cppMethodsByName.get(name)
    if (!cppVariants) {
      if (!isExcepted(exceptions, 'missingBackend', name)) {
        findings.missingBackend.push({ name, kind: 'method', reason: `TS déclare ${name}() mais aucune méthode C++ correspondante n'existe.` })
      }
      continue
    }

    const matchingVariant = cppVariants.find((v) => v.arity >= tsMethod.minArity && v.arity <= tsMethod.maxArity)
    if (!matchingVariant) {
      if (!isExcepted(exceptions, 'incompatible', name)) {
        findings.incompatible.push({
          name,
          kind: 'method',
          reason: `Arité TS [${tsMethod.minArity}..${tsMethod.maxArity}] ne correspond à aucune arité C++ (${cppVariants.map((v) => v.arity).join(', ')}).`,
        })
      }
    } else {
      const cppBucket = qtTypeBucket(matchingVariant.returnType)
      const tsReturn = unwrapPromise(tsMethod.returnType)
      if (cppBucket !== 'unverified' && !tsTypeMatchesBucket(tsReturn, cppBucket)) {
        if (!isExcepted(exceptions, 'incompatible', name)) {
          findings.incompatible.push({
            name,
            kind: 'method',
            reason: `Retour C++ ${matchingVariant.returnType} (catégorie "${cppBucket}") incompatible avec le type TS ${tsMethod.returnType}.`,
          })
        }
      }
      if (cppBucket === 'number64') {
        findings.warnings.push({ name, kind: 'method', reason: `Retour C++ ${matchingVariant.returnType} : précision JS non garantie au-delà de 2^53 ; TS le type en 'number'.` })
      }
    }

    if (!tsMockKeys.has(name)) {
      findings.missingMock.push({ name, kind: 'method', optional: tsMethod.optional })
    }
  }

  // --- Méthodes : C++ -> TS ---
  const tsMethodNames = new Set(tsContract.methods.map((m) => m.name))
  for (const name of cppMethodsByName.keys()) {
    if (!tsMethodNames.has(name) && !isExcepted(exceptions, 'missingTs', name)) {
      findings.missingTs.push({ name, kind: 'method', reason: `Méthode C++ ${name}() absente de l'interface TS BackendController.` })
    }
  }

  // --- Signaux : comparaison par NOM seulement (fiche : arité/forme du
  // payload QWebChannelSignal<T> non uniformément encodable pour un signal
  // Qt multi-paramètres -- pas de convention confirmée dans ce dépôt pour ce
  // cas, donc pas de vérification d'arité ici, limite assumée). ---
  for (const name of tsSignalNames) {
    if (!cppSignalsByName.has(name) && !isExcepted(exceptions, 'missingBackend', name)) {
      findings.missingBackend.push({ name, kind: 'signal', reason: `TS déclare le signal ${name} mais aucun signal C++ correspondant.` })
    }
  }
  for (const name of cppSignalsByName.keys()) {
    if (!tsSignalNames.has(name) && !isExcepted(exceptions, 'missingTs', name)) {
      findings.missingTs.push({ name, kind: 'signal', reason: `Signal C++ ${name} absent de l'interface TS.` })
    }
  }

  // --- Propriétés : comparaison par nom + lecture/écriture ---
  for (const name of tsPropertyNames) {
    if (!cppPropertiesByName.has(name) && !isExcepted(exceptions, 'missingBackend', name)) {
      findings.missingBackend.push({ name, kind: 'property', reason: `TS déclare la propriété ${name} mais aucune propriété C++ correspondante.` })
    }
  }
  for (const name of cppPropertiesByName.keys()) {
    if (!tsPropertyNames.has(name) && !isExcepted(exceptions, 'missingTs', name)) {
      findings.missingTs.push({ name, kind: 'property', reason: `Propriété C++ ${name} absente de l'interface TS.` })
    }
  }

  return findings
}
