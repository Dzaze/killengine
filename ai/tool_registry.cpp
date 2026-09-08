#include "tool_registry.h"

namespace killai {

namespace {

// PHASE (Backend IA externe, T2) : chaque argument porte maintenant un type
// JSON Schema ("string"/"integer"/"number"/"boolean") et une description,
// utilisés à la fois pour générer un input_schema Anthropic correct (voir
// anthropic_tool_schema.cpp) et, potentiellement plus tard, pour enrichir le
// prompt du modèle local (llama_runtime.cpp) au lieu du texte libre actuel.
// Tous les arguments listés ici restent obligatoires (pas d'arguments
// optionnels dans le registre actuel) — voir requiredArgs().
struct ArgSpec {
    QString name;
    QString type;
    QString description;
};

ArgSpec arg(const QString& name, const QString& type, const QString& description) {
    return ArgSpec{name, type, description};
}

QVariantMap makeTool(
    const QString& name,
    const QString& description,
    const QList<ArgSpec>& args,
    const QString& risk = "safe",
    bool requiresConfirmation = false) {
    QStringList argNames;
    QVariantList argSchema;
    argNames.reserve(args.size());
    argSchema.reserve(args.size());
    for (const auto& a : args) {
        argNames << a.name;
        QVariantMap argMap;
        argMap["name"] = a.name;
        argMap["type"] = a.type;
        argMap["description"] = a.description;
        argSchema << argMap;
    }

    QVariantMap tool;
    tool["name"] = name;
    tool["description"] = description;
    tool["requiredArgs"] = argNames;
    tool["args"] = argSchema;
    tool["risk"] = risk;
    tool["safe"] = !requiresConfirmation;
    tool["requiresConfirmation"] = requiresConfirmation;
    return tool;
}

QVariantList toolDefinitions() {
    return {
        makeTool("auto_resolve", "Planifie et execute une mini-boucle safe bornee: scan/reduction/fallbacks, puis checkpoint.",
            {arg("query", "string", "Requête en langage naturel décrivant l'objectif de l'investigation.")}),
        makeTool("get_auto_report", "Resume contexte, telemetry, nextBestAction, garde-fous et strategies.", {}),
        makeTool("exact_scan", "Scan exact sur le processus attaché.",
            {arg("value", "string", "Valeur exacte actuellement affichée à l'écran à rechercher."),
             arg("valueType", "string", "Type numérique de la valeur (ex: int32, int16, int64, float, double).")}),
        makeTool("exact_scan_module", "Scan exact borne a un module/DLL charge precis, via start/stop address du module. Lecture seule.",
            {arg("module", "string", "Nom du module/DLL (ex: game.exe) sur lequel borner le scan."),
             arg("value", "string", "Valeur exacte actuellement affichée à l'écran à rechercher."),
             arg("valueType", "string", "Type numérique de la valeur (ex: int32, int16, int64, float, double).")}),
        makeTool("exact_scan_multi_type", "Scan exact multi-type quand la representation memoire est inconnue.",
            {arg("value", "string", "Valeur affichée à l'écran, testée sur plusieurs types numériques faute de connaître sa représentation mémoire.")}),
        makeTool("next_scan", "Réduit les candidats existants.",
            {arg("mode", "string", "Mode de réduction des candidats existants (ex: increased, decreased, changed, unchanged, exact).")}),
        makeTool("get_candidates", "Liste les candidats actuels (adresse, type, dernière valeur, confiance) issus du dernier exact_scan/next_scan. À utiliser une fois la liste réduite à un petit nombre de candidats, pour obtenir une adresse réelle avant de proposer write_value/freeze_value — ne jamais inventer une adresse, toujours passer par cet outil. Si le nombre de candidats est encore trop grand, le résultat revient tronqué (displaySuppressed) : continue à réduire avec next_scan avant de rappeler cet outil. Il peut légitimement y avoir plusieurs adresses réelles pour une même valeur logique (copies redondantes, checksums) — ne suppose pas qu'une seule adresse est forcément la bonne, la liste peut en montrer plusieurs à considérer. Lecture seule, exécute directement.", {}),
        makeTool("encrypted_scan", "Scan chiffre borne XOR/Add/Sub/NOT sur valeur entiere affichee.",
            {arg("value", "string", "Valeur entière affichée à l'écran à rechercher sous forme chiffrée (XOR/Add/Sub/NOT)."),
             arg("valueType", "string", "Type numérique entier de la valeur (ex: int32, int16, int64).")}),
        makeTool("trace_ui_string", "Cherche la valeur affichee en ASCII/UTF-16 puis prepare analyse source.",
            {arg("value", "string", "Valeur actuellement affichée à l'écran, sous forme de texte, à rechercher en mémoire.")}),
        makeTool("analyze_ui_sources", "Analyse les sources numeriques proches des strings UI confirmees.",
            {arg("value", "string", "Valeur affichée à l'écran servant de point de départ pour analyser les sources numériques proches.")}),
        makeTool("read_window_text", "Observe les titres/textes de fenetres lies au processus attache pour synchroniser une investigation UI.", {}),
        makeTool("list_process_modules", "Liste les DLL/modules charges par le processus attache pour reperer le module applicatif pertinent avant AOB/desassemblage. Lecture seule.", {}),
        makeTool("start_changed_pages_diff", "Capture un snapshot borne des pages privees/writable avant une variation affichee; lecture seule.", {}),
        makeTool("finish_changed_pages_diff", "Compare le snapshot de pages apres variation et cherche les encodages numeriques modifies.",
            {arg("previousValue", "string", "Valeur affichée à l'écran avant la variation observée."),
             arg("currentValue", "string", "Valeur affichée à l'écran après la variation observée.")}),
        makeTool("unknown_capture", "Capture un snapshot unknown initial borne.", {}),
        makeTool("unknown_compare", "Compare le snapshot unknown initial apres variation utilisateur.",
            {arg("mode", "string", "Mode de comparaison du snapshot (ex: increased, decreased, changed, unchanged)."),
             arg("valueType", "string", "Type numérique attendu (ex: int32, float).")}),
        makeTool("prepare_write_checkpoint", "Prepare des candidats pour ecriture confirmee, sans ecrire.",
            {arg("value", "string", "Valeur candidate à préparer pour une écriture ultérieure confirmée.")}, "write", true),
        makeTool("write_value", "Écrit une valeur typée à une adresse apres confirmation explicite.",
            {arg("address", "string", "Adresse mémoire hexadécimale cible (ex: 0x1A2B3C4D)."),
             arg("valueType", "string", "Type numérique de la valeur à écrire (ex: int32, float)."),
             arg("value", "string", "Valeur à écrire à l'adresse cible.")}, "write", true),
        makeTool("freeze_value", "Active ou désactive un freeze apres confirmation explicite.",
            {arg("address", "string", "Adresse mémoire hexadécimale cible (ex: 0x1A2B3C4D)."),
             arg("valueType", "string", "Type numérique de la valeur figée (ex: int32, float)."),
             arg("value", "string", "Valeur à maintenir tant que le freeze est actif."),
             arg("enabled", "boolean", "true pour activer le freeze, false pour le désactiver.")}, "write", true),
        // PHASE 140 : requiredArgs vide -- ApplicationController::startSmartSearch
        // ne fait plus jamais executer ce tool depuis le chat (redirige
        // systematiquement vers l'UI, voir son dispatch), donc "address"/"size"
        // ne sont plus vraiment requis a ce niveau : les exiger bloquait le
        // validateur AVANT d'atteindre le message de redirection utile.
        makeTool("find_what_writes", "Capture l'instruction qui ecrit une adresse. Depuis le chat, redirige vers l'UI (attache un debugger, necessite une variation live de la valeur) plutot que d'executer.", {}, "debug", true),
        makeTool("analyze_field_stability", "Observe passivement (aucune écriture) le rythme des écritures sur une adresse candidate pour juger si elle ressemble à un champ affiché recalculé à chaque tick (\"likely_derived_display\") ou à une source événementielle (\"likely_event_driven\"). Attache brièvement un debugger comme find_what_writes, mais n'écrit jamais rien — exécuté directement, sans confirmation RiskGate. Utile avant de figer/patcher une adresse trouvée par scan : un champ affiché ne tiendra probablement pas en écriture directe, chercher la source en amont plutôt (voir disassemble_backward).",
            {arg("address", "string", "Adresse mémoire hexadécimale candidate à observer (ex: 0x1A2B3C4D).")}, "debug", false),
        // PHASE 140 : les 3 tools ci-dessous restent categorie "patch" pour la
        // taxonomie (etapes d'un workflow de patch), mais requiresConfirmation
        // passe a false -- verifie explicitement (PHASE 140) qu'aucun des trois
        // n'ecrit quoi que ce soit (lecture memoire seule + calcul), meme
        // precedent que analyze_field_stability (PHASE 130/131, risk=debug mais
        // requiresConfirmation=false car jamais d'ecriture). L'ecriture reelle
        // reste derriere applyCodePatch (tool absent de ce registre cote chat --
        // seul le pipe/UI l'expose), qui garde sa propre confirmation.
        makeTool("generate_aob", "Genere une signature AOB depuis une instruction confirmee. Lecture seule (aucune ecriture), execute directement.",
            {arg("address", "string", "Adresse mémoire hexadécimale de l'instruction confirmée (ex: 0x1A2B3C4D).")}, "patch", false),
        makeTool("suggest_patch", "Suggere un patch code sans application automatique. Lecture seule, execute directement.",
            {arg("address", "string", "Adresse mémoire hexadécimale de l'instruction à patcher (ex: 0x1A2B3C4D).")}, "patch", false),
        makeTool("disassemble_backward", "Désassemble les instructions avant un RIP capturé pour repérer les champs sources d'un compteur animé. Lecture seule, execute directement.",
            {arg("address", "string", "Adresse mémoire hexadécimale du RIP capturé (ex: 0x1A2B3C4D).")}, "patch", false),
        // PHASE 140 : requiredArgs vide, meme raison que find_what_writes
        // ci-dessus -- redirige systematiquement vers l'UI depuis le chat.
        makeTool("test_candidate_fields", "Teste automatiquement lequel des champs candidats (issus de disassemble_backward) tient réellement : écrit une valeur test, attend, relit, classe holds/reverts, puis restaure. Depuis le chat, redirige vers l'UI (écrit réellement, tourne ~1 minute en tâche de fond) plutôt que d'exécuter.", {}, "write", true),
        makeTool("kernel_write", "Écrit une valeur via le driver noyau (contourne les protections mémoire usermode). À utiliser seulement si l'utilisateur le demande explicitement (ex: \"écris via le kernel\") ou après échec d'une écriture usermode normale.",
            {arg("address", "string", "Adresse mémoire hexadécimale cible (ex: 0x1A2B3C4D)."),
             arg("valueType", "string", "Type numérique de la valeur à écrire (ex: int32, float)."),
             arg("value", "string", "Valeur à écrire à l'adresse cible.")}, "injection", true),
        makeTool("speedhack_set", "Accélère, ralentit, ou remet à la normale la vitesse perçue par le processus attaché (hook des fonctions de temps). mode=\"set\" avec un facteur (ex: 2.0 = 2x plus vite, 0.5 = 2x plus lent, 0.0 = pause), ou mode=\"off\" pour désactiver et revenir à la normale.",
            {arg("factor", "number", "Facteur de vitesse appliqué (ex: 2.0 = 2x plus vite, 0.5 = 2x plus lent, 0.0 = pause)."),
             arg("mode", "string", "\"set\" pour appliquer factor, \"off\" pour désactiver et revenir à la normale.")}, "injection", true),
        makeTool("block_process_network", "Coupe (ou rétablit, mode=\"off\") le réseau entrant/sortant du processus attaché via une règle pare-feu Windows dédiée. Utile pour isoler si une valeur mémoire instable vient d'une synchro serveur en arrière-plan plutôt que d'une réallocation purement locale, avant de conclure à une réallocation locale.",
            {arg("mode", "string", "\"on\" pour couper le réseau du processus attaché, \"off\" pour le rétablir.")}, "injection", true),
        // Réseau — outils de diagnostic (lecture seule)
        makeTool("get_process_network_connections", "Liste les connexions TCP/UDP actives du processus attaché avec résolution DNS. Lecture seule, exécute directement.", {}),
        makeTool("get_process_network_modules", "Liste les modules DLL réseau chargés par le processus attaché (wininet, winhttp, ws2_32, etc.). Lecture seule, exécute directement.", {}),
        // Réseau — outils d'action (risque élevé, confirmation requise)
        makeTool("start_http_proxy", "Intercepte les requêtes HTTP/HTTPS du processus attaché via injection DLL + hook WinINet/WinHTTP. Permet de modifier les body de requête en temps réel.",
            {arg("port", "integer", "Port TCP local d'écoute du proxy."),
             arg("interceptHttps", "boolean", "true pour intercepter aussi le trafic HTTPS (nécessite un certificat local).")}, "injection", true),
        makeTool("stop_http_proxy", "Arrête le proxy HTTP actif et retire les hooks.", {}, "injection", true),
        makeTool("get_http_proxy_requests", "Liste les requêtes HTTP interceptées par le proxy actif. Lecture seule, exécute directement.", {}),
        makeTool("modify_http_request", "Modifie le body d'une requête HTTP interceptée avant qu'elle ne soit envoyée.",
            {arg("requestId", "string", "Identifiant de la requête HTTP interceptée à modifier."),
             arg("newBody", "string", "Nouveau corps de requête à envoyer à la place de l'original.")}, "injection", true),
        makeTool("spoof_dns", "Redirige un domaine vers une IP locale (ex: 127.0.0.1) via le fichier hosts Windows. Nécessite UAC.",
            {arg("domain", "string", "Nom de domaine à rediriger (ex: example.com)."),
             arg("targetIp", "string", "Adresse IP locale vers laquelle rediriger le domaine (ex: 127.0.0.1).")}, "injection", true),
        makeTool("restore_dns", "Retire l'entrée spoofée du fichier hosts pour un domaine.",
            {arg("domain", "string", "Nom de domaine dont l'entrée spoofée doit être retirée du fichier hosts.")}, "injection", true),
        makeTool("set_lag_switch", "Retarde artificiellement les fonctions recv/WSARecv du processus attaché de X ms via injection DLL + MinHook. Utile pour tester la tolérance réseau d'un jeu.",
            {arg("enabled", "boolean", "true pour activer le retard artificiel, false pour le désactiver."),
             arg("delayMs", "integer", "Retard en millisecondes appliqué aux fonctions recv/WSARecv.")}, "injection", true),
        makeTool("discover_save_files", "Cherche les fichiers de sauvegarde/état probables du processus attaché (dossier LocalState de son package UWP), triés par date de modification récente. À proposer quand le scan mémoire classique échoue de façon répétée (valeur instable/réallouée) : la valeur affichée vient peut-être d'un fichier sur disque plutôt que d'une adresse mémoire stable — voir l'investigation Solitaire \"Bulles\" (docs/PHASE_TRACKER.md PHASE 90) où cette bascule a débloqué la recherche après plusieurs échecs mémoire.", {}),
        makeTool("inspect_local_settings", "Inspecte en lecture seule la ruche UWP Settings\\settings.dat du processus attaché (LocalSettings) avec RegLoadAppKeyW, sans montage global ni écriture registre. Utile quand discover_save_files ne montre pas la valeur dans un fichier de sauvegarde évident : certains jeux UWP stockent scores, options, timestamps ou états courts dans LocalSettings.", {}),
        makeTool("read_save_file_text", "Lit le contenu (décodé en texte imprimable, borné en taille) d'un fichier de sauvegarde trouvé via discover_save_files. Sert à repérer le champ correspondant à la valeur affichée à l'écran (ex: un compteur nommé dans du JSON) sans avoir à scanner la mémoire.",
            {arg("path", "string", "Chemin du fichier de sauvegarde trouvé via discover_save_files.")}),
        makeTool("patch_file_bytes", "Edite en place une sequence d'octets dans un fichier de sauvegarde (trouve via discoverProcessSaveFiles), apres confirmation explicite. La sequence de remplacement doit faire exactement la meme longueur que celle recherchee, et ne doit apparaitre qu'une seule fois dans le fichier (sinon l'action est refusee pour eviter de modifier le mauvais champ).",
            {arg("path", "string", "Chemin du fichier de sauvegarde à modifier."),
             arg("findHex", "string", "Séquence d'octets à rechercher, en hexadécimal."),
             arg("replaceHex", "string", "Séquence de remplacement en hexadécimal, de même longueur exacte que findHex.")}, "write", true),
        makeTool("watch_save_file", "Surveille un fichier de sauvegarde (trouvé via discover_save_files) et attend une écriture/suppression/renommage, via surveillance native du dossier (sans dépendance externe type Process Monitor). Bloquant jusqu'à timeoutMs (défaut 5000) ou jusqu'au premier changement détecté. Utile pour confirmer QUAND un fichier est réécrit par le jeu (ex: juste après une action utilisateur), avant de tenter une édition avec patch_file_bytes.",
            {arg("path", "string", "Chemin du fichier de sauvegarde à surveiller.")}),
        makeTool("trainer_list_features", "Liste en lecture seule les features Trainer locales via le pont UI/Pinia.", {}),
        makeTool("trainer_create_write", "Crée une feature Trainer simple de type write depuis une adresse explicite et une valeur. Ne l'active pas automatiquement.",
            {arg("address", "string", "Adresse mémoire hexadécimale cible (ex: 0x1A2B3C4D)."),
             arg("valueType", "string", "Type numérique de la valeur (ex: int32, float)."),
             arg("value", "string", "Valeur à écrire lorsque la feature Trainer sera activée.")}),
        makeTool("trainer_delete_feature", "Supprime une feature Trainer locale par id explicite.",
            {arg("id", "string", "Identifiant de la feature Trainer locale à supprimer.")}),
        makeTool("trainer_apply_request", "Prépare une demande d'activation Trainer mais ne clique pas le RiskGate UI. L'utilisateur doit confirmer dans l'onglet Trainer.", {}, "write", true),
        makeTool("trainer_restore_request", "Prépare une demande de restauration/désactivation Trainer mais ne clique pas le RiskGate UI. L'utilisateur doit confirmer dans l'onglet Trainer.", {}, "write", true),
        // PHASE 256 : Mode discret / Stealth mode
        makeTool("apply_stealth_mode", "Active le mode discret pour masquer KillEngine du processus cible. Profils : 'sc2' (tous les modules anti-détection), 'default' (anti-debug seul), 'minimal' (masquage processus seul). À utiliser quand l'utilisateur mentionne un jeu qui détecte les outils mémoire.",
            {arg("profile", "string", "Profil de discrétion à appliquer : 'sc2', 'default' ou 'minimal'.")}, "injection", true),
        makeTool("restore_stealth_mode", "Désactive le mode discret et restaure l'état original des modules activés.", {}, "injection", true),
        makeTool("get_stealth_status", "Retourne l'état courant du mode discret (actif ou non, profil utilisé, modules activés). Lecture seule, exécute directement.", {}, "safe", false),
        // WEBVIEW-C : exposition LLM des Q_INVOKABLE ApplicationController
        // ajoutes en WEBVIEW-D. Connexion externe et JS arbitraire restent
        // derriere RiskGate (requiresConfirmation=true) ; les lectures DOM et
        // le listing de targets sont des observations controlees.
        makeTool("getWebView2InspectorStatus", "Retourne l'etat courant de l'inspecteur WebView2/CDP : connecte ou non, endpoint utilise, target active.", {}, "safe", false),
        makeTool("listWebView2CdpTargets", "Liste les targets CDP WebView2 disponibles pour un PID browser WebView2 donne. Utiliser browserProcessId=0 pour lister toutes les targets visibles via le port direct ou le fallback WDP /msedge avant de choisir une target.",
            {arg("browserProcessId", "integer", "PID du process browser WebView2 hôte, ou 0 pour lister toutes les targets visibles.")}, "debug", false),
        makeTool("connectWebView2Inspector", "Connecte l'inspecteur WebView2 a une target CDP par PID/filtres/options ou webSocketDebuggerUrl explicite. Action sensible car elle s'attache a un process externe : confirmation RiskGate obligatoire.",
            {arg("browserProcessId", "integer", "PID du process browser WebView2 hôte auquel se connecter.")}, "debug", true),
        makeTool("disconnectWebView2Inspector", "Deconnecte l'inspecteur WebView2/CDP courant.", {}, "safe", false),
        makeTool("evaluateWebView2JavaScript", "Evalue une expression JavaScript arbitraire dans la target WebView2 connectee. Peut lire ou modifier l'etat JS selon le code fourni : confirmation RiskGate obligatoire.",
            {arg("expression", "string", "Expression JavaScript à évaluer dans la target WebView2 connectée.")}, "script", true),
        makeTool("findWebView2DisplayedValues", "Cherche une valeur numerique affichee dans le DOM de la target WebView2 connectee via une expression JS controlee.",
            {arg("value", "string", "Valeur numérique affichée à rechercher dans le DOM de la target WebView2 connectée.")}, "debug", false),
        makeTool("findWebView2DisplayedText", "Cherche un texte affiche dans le DOM de la target WebView2 connectee via une expression JS controlee.",
            {arg("text", "string", "Texte affiché à rechercher dans le DOM de la target WebView2 connectée.")}, "debug", false),
        // WEBVIEW-F : reconnaissance automatique du contexte JS a la connexion,
        // pour eviter de deviner du JS a l'aveugle (Object.keys(window), etc.).
        makeTool("probeWebView2GlobalScope", "Sonde le scope global JS (window) de la target WebView2 connectee et isole les variables/fonctions ajoutees par la page (SDK, etat de jeu) du bruit natif Chromium, via une baseline dynamique (about:blank) quand possible. Donne aussi un apercu structure (nombre de <video>/<audio>, srcs des <iframe>). A utiliser juste apres connectWebView2Inspector plutot que de deviner du JS a l'aveugle. Lecture seule.", {}, "safe", false),
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
