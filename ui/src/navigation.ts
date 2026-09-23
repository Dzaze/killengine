import type { AppView } from '@/stores/app'

export interface NavDestination {
  id: AppView
  labelKey: string
  subtitleKey?: string
}

export interface NavGroup {
  id: string
  labelKey: string
  defaultOpen: boolean
  destinations: NavDestination[]
}

// UX-PRODUIT-7 (docs/PHASE_TRACKER.md, 23/09/2026) : source unique pour le
// menu et l'état actif -- IDs stables inchangés (sauf ajout `project` par
// UX-PRODUIT-8), seul le libellé "Modules" change dans le rendu, pas son ID
// ni les API getProcessModules/getModuleCatalog qu'il ne touche pas.
// Fichier indépendant (pas dans app.ts) pour éviter tout import circulaire
// entre App.vue et le store.
export const navGroups: NavGroup[] = [
  {
    id: 'parcours',
    labelKey: 'nav.groups.parcours',
    defaultOpen: true,
    destinations: [
      { id: 'process', labelKey: 'nav.process' },
      { id: 'assistant', labelKey: 'nav.assistant' },
      { id: 'investigation', labelKey: 'nav.investigation' },
      { id: 'expert', labelKey: 'nav.expert' },
      { id: 'profiles', labelKey: 'nav.profiles' },
      { id: 'trainer', labelKey: 'nav.trainer' },
    ],
  },
  {
    id: 'session',
    labelKey: 'nav.groups.session',
    defaultOpen: true,
    destinations: [
      { id: 'project', labelKey: 'nav.project' },
    ],
  },
  {
    id: 'inspection',
    labelKey: 'nav.groups.inspection',
    defaultOpen: false,
    destinations: [
      { id: 'memory', labelKey: 'nav.memory' },
      { id: 'memory-timeline', labelKey: 'nav.memoryTimeline', subtitleKey: 'nav.memoryTimelineSubtitle' },
      { id: 'memory-heatmap', labelKey: 'nav.memoryHeatmap', subtitleKey: 'nav.memoryHeatmapSubtitle' },
      { id: 'pattern-learning', labelKey: 'nav.patternLearning', subtitleKey: 'nav.patternLearningSubtitle' },
      { id: 'clr', labelKey: 'nav.clr' },
      { id: 'webview2', labelKey: 'nav.webview2' },
      { id: 'network', labelKey: 'nav.network' },
    ],
  },
  {
    id: 'outils-avances',
    labelKey: 'nav.groups.advancedTools',
    defaultOpen: false,
    destinations: [
      { id: 'scripting', labelKey: 'nav.scripting' },
      { id: 'speedhack', labelKey: 'nav.speedhack' },
    ],
  },
  {
    id: 'configuration-aide',
    labelKey: 'nav.groups.configHelp',
    defaultOpen: false,
    destinations: [
      { id: 'modules', labelKey: 'nav.modules' },
      { id: 'settings', labelKey: 'nav.settings' },
      { id: 'lexicon', labelKey: 'nav.lexicon' },
    ],
  },
]

export function groupIdForView(view: AppView): string | null {
  for (const group of navGroups) {
    if (group.destinations.some((d) => d.id === view)) return group.id
  }
  return null
}
