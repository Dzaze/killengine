export type ExpertStepId = 'find' | 'inspect' | 'act' | 'persist'

export interface ExpertToolEntry {
  /** ID stable, sert aussi de clé de persistance pour les favoris. */
  id: string
  /** Clé i18n existante réutilisée pour le titre (pas de duplication de texte). */
  labelKey: string
  step: ExpertStepId
  /** ID DOM cible pour le scroll -- porté par le composant du panneau via
   * l'attribut `id` (fallthrough Vue sur son élément racine), sauf
   * `find-what-writes` qui réutilise l'ancre déjà existante et déjà
   * référencée par l'IA (voir docs/PHASE_TRACKER.md, UX-PRODUIT-9). */
  anchor: string
  /** Termes de recherche supplémentaires (bilingues), en plus du titre i18n
   * résolu dans les deux langues au moment de la recherche. */
  keywords: string[]
}

// UX-PRODUIT-9 (docs/PHASE_TRACKER.md, 23/09/2026) : catalogue de navigation
// pour le sommaire/recherche/favoris d'ExpertView -- sert uniquement à
// naviguer (scroll/focus), ne remplace pas le registry des outils IA
// (ai/tool_registry.cpp) et n'exécute aucune méthode moteur.
export const expertToolCatalog: ExpertToolEntry[] = [
  { id: 'exact-scan', labelKey: 'scan.exact', step: 'find', anchor: 'expert-anchor-exact-scan', keywords: ['premier scan', 'first scan', 'valeur connue', 'known value'] },
  { id: 'next-scan', labelKey: 'scan.nextScan', step: 'find', anchor: 'expert-anchor-next-scan', keywords: ['affiner', 'refine', 'changed', 'unchanged', 'increased', 'decreased'] },
  { id: 'unknown-scan', labelKey: 'unknown.title', step: 'find', anchor: 'expert-anchor-unknown-scan', keywords: ['snapshot', 'valeur inconnue', 'unknown value'] },
  { id: 'trace-ui-string', labelKey: 'expert.traceUiStringTitle', step: 'find', anchor: 'expert-anchor-trace-ui-string', keywords: ['ui string', 'texte affiché', 'on-screen text', 'live investigation'] },
  { id: 'find-what-writes', labelKey: 'expert.writtenBy', step: 'find', anchor: 'expert-anchor-find-what-writes', keywords: ['find what writes', 'écrit par', 'debugger', 'breakpoint'] },
  { id: 'visual-observation', labelKey: 'expert.visualObservationTitle', step: 'find', anchor: 'expert-anchor-visual-observation', keywords: ['montre-moi ce qui change', 'show me what changes', 'observation', 'corrélation', 'correlation'] },
  { id: 'group-scan', labelKey: 'groupScanPanel.title', step: 'find', anchor: 'expert-anchor-group-scan', keywords: ['group scan', 'scan groupé', 'multi struct'] },
  { id: 'find-what-accesses', labelKey: 'findWhatAccesses.title', step: 'find', anchor: 'expert-anchor-find-what-accesses', keywords: ['accès mémoire', 'memory access', 'lecture', 'read access'] },
  { id: 'auto-dissect', labelKey: 'autoDissect.title', step: 'find', anchor: 'expert-anchor-auto-dissect', keywords: ['autodissection', 'structure', 'champs', 'fields'] },
  { id: 'region', labelKey: 'regionPanel.title', step: 'inspect', anchor: 'expert-anchor-region', keywords: ['région', 'region', 'protection mémoire', 'memory protection'] },
  { id: 'save-files', labelKey: 'saveFilesPanel.title', step: 'inspect', anchor: 'expert-anchor-save-files', keywords: ['sauvegarde', 'save', 'export', 'fichier'] },
  { id: 'candidates', labelKey: 'candidatePanel.title', step: 'inspect', anchor: 'expert-anchor-candidates', keywords: ['candidats', 'candidates', 'résultats', 'results'] },
  { id: 'watch', labelKey: 'watchLivePanel.title', step: 'inspect', anchor: 'expert-anchor-watch', keywords: ['watch', 'surveiller', 'live', 'temps réel'] },
  { id: 'pointer-chain-scan', labelKey: 'pointerChainScan.title', step: 'inspect', anchor: 'expert-anchor-pointer-chain-scan', keywords: ['pointeur', 'pointer', 'chaîne', 'chain', 'offset'] },
  { id: 'pointer-chain-watch', labelKey: 'pointerChainWatchPanel.title', step: 'inspect', anchor: 'expert-anchor-pointer-chain-watch', keywords: ['pointeur', 'pointer', 'watch pointeur'] },
  { id: 'write', labelKey: 'write.title', step: 'act', anchor: 'expert-anchor-write', keywords: ['écrire', 'write', 'freeze', 'geler'] },
  { id: 'aob-signature', labelKey: 'aobSignature.title', step: 'persist', anchor: 'expert-anchor-aob', keywords: ['aob', 'signature', 'pattern', 'motif'] },
  { id: 'injection', labelKey: 'injectionPanel.title', step: 'persist', anchor: 'expert-anchor-injection', keywords: ['injection', 'dll', 'hook', 'asm'] },
  { id: 'session', labelKey: 'sessionPanel.title', step: 'persist', anchor: 'expert-anchor-session', keywords: ['session', 'profil', 'profile'] },
  { id: 'action-log', labelKey: 'actionLogPanel.title', step: 'persist', anchor: 'expert-anchor-action-log', keywords: ['journal', 'log', 'historique', 'history'] },
]
