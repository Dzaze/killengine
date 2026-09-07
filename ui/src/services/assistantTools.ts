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
 * Le backend Claude couvre les 57 outils du registre ; le dispatch du modèle
 * local (`SmartSearchManager::startSmartSearch`) n'a lui jamais été étendu
 * au-delà d'un sous-ensemble historique et renvoie "outil non supporté" sur
 * le reste (réseau, proxy HTTP, DNS, stealth, WebView2/CDP) — champ `note`
 * précisé pour chacun de ces outils ci-dessous.
 */

/** Comment l'outil s'exécute réellement depuis le chat Assistant. */
export type AssistantToolExecution = 'direct' | 'confirm' | 'redirect'

/** Catégorie de risque du registre (`ai/tool_registry.cpp`, champ `risk`). */
export type AssistantToolRisk = 'safe' | 'write' | 'debug' | 'patch' | 'injection' | 'script'

export interface AssistantTool {
  name: string
  category: string
  risk: AssistantToolRisk
  execution: AssistantToolExecution
  /** Description courte orientée utilisateur. */
  summary: string
  /** Précision utile (redirection, limitation connue...). */
  note?: string
}

/** Nombre d'outils au moment de la dernière synchronisation avec le registre. */
export const ASSISTANT_TOOLS_SNAPSHOT = 57

/** Note réutilisée pour tout outil que seul le backend Claude sait exécuter aujourd'hui. */
const CLAUDE_ONLY_NOTE =
  'Exécutable uniquement via le backend Claude (Réglages > Backend IA externe). Le modèle local renvoie "outil non supporté" sur cette action.'

/**
 * Statuts d'exécution (PHASE 146/147/148, étendu T4 backend Claude) :
 * - `direct`   : exécuté directement par le chat, jamais d'écriture (lecture/analyse seule).
 * - `confirm`  : l'Assistant prépare l'action, un clic de confirmation UI (RiskGate) reste obligatoire.
 * - `redirect` : jamais exécuté depuis le chat, même après confirmation — toujours redirigé vers l'UI.
 */
export const assistantTools: AssistantTool[] = [
  // --- Scan mémoire ---
  { name: 'auto_resolve', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Planifie et exécute une mini-boucle sûre bornée (scan, réduction, fallbacks).' },
  { name: 'get_auto_report', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Résume contexte, télémétrie et prochaine action conseillée.' },
  { name: 'exact_scan', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Scan exact sur le processus attaché.' },
  { name: 'exact_scan_module', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Scan exact borné à un module/DLL chargé précis.' },
  { name: 'exact_scan_multi_type', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Scan exact multi-type quand la représentation mémoire est inconnue.' },
  { name: 'next_scan', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Réduit les candidats existants après variation.' },
  { name: 'encrypted_scan', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Scan chiffré borné XOR/Add/Sub/NOT.' },
  { name: 'trace_ui_string', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Cherche la valeur affichée en ASCII/UTF-16 (strings UI).' },
  { name: 'analyze_ui_sources', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Analyse les sources numériques proches des strings UI.' },
  { name: 'read_window_text', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Observe les titres/textes de fenêtres du processus.' },
  { name: 'list_process_modules', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Liste les DLL/modules chargés par le processus attaché.' },
  { name: 'start_changed_pages_diff', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Snapshot borné des pages modifiables avant variation.' },
  { name: 'finish_changed_pages_diff', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Compare le snapshot et cherche les encodages modifiés.' },
  { name: 'unknown_capture', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Capture un snapshot unknown initial borné.' },
  { name: 'unknown_compare', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Compare le snapshot unknown après variation.' },

  // --- Écriture / Freeze ---
  { name: 'prepare_write_checkpoint', category: 'Écriture / Freeze', risk: 'write', execution: 'confirm', summary: 'Prépare des candidats pour écriture, sans écrire.', note: 'Dépend de l\'historique de scan interne au modèle local — non disponible depuis le backend Claude (utiliser write_value directement, adresse et valeur déjà connues).' },
  { name: 'write_value', category: 'Écriture / Freeze', risk: 'write', execution: 'confirm', summary: 'Écrit une valeur typée à une adresse.' },
  { name: 'freeze_value', category: 'Écriture / Freeze', risk: 'write', execution: 'confirm', summary: 'Active ou désactive un freeze.' },

  // --- Debug / Patch ---
  { name: 'find_what_writes', category: 'Debug / Patch', risk: 'debug', execution: 'redirect', summary: 'Capture l\'instruction qui écrit une adresse.', note: 'Attache un debugger et nécessite une variation live → bouton « Écrit par » dans la vue Expert. Jamais exécuté depuis le chat, quel que soit le backend.' },
  { name: 'analyze_field_stability', category: 'Debug / Patch', risk: 'debug', execution: 'direct', summary: 'Classe le rythme d\'écriture : champ affiché recalculé vs vraie source.' },
  { name: 'generate_aob', category: 'Debug / Patch', risk: 'patch', execution: 'direct', summary: 'Génère une signature AOB depuis une adresse d\'instruction.' },
  { name: 'suggest_patch', category: 'Debug / Patch', risk: 'patch', execution: 'direct', summary: 'Suggère des patchs de code sans les appliquer.' },
  { name: 'disassemble_backward', category: 'Debug / Patch', risk: 'patch', execution: 'direct', summary: 'Désassemble avant un RIP pour repérer les champs sources.' },
  { name: 'test_candidate_fields', category: 'Debug / Patch', risk: 'write', execution: 'redirect', summary: 'Teste quels champs candidats tiennent réellement.', note: 'Écrit réellement une valeur test (~1 min en tâche de fond) → vue Expert. Jamais exécuté depuis le chat, quel que soit le backend.' },

  // --- Injection / Kernel ---
  { name: 'kernel_write', category: 'Injection / Kernel', risk: 'injection', execution: 'confirm', summary: 'Écrit via le driver noyau (contourne les protections usermode).' },
  { name: 'speedhack_set', category: 'Injection / Kernel', risk: 'injection', execution: 'confirm', summary: 'Accélère, ralentit ou remet la vitesse du processus.' },

  // --- Réseau ---
  { name: 'block_process_network', category: 'Réseau', risk: 'injection', execution: 'confirm', summary: 'Coupe ou rétablit le réseau du processus (pare-feu).' },
  { name: 'get_process_network_connections', category: 'Réseau', risk: 'safe', execution: 'direct', summary: 'Liste les connexions TCP/UDP actives du processus, avec résolution DNS.', note: CLAUDE_ONLY_NOTE },
  { name: 'get_process_network_modules', category: 'Réseau', risk: 'safe', execution: 'direct', summary: 'Liste les modules DLL réseau chargés (wininet, winhttp, ws2_32...).', note: CLAUDE_ONLY_NOTE },
  { name: 'start_http_proxy', category: 'Réseau', risk: 'injection', execution: 'confirm', summary: 'Intercepte les requêtes HTTP/HTTPS du processus cible.', note: CLAUDE_ONLY_NOTE },
  { name: 'stop_http_proxy', category: 'Réseau', risk: 'injection', execution: 'confirm', summary: 'Arrête le proxy HTTP actif et retire les hooks.', note: CLAUDE_ONLY_NOTE },
  { name: 'get_http_proxy_requests', category: 'Réseau', risk: 'safe', execution: 'direct', summary: 'Liste les requêtes HTTP interceptées par le proxy actif.', note: CLAUDE_ONLY_NOTE },
  { name: 'modify_http_request', category: 'Réseau', risk: 'injection', execution: 'confirm', summary: 'Modifie le body d\'une requête HTTP interceptée avant envoi.', note: CLAUDE_ONLY_NOTE },
  { name: 'spoof_dns', category: 'Réseau', risk: 'injection', execution: 'confirm', summary: 'Redirige un domaine vers une IP locale via le fichier hosts.', note: CLAUDE_ONLY_NOTE },
  { name: 'restore_dns', category: 'Réseau', risk: 'injection', execution: 'confirm', summary: 'Retire l\'entrée DNS spoofée du fichier hosts.', note: CLAUDE_ONLY_NOTE },
  { name: 'set_lag_switch', category: 'Réseau', risk: 'injection', execution: 'confirm', summary: 'Retarde les fonctions recv/WSARecv du processus cible.', note: CLAUDE_ONLY_NOTE },

  // --- Stealth ---
  { name: 'apply_stealth_mode', category: 'Stealth', risk: 'injection', execution: 'confirm', summary: 'Active le mode discret (anti-debug, masquage process/DLL).', note: CLAUDE_ONLY_NOTE },
  { name: 'restore_stealth_mode', category: 'Stealth', risk: 'injection', execution: 'confirm', summary: 'Désactive le mode discret et restaure l\'état original.', note: CLAUDE_ONLY_NOTE },
  { name: 'get_stealth_status', category: 'Stealth', risk: 'safe', execution: 'direct', summary: 'Retourne l\'état courant du mode discret (actif, profil, modules).', note: CLAUDE_ONLY_NOTE },

  // --- Fichiers de sauvegarde / UWP ---
  { name: 'discover_save_files', category: 'Fichiers de sauvegarde / UWP', risk: 'safe', execution: 'direct', summary: 'Cherche les fichiers de sauvegarde probables du processus (UWP).' },
  { name: 'inspect_local_settings', category: 'Fichiers de sauvegarde / UWP', risk: 'safe', execution: 'direct', summary: 'Inspecte en lecture seule les LocalSettings UWP.' },
  { name: 'read_save_file_text', category: 'Fichiers de sauvegarde / UWP', risk: 'safe', execution: 'direct', summary: 'Lit le contenu texte borné d\'un fichier de sauvegarde.' },
  { name: 'patch_file_bytes', category: 'Fichiers de sauvegarde / UWP', risk: 'write', execution: 'redirect', summary: 'Édite des octets dans un fichier de sauvegarde.', note: 'Écriture disque réelle → jamais exécuté depuis le chat (PHASE 148), redirige vers le panneau Fichiers de sauvegarde de la vue Expert. Vrai quel que soit le backend.' },
  { name: 'watch_save_file', category: 'Fichiers de sauvegarde / UWP', risk: 'safe', execution: 'direct', summary: 'Surveille un fichier jusqu\'à changement ou timeout.' },

  // --- Trainer ---
  { name: 'trainer_list_features', category: 'Trainer', risk: 'safe', execution: 'direct', summary: 'Liste les features Trainer locales.' },
  { name: 'trainer_create_write', category: 'Trainer', risk: 'safe', execution: 'direct', summary: 'Crée une feature Trainer write sans l\'activer.' },
  { name: 'trainer_delete_feature', category: 'Trainer', risk: 'safe', execution: 'direct', summary: 'Supprime une feature Trainer par id.' },
  { name: 'trainer_apply_request', category: 'Trainer', risk: 'write', execution: 'confirm', summary: 'Prépare une demande d\'activation Trainer.', note: 'Confirmation à donner dans l\'onglet Trainer (modèle local) ou via le clic RiskGate déclenché par le backend Claude.' },
  { name: 'trainer_restore_request', category: 'Trainer', risk: 'write', execution: 'confirm', summary: 'Prépare une demande de restauration Trainer.', note: 'Confirmation à donner dans l\'onglet Trainer (modèle local) ou via le clic RiskGate déclenché par le backend Claude.' },

  // --- WebView2 / CDP ---
  { name: 'getWebView2InspectorStatus', category: 'WebView2 / CDP', risk: 'safe', execution: 'direct', summary: 'Retourne l\'état de l\'inspecteur WebView2/CDP (connecté, endpoint, target).', note: CLAUDE_ONLY_NOTE },
  { name: 'listWebView2CdpTargets', category: 'WebView2 / CDP', risk: 'debug', execution: 'direct', summary: 'Liste les targets CDP WebView2 disponibles pour un process browser donné.', note: CLAUDE_ONLY_NOTE },
  { name: 'connectWebView2Inspector', category: 'WebView2 / CDP', risk: 'debug', execution: 'confirm', summary: 'Connecte l\'inspecteur à une target CDP.', note: `S'attache à un process externe. ${CLAUDE_ONLY_NOTE}` },
  { name: 'disconnectWebView2Inspector', category: 'WebView2 / CDP', risk: 'safe', execution: 'direct', summary: 'Déconnecte l\'inspecteur WebView2/CDP courant.', note: CLAUDE_ONLY_NOTE },
  { name: 'evaluateWebView2JavaScript', category: 'WebView2 / CDP', risk: 'script', execution: 'confirm', summary: 'Évalue une expression JavaScript arbitraire dans la target connectée.', note: `Peut lire ou modifier l'état JS selon le code fourni. ${CLAUDE_ONLY_NOTE}` },
  { name: 'findWebView2DisplayedValues', category: 'WebView2 / CDP', risk: 'debug', execution: 'direct', summary: 'Cherche une valeur numérique affichée dans le DOM de la target connectée.', note: CLAUDE_ONLY_NOTE },
  { name: 'findWebView2DisplayedText', category: 'WebView2 / CDP', risk: 'debug', execution: 'direct', summary: 'Cherche un texte affiché dans le DOM de la target connectée.', note: CLAUDE_ONLY_NOTE },
  { name: 'probeWebView2GlobalScope', category: 'WebView2 / CDP', risk: 'safe', execution: 'direct', summary: 'Sonde le scope JS global (window) de la target connectée.', note: CLAUDE_ONLY_NOTE },
]
