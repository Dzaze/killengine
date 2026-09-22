// Formatage numérique/octets partagé entre ExpertView.vue et les panneaux
// extraits sous ui/src/components/expert/ — anciennement dupliqué localement
// dans ExpertView.vue.

export function formatNumber(value: number | undefined): string {
  return new Intl.NumberFormat('fr-FR').format(value ?? 0)
}

export function formatRate(value: number | undefined): string {
  return `${formatNumber(Math.round(value ?? 0))}/s`
}

// UX-PIPE-6 (docs/PHASE_TRACKER.md, 18/09/2026) : une estimation longue
// affichée en secondes brutes ("1159s") est illisible -- convertit en
// minutes/secondes au-delà d'une minute, garde une valeur exacte en secondes
// en dessous (pas d'arrondi trompeur sur une courte estimation).
export function formatDuration(totalSeconds: number | undefined): string {
  const seconds = Math.max(0, Math.round(totalSeconds ?? 0))
  if (seconds < 60) return `${seconds} s`
  const minutes = Math.floor(seconds / 60)
  const remainingSeconds = seconds % 60
  return remainingSeconds === 0 ? `${minutes} min` : `${minutes} min ${remainingSeconds} s`
}

export function formatBytes(value: number | undefined): string {
  const bytes = value ?? 0
  if (bytes >= 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} Go`
  if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} Mo`
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} Ko`
  return `${formatNumber(bytes)} o`
}

// Nom de profil/cible Trainer nettoyé (extension retirée, caractères non
// alphanumériques remplacés) — partagé entre ExpertView.vue et les panneaux
// extraits (AOB, pointer chains) qui suggèrent tous deux un nom par défaut.
export function cleanTrainerName(value: string, fallback: string): string {
  const cleaned = value
    .replace(/\.[^.]+$/, '')
    .replace(/[^a-z0-9_-]+/gi, '_')
    .replace(/^_+|_+$/g, '')
  return cleaned || fallback
}
