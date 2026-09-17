// PORT-3b (docs/PORTABILITY_ROADMAP.md, 17/09/2026) : plusieurs stores
// (trainer.ts, workspaceItems.ts, workspaceSession.ts) enveloppaient
// window.localStorage.setItem() dans un try/catch qui avalait l'exception
// (quota dépassé, stockage désactivé) avec un commentaire "best-effort" --
// leurs appelants annonçaient ensuite une sauvegarde réussie sans jamais
// savoir que l'écriture avait échoué. Ce petit utilitaire partagé renvoie un
// résultat exploitable au lieu de rien, sans dupliquer tout le stockage
// frontend dans un nouveau système.
export interface PersistOutcome {
  success: boolean
  error?: string
}

export function persistJsonToLocalStorage(key: string, value: unknown): PersistOutcome {
  try {
    window.localStorage.setItem(key, JSON.stringify(value))
    return { success: true }
  } catch (e) {
    return { success: false, error: e instanceof Error ? e.message : String(e) }
  }
}
