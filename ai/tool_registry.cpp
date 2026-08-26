#include "tool_registry.h"

namespace killai {

namespace {

QVariantMap makeTool(
    const QString& name,
    const QString& description,
    const QStringList& requiredArgs,
    const QString& risk = "safe",
    bool requiresConfirmation = false) {
    QVariantMap tool;
    tool["name"] = name;
    tool["description"] = description;
    tool["requiredArgs"] = requiredArgs;
    tool["risk"] = risk;
    tool["safe"] = !requiresConfirmation;
    tool["requiresConfirmation"] = requiresConfirmation;
    return tool;
}

QVariantList toolDefinitions() {
    return {
        makeTool("auto_resolve", "Planifie et execute une mini-boucle safe bornee: scan/reduction/fallbacks, puis checkpoint.", {"query"}),
        makeTool("get_auto_report", "Resume contexte, telemetry, nextBestAction, garde-fous et strategies.", {}),
        makeTool("exact_scan", "Scan exact sur le processus attaché.", {"value", "valueType"}),
        makeTool("exact_scan_multi_type", "Scan exact multi-type quand la representation memoire est inconnue.", {"value"}),
        makeTool("next_scan", "Réduit les candidats existants.", {"mode"}),
        makeTool("encrypted_scan", "Scan chiffre borne XOR/Add/Sub/NOT sur valeur entiere affichee.", {"value", "valueType"}),
        makeTool("trace_ui_string", "Cherche la valeur affichee en ASCII/UTF-16 puis prepare analyse source.", {"value"}),
        makeTool("analyze_ui_sources", "Analyse les sources numeriques proches des strings UI confirmees.", {"value"}),
        makeTool("read_window_text", "Observe les titres/textes de fenetres lies au processus attache pour synchroniser une investigation UI.", {}),
        makeTool("start_changed_pages_diff", "Capture un snapshot borne des pages privees/writable avant une variation affichee; lecture seule.", {}),
        makeTool("finish_changed_pages_diff", "Compare le snapshot de pages apres variation et cherche les encodages numeriques modifies.", {"previousValue", "currentValue"}),
        makeTool("unknown_capture", "Capture un snapshot unknown initial borne.", {}),
        makeTool("unknown_compare", "Compare le snapshot unknown initial apres variation utilisateur.", {"mode", "valueType"}),
        makeTool("prepare_write_checkpoint", "Prepare des candidats pour ecriture confirmee, sans ecrire.", {"value"}, "write", true),
        makeTool("write_value", "Écrit une valeur typée à une adresse apres confirmation explicite.", {"address", "valueType", "value"}, "write", true),
        makeTool("freeze_value", "Active ou désactive un freeze apres confirmation explicite.", {"address", "valueType", "value", "enabled"}, "write", true),
        makeTool("find_what_writes", "Capture l'instruction qui ecrit une adresse apres confirmation explicite.", {"address", "size"}, "debug", true),
        makeTool("generate_aob", "Genere une signature AOB depuis une instruction confirmee.", {"address"}, "patch", true),
        makeTool("suggest_patch", "Suggere un patch code sans application automatique.", {"address"}, "patch", true),
        makeTool("disassemble_backward", "Désassemble les instructions avant un RIP capturé pour repérer les champs sources d'un compteur animé.", {"address"}, "patch", true),
        makeTool("test_candidate_fields", "Teste automatiquement lequel des champs candidats (issus de disassemble_backward) tient réellement : écrit une valeur test, attend, relit, classe holds/reverts, puis restaure. Remplace la lecture manuelle d'assembleur par une preuve empirique.", {"address", "watchedAddress"}, "write", true),
        makeTool("kernel_write", "Écrit une valeur via le driver noyau (contourne les protections mémoire usermode). À utiliser seulement si l'utilisateur le demande explicitement (ex: \"écris via le kernel\") ou après échec d'une écriture usermode normale.", {"address", "valueType", "value"}, "injection", true),
        makeTool("speedhack_set", "Accélère, ralentit, ou remet à la normale la vitesse perçue par le processus attaché (hook des fonctions de temps). mode=\"set\" avec un facteur (ex: 2.0 = 2x plus vite, 0.5 = 2x plus lent, 0.0 = pause), ou mode=\"off\" pour désactiver et revenir à la normale.", {"factor", "mode"}, "injection", true),
        makeTool("block_process_network", "Coupe (ou rétablit, mode=\"off\") le réseau entrant/sortant du processus attaché via une règle pare-feu Windows dédiée. Utile pour isoler si une valeur mémoire instable vient d'une synchro serveur en arrière-plan plutôt que d'une réallocation purement locale, avant de conclure à une réallocation locale.", {"mode"}, "injection", true),
        makeTool("discover_save_files", "Cherche les fichiers de sauvegarde/état probables du processus attaché (dossier LocalState de son package UWP), triés par date de modification récente. À proposer quand le scan mémoire classique échoue de façon répétée (valeur instable/réallouée) : la valeur affichée vient peut-être d'un fichier sur disque plutôt que d'une adresse mémoire stable — voir l'investigation Solitaire \"Bulles\" (docs/PHASE_TRACKER.md PHASE 90) où cette bascule a débloqué la recherche après plusieurs échecs mémoire.", {}),
        makeTool("inspect_local_settings", "Inspecte en lecture seule la ruche UWP Settings\\settings.dat du processus attaché (LocalSettings) avec RegLoadAppKeyW, sans montage global ni écriture registre. Utile quand discover_save_files ne montre pas la valeur dans un fichier de sauvegarde évident : certains jeux UWP stockent scores, options, timestamps ou états courts dans LocalSettings.", {}),
        makeTool("read_save_file_text", "Lit le contenu (décodé en texte imprimable, borné en taille) d'un fichier de sauvegarde trouvé via discover_save_files. Sert à repérer le champ correspondant à la valeur affichée à l'écran (ex: un compteur nommé dans du JSON) sans avoir à scanner la mémoire.", {"path"}),
        makeTool("patch_file_bytes", "Edite en place une sequence d'octets dans un fichier de sauvegarde (trouve via discoverProcessSaveFiles), apres confirmation explicite. La sequence de remplacement doit faire exactement la meme longueur que celle recherchee, et ne doit apparaitre qu'une seule fois dans le fichier (sinon l'action est refusee pour eviter de modifier le mauvais champ).", {"path", "findHex", "replaceHex"}, "write", true),
        makeTool("watch_save_file", "Surveille un fichier de sauvegarde (trouvé via discover_save_files) et attend une écriture/suppression/renommage, via surveillance native du dossier (sans dépendance externe type Process Monitor). Bloquant jusqu'à timeoutMs (défaut 5000) ou jusqu'au premier changement détecté. Utile pour confirmer QUAND un fichier est réécrit par le jeu (ex: juste après une action utilisateur), avant de tenter une édition avec patch_file_bytes.", {"path"}),
        makeTool("trainer_list_features", "Liste en lecture seule les features Trainer locales via le pont UI/Pinia.", {}),
        makeTool("trainer_create_write", "Crée une feature Trainer simple de type write depuis une adresse explicite et une valeur. Ne l'active pas automatiquement.", {"address", "valueType", "value"}),
        makeTool("trainer_delete_feature", "Supprime une feature Trainer locale par id explicite.", {"id"}),
        makeTool("trainer_apply_request", "Prépare une demande d'activation Trainer mais ne clique pas le RiskGate UI. L'utilisateur doit confirmer dans l'onglet Trainer.", {}, "write", true),
        makeTool("trainer_restore_request", "Prépare une demande de restauration/désactivation Trainer mais ne clique pas le RiskGate UI. L'utilisateur doit confirmer dans l'onglet Trainer.", {}, "write", true),
    };
}

} // namespace

ToolRegistry::ToolRegistry(QObject* parent)
    : QObject(parent) {
}

QVariantList ToolRegistry::availableTools() const {
    return toolDefinitions();
}

bool ToolRegistry::hasTool(const QString& name) const {
    for (const auto& item : toolDefinitions()) {
        if (item.toMap().value("name").toString() == name) {
            return true;
        }
    }
    return false;
}

QStringList ToolRegistry::requiredArgs(const QString& name) const {
    for (const auto& item : toolDefinitions()) {
        const auto tool = item.toMap();
        if (tool.value("name").toString() == name) {
            return tool.value("requiredArgs").toStringList();
        }
    }
    return {};
}

QVariantMap ToolRegistry::toolMetadata(const QString& name) const {
    for (const auto& item : toolDefinitions()) {
        const auto tool = item.toMap();
        if (tool.value("name").toString() == name) {
            return tool;
        }
    }
    return {};
}

} // namespace killai
