/**
 * Niveau de risque d'un panneau ou d'une etape du Mode Expert.
 *
 * - `read`  : lit la memoire du processus cible, ne la modifie jamais.
 * - `write` : modifie les donnees du processus cible (reversible via rollback).
 * - `code`  : attache un debugger ou reecrit les instructions du processus cible.
 */
export type RiskLevel = 'read' | 'write' | 'code'
