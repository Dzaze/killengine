/**
 * Inventaire statique des outils Assistant (chat `startSmartSearch`).
 *
 * Aligné sur `ai/tool_registry.cpp` (source de vérité risque/confirmation) et
 * `docs/KILLENGINE_ASSISTANT_TOOLS_MAP.md` — 35 outils au 26/08/2026 (PHASE 148).
 * Liste volontairement statique : aucune API backend n'expose encore le registre
 * d'outils à l'UI, et un pont improvisé serait fragile. À resynchroniser
 * manuellement si le registry change (voir le compteur ASSISTANT_TOOLS_SNAPSHOT
 * et le test de non-régression côté C++ `AIToolRegistryTest`).
 */

/** Comment l'outil s'exécute réellement depuis le chat Assistant. */
export type AssistantToolExecution = 'direct' | 'confirm' | 'redirect'

/** Catégorie de risque du registre (`ai/tool_registry.cpp`, champ `risk`). */
export type AssistantToolRisk = 'safe' | 'write' | 'debug' | 'patch' | 'injection'

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
export const ASSISTANT_TOOLS_SNAPSHOT = 35

/**
 * Statuts d'exécution (PHASE 146/147/148) :
 * - `direct`   : exécuté directement par le chat, jamais d'écriture (lecture/analyse seule).
 * - `confirm`  : l'Assistant prépare l'action, un clic de confirmation UI (RiskGate) reste obligatoire.
 * - `redirect` : jamais exécuté depuis le chat, même après confirmation — toujours redirigé vers l'UI.
 */
export const assistantTools: AssistantTool[] = [
  // --- Scan mémoire ---
  { name: 'auto_resolve', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Planifie et exécute une mini-boucle sûre bornée (scan, réduction, fallbacks).' },
  { name: 'get_auto_report', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Résume contexte, télémétrie et prochaine action conseillée.' },
  { name: 'exact_scan', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Scan exact sur le processus attaché.' },
  { name: 'exact_scan_multi_type', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Scan exact multi-type quand la représentation mémoire est inconnue.' },
  { name: 'next_scan', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Réduit les candidats existants après variation.' },
  { name: 'encrypted_scan', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Scan chiffré borné XOR/Add/Sub/NOT.' },
  { name: 'trace_ui_string', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Cherche la valeur affichée en ASCII/UTF-16 (strings UI).' },
  { name: 'analyze_ui_sources', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Analyse les sources numériques proches des strings UI.' },
  { name: 'read_window_text', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Observe les titres/textes de fenêtres du processus.' },
  { name: 'start_changed_pages_diff', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Snapshot borné des pages modifiables avant variation.' },
  { name: 'finish_changed_pages_diff', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Compare le snapshot et cherche les encodages modifiés.' },
  { name: 'unknown_capture', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Capture un snapshot unknown initial borné.' },
  { name: 'unknown_compare', category: 'Scan mémoire', risk: 'safe', execution: 'direct', summary: 'Compare le snapshot unknown après variation.' },

  // --- Écriture / Freeze ---
  { name: 'prepare_write_checkpoint', category: 'Écriture / Freeze', risk: 'write', execution: 'confirm', summary: 'Prépare des candidats pour écriture, sans écrire.' },
  { name: 'write_value', category: 'Écriture / Freeze', risk: 'write', execution: 'confirm', summary: 'Écrit une valeur typée à une adresse.' },
  { name: 'freeze_value', category: 'Écriture / Freeze', risk: 'write', execution: 'confirm', summary: 'Active ou désactive un freeze.' },

  // --- Debug / Patch ---
  { name: 'find_what_writes', category: 'Debug / Patch', risk: 'debug', execution: 'redirect', summary: 'Capture l\'instruction qui écrit une adresse.', note: 'Attache un debugger et nécessite une variation live → bouton « Écrit par » dans la vue Expert.' },
  { name: 'analyze_field_stability', category: 'Debug / Patch', risk: 'debug', execution: 'direct', summary: 'Classe le rythme d\'écriture : champ affiché recalculé vs vraie source.' },
  { name: 'generate_aob', category: 'Debug / Patch', risk: 'patch', execution: 'direct', summary: 'Génère une signature AOB depuis une adresse d\'instruction.' },
  { name: 'suggest_patch', category: 'Debug / Patch', risk: 'patch', execution: 'direct', summary: 'Suggère des patchs de code sans les appliquer.' },
  { name: 'disassemble_backward', category: 'Debug / Patch', risk: 'patch', execution: 'direct', summary: 'Désassemble avant un RIP pour repérer les champs sources.' },
  { name: 'test_candidate_fields', category: 'Debug / Patch', risk: 'write', execution: 'redirect', summary: 'Teste quels champs candidats tiennent réellement.', note: 'Écrit réellement une valeur test (~1 min en tâche de fond) → vue Expert.' },

  // --- Injection / Kernel / Système ---
  { name: 'kernel_write', category: 'Injection / Kernel / Système', risk: 'injection', execution: 'confirm', summary: 'Écrit via le driver noyau (contourne les protections usermode).' },
  { name: 'speedhack_set', category: 'Injection / Kernel / Système', risk: 'injection', execution: 'confirm', summary: 'Accélère, ralentit ou remet la vitesse du processus.' },
  { name: 'block_process_network', category: 'Injection / Kernel / Système', risk: 'injection', execution: 'confirm', summary: 'Coupe ou rétablit le réseau du processus (pare-feu).' },

  // --- Fichiers de sauvegarde / UWP ---
  { name: 'discover_save_files', category: 'Fichiers de sauvegarde / UWP', risk: 'safe', execution: 'direct', summary: 'Cherche les fichiers de sauvegarde probables du processus (UWP).' },
  { name: 'inspect_local_settings', category: 'Fichiers de sauvegarde / UWP', risk: 'safe', execution: 'direct', summary: 'Inspecte en lecture seule les LocalSettings UWP.' },
  { name: 'read_save_file_text', category: 'Fichiers de sauvegarde / UWP', risk: 'safe', execution: 'direct', summary: 'Lit le contenu texte borné d\'un fichier de sauvegarde.' },
  { name: 'patch_file_bytes', category: 'Fichiers de sauvegarde / UWP', risk: 'write', execution: 'redirect', summary: 'Édite des octets dans un fichier de sauvegarde.', note: 'Écriture disque réelle → jamais exécuté depuis le chat (PHASE 148), redirige vers le panneau Fichiers de sauvegarde de la vue Expert.' },
  { name: 'watch_save_file', category: 'Fichiers de sauvegarde / UWP', risk: 'safe', execution: 'direct', summary: 'Surveille un fichier jusqu\'à changement ou timeout.' },

  // --- Trainer ---
  { name: 'trainer_list_features', category: 'Trainer', risk: 'safe', execution: 'direct', summary: 'Liste les features Trainer locales.' },
  { name: 'trainer_create_write', category: 'Trainer', risk: 'safe', execution: 'direct', summary: 'Crée une feature Trainer write sans l\'activer.' },
  { name: 'trainer_delete_feature', category: 'Trainer', risk: 'safe', execution: 'direct', summary: 'Supprime une feature Trainer par id.' },
  { name: 'trainer_apply_request', category: 'Trainer', risk: 'write', execution: 'confirm', summary: 'Prépare une demande d\'activation Trainer.', note: 'Confirmation à donner dans l\'onglet Trainer.' },
  { name: 'trainer_restore_request', category: 'Trainer', risk: 'write', execution: 'confirm', summary: 'Prépare une demande de restauration Trainer.', note: 'Confirmation à donner dans l\'onglet Trainer.' },
]