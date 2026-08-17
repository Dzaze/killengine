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
