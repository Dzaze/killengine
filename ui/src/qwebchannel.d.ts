/**
 * KillEngine - Déclarations de types pour l'intégration QWebChannel Qt
 */

// QWebChannel global injecté par Qt WebEngine
declare global {
  interface QWebChannel {
    registerObject(name: string, object: unknown): void
    objects: Record<string, unknown>
  }

  interface QWebChannelSignal<T = unknown> {
    connect(callback: (payload: T) => void): void
    disconnect?(callback: (payload: T) => void): void
  }

  interface Window {
    QWebChannel: {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      new (transport: any, callback: (channel: QWebChannel) => void): QWebChannel
    }
    qt?: {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      webChannelTransport: any
    }
    // PHASE 119 -- pont pipe d'automatisation -> Vue/Pinia (voir
    // docs/POWER_UP_ROADMAP.md section N). Surface JS bornée, câblée
    // depuis ui/src/stores/app.ts, appelée par
    // ApplicationController::callVueStoreAction (C++) via runJavaScript().
    __killengineAutomationBridge?: {
      dispatch(action: string, args: unknown[]): unknown
    }
  }
}

export {}
