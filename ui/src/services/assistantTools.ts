/**
 * Inventaire statique des outils Assistant (chat `startSmartSearch`).
 *
 * Aligné sur `ai/tool_registry.cpp` (source de vérité risque/confirmation) et
 * `docs/KILLENGINE_ASSISTANT_TOOLS_MAP.md` — 57 outils au 07/09/2026 (chantier
 * backend IA externe, PHASE EXTERNAL-AI-BACKEND T1-T5, voir
 * `docs/EXTERNAL_AI_BACKEND_ROADMAP.md`). Liste volontairement statique :
 * aucune API backend n'expose encore le registre d'outils à l'UI, et un pont
 * improvisé serait fragile. À resynchroniser manuellement si le registry
 * change (voir le compteur ASSISTANT_TOOLS_SNAPSHOT et le test de
 * non-régression côté C++ `AIToolRegistryTest`).
 *
 * Depuis le 07/09/2026, KillEngine a DEUX backends de chat possibles (voir
 * Réglages > "Backend IA externe (Claude)") : le modèle local embarqué
 * (défaut) et un backend Claude optionnel (`apps/desktop/claude_chat_manager.cpp`).
 * Le backend Claude couvrait déjà les 57 outils du registre (T4) ; le
 * dispatch du modèle local (`SmartSearchManager::startSmartSearch`) a été
 * étendu le même jour pour couvrir lui aussi les 20 outils réseau/proxy
 * HTTP/DNS/stealth/WebView2 qui tombaient auparavant sur "outil non
 * supporté" malgré leur présence dans le schéma envoyé au modèle (PHASE
 * 271-272 : aucun outil caché du schéma — un outil annoncé doit réellement
 * s'exécuter). Les deux backends couvrent maintenant les mêmes 57 outils ;
 * la seule différence restante est notée sur `connectWebView2Inspector`
 * ci-dessous (le modèle local redirige vers l'onglet WebView2 plutôt que de
 * se connecter directement).
 *
 * Localisation (10/09/2026) : `category` est un slug stable (clé de
 * regroupement + clé i18n `assistantToolsPanel.categories.<slug>`), et
 * `summary`/`note` ne sont plus stockés ici en texte brut -- AssistantToolsPanel.vue
 * les résout via `t('assistantToolsPanel.tools.' + tool.name + '.summary'/'.note')`
 * pour rester réactif au changement de langue.
 */

/** Comment l'outil s'exécute réellement depuis le chat Assistant. */
export type AssistantToolExecution = 'direct' | 'confirm' | 'redirect'

/** Catégorie de risque du registre (`ai/tool_registry.cpp`, champ `risk`). */
export type AssistantToolRisk = 'safe' | 'write' | 'debug' | 'patch' | 'injection' | 'script'

export interface AssistantTool {
  name: string
  /** Slug stable de catégorie, traduit via assistantToolsPanel.categories.<category>. */
  category: string
  risk: AssistantToolRisk
  execution: AssistantToolExecution
  /** true si assistantToolsPanel.tools.<name>.note existe dans les locales. */
  hasNote?: boolean
}

/** Nombre d'outils au moment de la dernière synchronisation avec le registre. */
export const ASSISTANT_TOOLS_SNAPSHOT = 58

/**
 * Statuts d'exécution (PHASE 146/147/148, étendu T4 backend Claude) :
 * - `direct`   : exécuté directement par le chat, jamais d'écriture (lecture/analyse seule).
 * - `confirm`  : l'Assistant prépare l'action, un clic de confirmation UI (RiskGate) reste obligatoire.
 * - `redirect` : jamais exécuté depuis le chat, même après confirmation — toujours redirigé vers l'UI.
 */
export const assistantTools: AssistantTool[] = [
  // --- Scan mémoire ---
  { name: 'auto_resolve', category: 'scanMemory', risk: 'safe', execution: 'direct' },
  { name: 'get_auto_report', category: 'scanMemory', risk: 'safe', execution: 'direct' },
  { name: 'exact_scan', category: 'scanMemory', risk: 'safe', execution: 'direct' },
  { name: 'exact_scan_module', category: 'scanMemory', risk: 'safe', execution: 'direct' },
  { name: 'exact_scan_multi_type', category: 'scanMemory', risk: 'safe', execution: 'direct' },
  { name: 'next_scan', category: 'scanMemory', risk: 'safe', execution: 'direct' },
  { name: 'get_candidates', category: 'scanMemory', risk: 'safe', execution: 'direct', hasNote: true },
  { name: 'encrypted_scan', category: 'scanMemory', risk: 'safe', execution: 'direct' },
  { name: 'trace_ui_string', category: 'scanMemory', risk: 'safe', execution: 'direct' },
  { name: 'analyze_ui_sources', category: 'scanMemory', risk: 'safe', execution: 'direct' },
  { name: 'read_window_text', category: 'scanMemory', risk: 'safe', execution: 'direct' },
  { name: 'list_process_modules', category: 'scanMemory', risk: 'safe', execution: 'direct' },
  { name: 'start_changed_pages_diff', category: 'scanMemory', risk: 'safe', execution: 'direct' },
  { name: 'finish_changed_pages_diff', category: 'scanMemory', risk: 'safe', execution: 'direct' },
  { name: 'unknown_capture', category: 'scanMemory', risk: 'safe', execution: 'direct' },
  { name: 'unknown_compare', category: 'scanMemory', risk: 'safe', execution: 'direct' },

  // --- Écriture / Freeze ---
  { name: 'prepare_write_checkpoint', category: 'writeFreeze', risk: 'write', execution: 'confirm', hasNote: true },
  { name: 'write_value', category: 'writeFreeze', risk: 'write', execution: 'confirm' },
  { name: 'freeze_value', category: 'writeFreeze', risk: 'write', execution: 'confirm' },

  // --- Debug / Patch ---
  { name: 'find_what_writes', category: 'debugPatch', risk: 'debug', execution: 'redirect', hasNote: true },
  { name: 'analyze_field_stability', category: 'debugPatch', risk: 'debug', execution: 'direct' },
  { name: 'generate_aob', category: 'debugPatch', risk: 'patch', execution: 'direct' },
  { name: 'suggest_patch', category: 'debugPatch', risk: 'patch', execution: 'direct' },
  { name: 'disassemble_backward', category: 'debugPatch', risk: 'patch', execution: 'direct' },
  { name: 'test_candidate_fields', category: 'debugPatch', risk: 'write', execution: 'redirect', hasNote: true },

  // --- Injection / Kernel ---
  { name: 'kernel_write', category: 'injectionKernel', risk: 'injection', execution: 'confirm' },
  { name: 'speedhack_set', category: 'injectionKernel', risk: 'injection', execution: 'confirm' },

  // --- Réseau ---
  { name: 'block_process_network', category: 'network', risk: 'injection', execution: 'confirm' },
  { name: 'get_process_network_connections', category: 'network', risk: 'safe', execution: 'direct', hasNote: true },
  { name: 'get_process_network_modules', category: 'network', risk: 'safe', execution: 'direct' },
  { name: 'start_http_proxy', category: 'network', risk: 'injection', execution: 'confirm' },
  { name: 'stop_http_proxy', category: 'network', risk: 'injection', execution: 'confirm' },
  { name: 'get_http_proxy_requests', category: 'network', risk: 'safe', execution: 'direct' },
  { name: 'modify_http_request', category: 'network', risk: 'injection', execution: 'confirm' },
  { name: 'spoof_dns', category: 'network', risk: 'injection', execution: 'confirm' },
  { name: 'restore_dns', category: 'network', risk: 'injection', execution: 'confirm' },
  { name: 'set_lag_switch', category: 'network', risk: 'injection', execution: 'confirm' },

  // --- Stealth ---
  { name: 'apply_stealth_mode', category: 'stealth', risk: 'injection', execution: 'confirm' },
  { name: 'restore_stealth_mode', category: 'stealth', risk: 'injection', execution: 'confirm' },
  { name: 'get_stealth_status', category: 'stealth', risk: 'safe', execution: 'direct' },

  // --- Fichiers de sauvegarde / UWP ---
  { name: 'discover_save_files', category: 'saveFilesUwp', risk: 'safe', execution: 'direct' },
  { name: 'inspect_local_settings', category: 'saveFilesUwp', risk: 'safe', execution: 'direct' },
  { name: 'read_save_file_text', category: 'saveFilesUwp', risk: 'safe', execution: 'direct' },
  { name: 'patch_file_bytes', category: 'saveFilesUwp', risk: 'write', execution: 'redirect', hasNote: true },
  { name: 'watch_save_file', category: 'saveFilesUwp', risk: 'safe', execution: 'direct' },

  // --- Trainer ---
  { name: 'trainer_list_features', category: 'trainer', risk: 'safe', execution: 'direct' },
  { name: 'trainer_create_write', category: 'trainer', risk: 'safe', execution: 'direct' },
  { name: 'trainer_delete_feature', category: 'trainer', risk: 'safe', execution: 'direct' },
  { name: 'trainer_apply_request', category: 'trainer', risk: 'write', execution: 'confirm', hasNote: true },
  { name: 'trainer_restore_request', category: 'trainer', risk: 'write', execution: 'confirm', hasNote: true },

  // --- WebView2 / CDP ---
  { name: 'getWebView2InspectorStatus', category: 'webview2Cdp', risk: 'safe', execution: 'direct' },
  { name: 'listWebView2CdpTargets', category: 'webview2Cdp', risk: 'debug', execution: 'direct' },
  { name: 'connectWebView2Inspector', category: 'webview2Cdp', risk: 'debug', execution: 'confirm', hasNote: true },
  { name: 'disconnectWebView2Inspector', category: 'webview2Cdp', risk: 'safe', execution: 'direct' },
  { name: 'evaluateWebView2JavaScript', category: 'webview2Cdp', risk: 'script', execution: 'confirm', hasNote: true },
  { name: 'findWebView2DisplayedValues', category: 'webview2Cdp', risk: 'debug', execution: 'direct' },
  { name: 'findWebView2DisplayedText', category: 'webview2Cdp', risk: 'debug', execution: 'direct' },
  { name: 'probeWebView2GlobalScope', category: 'webview2Cdp', risk: 'safe', execution: 'direct' },
]
