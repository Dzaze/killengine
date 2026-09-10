import { createApp } from 'vue'
import { createPinia } from 'pinia'
import App from './App.vue'
import { i18n } from './i18n'
import './assets/main.css'

const app = createApp(App)
const { t } = i18n.global

// Sécurité anti "vue vide silencieuse" : toute erreur de rendu (ex: TDZ,
// computed cassé) doit être visible immédiatement dans l'UI et la console.
function showFatalErrorBanner(err: unknown, info: string) {
  const existing = document.getElementById('ke-fatal-banner')
  if (existing) existing.remove()

  const banner = document.createElement('div')
  banner.id = 'ke-fatal-banner'
  banner.setAttribute('role', 'alert')
  banner.style.cssText = [
    'position:fixed',
    'top:0',
    'left:0',
    'right:0',
    'z-index:99999',
    'background:#f7768e',
    'color:#10121a',
    'font:12px/1.5 "Segoe UI",sans-serif',
    'padding:10px 16px',
    'border-bottom:2px solid #e0af68',
    'white-space:pre-wrap',
    'word-break:break-word',
  ].join(';')
  banner.textContent = t('main.fatalErrorBanner', { info, message: String(err instanceof Error ? err.message : err) })

  const close = document.createElement('button')
  close.textContent = t('main.fatalErrorBannerClose')
  close.style.cssText = 'margin-left:12px;border:1px solid #10121a;background:transparent;color:#10121a;cursor:pointer;border-radius:4px;padding:2px 8px;font-weight:600'
  close.addEventListener('click', () => banner.remove())
  banner.appendChild(close)
  document.body.appendChild(banner)
}

app.config.errorHandler = (err, _instance, info) => {
  console.error('[KillEngine] Vue runtime error:', err, `(${info})`)
  showFatalErrorBanner(err, info)
}

if (typeof window !== 'undefined') {
  window.addEventListener('unhandledrejection', (event) => {
    console.error('[KillEngine] Unhandled promise rejection:', event.reason)
  })
}

app.use(createPinia())
app.use(i18n)
app.mount('#app')