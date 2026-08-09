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
  }
}

export {}
