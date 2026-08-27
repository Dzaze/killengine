// Types de valeur numériques supportés par le moteur de scan — partagé entre
// ExpertView.vue et les panneaux extraits sous ui/src/components/expert/.
export const valueTypeOptions = [
  'Int8', 'UInt8', 'Int16', 'UInt16', 'Int32', 'UInt32', 'Int64', 'UInt64', 'Float32', 'Float64',
]

// Taille en octets pour un breakpoint matériel (findWhatWrites/analyzeFieldStability) —
// partagé entre ExpertView.vue et CandidatePanel.vue.
export function findWhatWritesSizeForType(type: string): number {
  if (type.endsWith('8')) return 1
  if (type.endsWith('16')) return 2
  if (type.endsWith('64') || type === 'Float64') return 8
  return 4
}
