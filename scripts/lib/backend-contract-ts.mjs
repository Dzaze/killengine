// UX-PRODUIT-14A (docs/PHASE_TRACKER.md) -- extraction structurelle pure de
// ui/src/services/backend.ts via l'API compilateur TypeScript (pas de regex
// fragile : l'interface BackendController est indentée de façon incohérente
// dans le fichier source -- ligne 1421+ à 2 espaces au lieu de 4 -- un
// heuristique par colonne se tromperait, l'AST non). Première utilisation de
// `typescript` en tant que bibliothèque dans ce dépôt (déjà une devDependency
// vendorée par vue-tsc, voir ui/package.json).
//
// Zéro dépendance runtime Vue/Pinia : prend un chemin de fichier, retourne une
// structure pure. Testable en isolation (voir scripts/test-backend-contract.ps1).

import fs from 'node:fs'
// `typescript` est une devDependency de ui/ (vendorée pour vue-tsc), pas du
// repo racine -- import explicite plutôt que de dépendre de la résolution de
// module Node (qui ne remonterait jamais jusqu'à ui/node_modules depuis
// scripts/lib/). Même esprit que le chemin esbuild.cmd explicite déjà utilisé
// par scripts/test-workspace-import-validation.ps1.
import ts from '../../ui/node_modules/typescript/lib/typescript.js'

/**
 * @typedef {{ name: string, optional: boolean, minArity: number, maxArity: number, paramTypes: string[], returnType: string }} TsMethodMember
 * @typedef {{ name: string, optional: boolean, typeText: string }} TsPropertyMember
 * @typedef {{ name: string, optional: boolean, payloadType: string }} TsSignalMember
 */

/**
 * @param {string} backendTsPath
 * @returns {{
 *   methods: TsMethodMember[],
 *   properties: TsPropertyMember[],
 *   signals: TsSignalMember[],
 *   mockKeys: string[],
 * }}
 */
export function extractBackendContract(backendTsPath) {
  const sourceText = fs.readFileSync(backendTsPath, 'utf8')
  const sourceFile = ts.createSourceFile(backendTsPath, sourceText, ts.ScriptTarget.Latest, true, ts.ScriptKind.TS)

  let interfaceNode = null
  let mockMethodNode = null

  const visit = (node) => {
    if (ts.isInterfaceDeclaration(node) && node.name.text === 'BackendController') {
      interfaceNode = node
    }
    if (ts.isMethodDeclaration(node) && node.name && node.name.getText(sourceFile) === 'createMockBackend') {
      mockMethodNode = node
    }
    ts.forEachChild(node, visit)
  }
  visit(sourceFile)

  if (!interfaceNode) {
    throw new Error(`BackendController interface not found in ${backendTsPath}`)
  }
  if (!mockMethodNode) {
    throw new Error(`createMockBackend method not found in ${backendTsPath}`)
  }

  const methods = []
  const properties = []
  const signals = []

  for (const member of interfaceNode.members) {
    if (!member.name) continue
    const name = member.name.getText(sourceFile)
    const optional = Boolean(member.questionToken)

    if (ts.isMethodSignature(member)) {
      const params = member.parameters.map((p) => ({
        typeText: p.type ? p.type.getText(sourceFile) : 'any',
        optional: Boolean(p.questionToken) || Boolean(p.initializer),
      }))
      const requiredCount = params.filter((p) => !p.optional).length
      methods.push({
        name,
        optional,
        minArity: requiredCount,
        maxArity: params.length,
        paramTypes: params.map((p) => p.typeText),
        returnType: member.type ? member.type.getText(sourceFile) : 'void',
      })
    } else if (ts.isPropertySignature(member)) {
      const typeText = (member.type ? member.type.getText(sourceFile) : 'any').trim()
      const signalMatch = /^QWebChannelSignal<([\s\S]*)>$/.exec(typeText)
      if (signalMatch) {
        signals.push({ name, optional, payloadType: signalMatch[1].trim() })
      } else {
        properties.push({ name, optional, typeText })
      }
    }
    // Autres formes de membres (index signatures, call signatures...) : aucune
    // rencontrée dans BackendController à ce jour, ignorées silencieusement
    // plutôt que de faire échouer toute l'extraction sur une forme inattendue.
  }

  const mockKeys = new Set()
  const findReturnObjectLiteral = (node) => {
    if (ts.isReturnStatement(node) && node.expression && ts.isObjectLiteralExpression(node.expression)) {
      for (const prop of node.expression.properties) {
        if (prop.name) {
          mockKeys.add(prop.name.getText(sourceFile))
        }
      }
      return
    }
    ts.forEachChild(node, findReturnObjectLiteral)
  }
  if (mockMethodNode.body) {
    findReturnObjectLiteral(mockMethodNode.body)
  }

  return { methods, properties, signals, mockKeys: [...mockKeys] }
}
