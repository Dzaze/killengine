// Formatage numérique/octets partagé entre ExpertView.vue et les panneaux
// extraits sous ui/src/components/expert/ — anciennement dupliqué localement
// dans ExpertView.vue.

export function formatNumber(value: number | undefined): string {
  return new Intl.NumberFormat('fr-FR').format(value ?? 0)
}

export function formatRate(value: number | undefined): string {
  return `${formatNumber(Math.round(value ?? 0))}/s`
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
