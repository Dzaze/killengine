#include "ai_engine.h"
#include "investigation_notebook_planner.h"
#include "intent_contract.h"
#include "query_text_utils.h"
#include "logging/logger.h"

#include <QCoreApplication>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QVariantList>

namespace killai {

namespace {

constexpr int kMaxHistoryTurns = 12;

bool looksLikeBadTargets(const QString& q) {
    return q.contains("marche pas") || q.contains("marché pas") || q.contains("pas marché")
        || q.contains("ne marche pas") || q.contains("mauvaise adresse")
        || q.contains("pas bon") || q.contains("rien change");
}

bool describesIncrease(const QString& q) {
    return q.contains("augment") || q.contains("increased") || q.contains("monte")
        || q.contains("plus grand") || q.contains("plus haut");
}

bool describesDecrease(const QString& q) {
    return q.contains("diminu") || q.contains("decreased") || q.contains("baisse")
        || q.contains("descend") || q.contains("plus petit") || q.contains("plus bas");
}

bool describesChange(const QString& q) {
    return q.contains("chang") || q.contains("change") || q.contains("varie")
        || q.contains("différent") || q.contains("different");
}

bool describesStable(const QString& q) {
    return q.contains("pareil") || q.contains("stable") || q.contains("inchang")
        || q.contains("unchanged") || q.contains("bouge pas");
}

bool hasNegatedFreezeInstruction(const QString& q) {
    return q.contains("sans freeze") || q.contains("sans freezer")
        || q.contains("sans geler") || q.contains("sans figer")
        || q.contains("ni freeze") || q.contains("ni freezer")
        || q.contains("ni geler") || q.contains("ni figer")
        || q.contains("pas de freeze") || q.contains("pas freeze")
        || q.contains("ne freeze pas") || q.contains("ne pas freeze")
        || q.contains("ne pas freezer") || q.contains("without freeze")
        || q.contains("without freezing") || q.contains("no freeze")
        || q.contains("do not freeze") || q.contains("don't freeze");
}

bool wantsInspectorMode(const QString& q) {
    return q.contains("inspecteur") || q.contains("inspector") || q.contains("codex")
        || q.contains("enquete") || q.contains("enquête") || q.contains("raisonne")
        || q.contains("comprendre") || q.contains("preuve");
}

bool describesUiCopyOrBuffer(const QString& q) {
    return q.contains("copie ui") || q.contains("buffer") || q.contains("buffers")
        || q.contains("string instable") || q.contains("texte instable")
        || q.contains("affichage decouple") || q.contains("affichage découpl")
        || q.contains("pas ecrit sur place") || q.contains("pas écrit sur place");
}

bool wantsModuleSourcePivot(const QString& q) {
    const bool mentionsModule = q.contains("dll") || q.contains("module") || q.contains("modules")
        || q.contains("code du jeu") || q.contains("game code");
    const bool mentionsSource = q.contains("vraie source") || q.contains("source xp")
        || q.contains("source gameplay") || q.contains("copie affiche") || q.contains("copie affichée")
        || q.contains("pas juste une copie") || q.contains("not just a copy");
    const bool saysScanFailed = q.contains("ne converge pas") || q.contains("ne convergent pas")
        || q.contains("converge pas") || q.contains("marche pas") || q.contains("marchera pas")
        || q.contains("ne va pas marcher") || q.contains("won't work") || q.contains("does not converge")
        || q.contains("doesn't converge");
    return mentionsModule && (mentionsSource || saysScanFailed);
}

bool wantsProcessModuleListing(const QString& q) {
    const bool asksList = q.contains("liste") || q.contains("lister") || q.contains("enumere")
        || q.contains("énumère") || q.contains("montre") || q.contains("affiche")
        || q.contains("list") || q.contains("show");
    const bool mentionsModule = q.contains("dll") || q.contains("module") || q.contains("modules");
    return mentionsModule && asksList;
}

struct AddressToolMatch {
    QString tool;
    QVariantMap args;
    QString rationale;
};

QString moduleNameFromQuery(const QString& query) {
    const QRegularExpression explicitModuleRe(R"(([A-Za-z0-9_.-]+\.(?:dll|exe)))", QRegularExpression::CaseInsensitiveOption);
    const auto match = explicitModuleRe.match(query);
    if (match.hasMatch()) {
        return match.captured(1);
    }

    const QString q = query.toLower();
    if (q.contains("webview")) {
        return "WebView";
    }
    if (q.contains("solitaire")) {
        return "Solitaire";
    }
    return {};
}

QStringList decimalNumbersFromQuery(const QString& query) {
    QString text = query;
    const QRegularExpression hexRe(R"(\b0x[0-9a-fA-F]{5,16}\b)");
    text.replace(hexRe, " ");
    QStringList numbers;
    const QRegularExpression numberRe(R"([-+]?\d+(?:[\.,]\d+)?)");
    auto it = numberRe.globalMatch(text);
    while (it.hasNext()) {
        numbers.append(it.next().captured(0).replace(',', '.'));
    }
    return numbers;
}

QString inferredValueTypeFromQuery(const QString& query) {
    const QString q = query.toLower();
    if (q.contains("uint8") || q.contains("u8") || q.contains("byte")) return "UInt8";
    if (q.contains("int8") || q.contains("i8")) return "Int8";
    if (q.contains("uint16") || q.contains("u16")) return "UInt16";
    if (q.contains("int16") || q.contains("i16") || q.contains("short")) return "Int16";
    if (q.contains("uint32") || q.contains("u32")) return "UInt32";
    if (q.contains("uint64") || q.contains("u64")) return "UInt64";
    if (q.contains("float64") || q.contains("double")) return "Float64";
    if (q.contains("float32") || q.contains("float")) return "Float32";
    if (q.contains("int64") || q.contains("long")) return "Int64";
    return "Int32";
}

AddressToolMatch matchModuleExactScanTool(const QString& query) {
    const QString module = moduleNameFromQuery(query);
    if (module.isEmpty()) {
        return {};
    }

    const QStringList numbers = decimalNumbersFromQuery(query);
    if (numbers.isEmpty()) {
        return {};
    }

    const QString q = query.toLower();
    const bool wantsModuleBoundedSearch = q.contains("dll") || q.contains("module")
        || q.contains("utilise") || q.contains("use ") || q.contains("dans ");
    if (!wantsModuleBoundedSearch) {
        return {};
    }

    QVariantMap args;
    args["module"] = module;
    args["value"] = numbers.first();
    args["valueType"] = inferredValueTypeFromQuery(query);
    if (numbers.size() >= 2) {
        args["targetValue"] = numbers.at(1);
    }
    return {"exact_scan_module", args,
        "Je limite le scan exact au module/DLL indiqué au lieu de scanner tout le processus."};
}

QVariantMap makeModuleSourcePivotResponse(const QString& stateName) {
    QVariantMap result;
    result["status"] = "needs_clarification";
    result["actionStatus"] = "not_executed";
    result["message"] = "D'accord, on arrête de réduire en exact/increased : tu demandes un pivot vers les DLL/modules et la vraie source XP. "
                        "Liste d'abord les modules du processus, repère le module applicatif Solitaire/WebView pertinent, puis utilise AOB/désassemblage ou Trace UI string/Changed Pages pour relier l'affichage XP à la source.";
    result["state"] = stateName;
    QVariantList recoveryActions;
    recoveryActions.append(QVariantMap{{"id", "open_expert"}, {"label", "Ouvrir Expert"}, {"expertStep", "inspect"}});
    recoveryActions.append(QVariantMap{{"id", "trace_ui_string"}, {"label", "Trace UI string"}});
    recoveryActions.append(QVariantMap{{"id", "start_changed_pages_diff"}, {"label", "Changed Pages"}});
    result["recoveryActions"] = recoveryActions;
    return result;
}

QString variationMode(const QString& q, const QString& fallback = "changed") {
    if (describesIncrease(q)) return "increased";
    if (describesDecrease(q)) return "decreased";
    if (describesStable(q)) return "unchanged";
    if (describesChange(q)) return "changed";
    return fallback;
}

/// Un match d'outil "hors memoire" (fichiers de sauvegarde UWP, LocalSettings,
/// surveillance fichier) trouve par mots-cles explicites FR/EN dans la requete.
/// tool vide = aucun match.
struct OffMemoryToolMatch {
    QString tool;
    QString rationale;
};

/// PHASE 91/99 : identifie un outil d'investigation "hors memoire" a partir de
/// mots-cles explicites dans la requete (voir docs/PHASE_TRACKER.md PHASE 90,
/// investigation Solitaire "Bulles" -- la valeur affichee vient parfois d'un
/// fichier sur disque plutot que d'une adresse memoire stable). Une seule
/// liste de mots-cles, utilisee a la fois par le fast-path de
/// AIEngine::processQuery (court-circuite le modele local avant de le lancer)
/// et par deterministicPlanWithContext (repli normal si le modele echoue) --
/// jamais deux copies qui pourraient diverger.
OffMemoryToolMatch matchOffMemoryTool(const QString& q) {
    if (q.contains("localsettings") || q.contains("local settings") || q.contains("settings.dat")
        || q.contains("ruche registre") || q.contains("registre uwp") || q.contains("registry hive")) {
        return {"inspect_local_settings",
            "J'inspecte en lecture seule la ruche LocalSettings/settings.dat du package UWP attache."};
    }

    // Sans chemin de fichier deja connu, "surveiller" n'est pas actionnable
    // directement (watch_save_file exige un path) -- on route d'abord vers
    // la decouverte, etape necessaire de toute facon avant de surveiller.
    if (q.contains("watch fichier") || q.contains("surveille fichier") || q.contains("surveiller fichier")
        || q.contains("surveillance fichier") || q.contains("watch file") || q.contains("file watch")) {
        return {"discover_save_files",
            "Je cherche d'abord les fichiers de sauvegarde disponibles -- tu pourras ensuite me demander de surveiller l'un d'eux."};
    }

    if (q.contains("fichier de sauvegarde") || q.contains("fichiers de sauvegarde")
        || q.contains("sauvegarde disque") || q.contains("sur le disque") || q.contains("on disk")
        || q.contains("dans un fichier") || q.contains("save file") || q.contains("savefile")
        || q.contains("localstate")) {
        return {"discover_save_files",
            "Je cherche les fichiers de sauvegarde/etat du processus attache sur le disque."};
    }

    return {};
}

/// PHASE 120-B (29/08/2026) : redirections de navigation SANS risque propre
/// (aucun tool_call, aucune ecriture) associees a chaque sujet du playbook --
/// le risque reel reste entierement dans les outils vers lesquels on redirige
/// une fois la vue ouverte, toujours gates par confirmRiskAction comme avant.
/// Reutilise des ids deja geres par AssistantView.vue::runRecoveryAction
/// (open_expert/open_pointer_scan/trace_ui_string) plutot que des ids qui
/// exigent une adresse deja capturee (find_what_writes_targets,
/// escalate_freeze_bp...) -- cette reponse est generique par symptome, elle
/// n'a jamais d'adresse precise en contexte.
///
/// `expertStep` (optionnel, uniquement pour open_expert) : filtre la vue
/// Expert sur l'etape existante correspondante (find/inspect/act/persist,
/// ExpertView.vue::activeStep) au lieu de la laisser sur 'all' -- resout le
/// probleme concret constate le 29/08/2026 (clic sur "Ouvrir Expert pour
/// lancer Ecrit par" n'amenait qu'a la vue de depart, l'utilisateur devait
/// chercher/scroller). Consomme via un champ store dedie (pendingExpertStep),
/// jamais applique a une visite manuelle d'Expert -- voir commentaire
/// ExpertView.vue::activeStep sur le choix delibere de ne jamais forcer un
/// changement d'onglet en dehors d'un clic explicite comme celui-ci.
QVariantList recoveryActionsForTopic(const QString& topic) {
    QVariantList actions;
    if (topic == "simple_visible_value") {
        actions.append(QVariantMap{{"id", "open_expert"}, {"label", "Ouvrir Expert pour lancer le scan"}, {"expertStep", "find"}});
    } else if (topic == "displayed_value_not_found") {
        actions.append(QVariantMap{{"id", "trace_ui_string"}, {"label", "Lancer Trace UI string"}});
    } else if (topic == "unstable_address") {
        actions.append(QVariantMap{{"id", "open_pointer_scan"}, {"label", "Ouvrir Expert, section Pointeurs"}});
    } else if (topic == "freeze_flickers") {
        actions.append(QVariantMap{{"id", "open_expert"}, {"label", "Ouvrir Expert pour analyser la stabilité du champ"}, {"expertStep", "inspect"}});
    } else if (topic == "code_patch_request") {
        actions.append(QVariantMap{{"id", "open_expert"}, {"label", "Ouvrir Expert pour générer l'AOB"}, {"expertStep", "persist"}});
    } else if (topic == "what_writes_value") {
        actions.append(QVariantMap{
            {"id", "open_expert"}, {"label", "Ouvrir Expert pour lancer Écrit par"},
            {"expertStep", "find"}, {"expertAnchor", "expert-anchor-find-what-writes"}});
    } else if (topic == "save_file_or_uwp") {
        actions.append(QVariantMap{{"id", "open_expert"}, {"label", "Ouvrir Expert, section fichiers de sauvegarde"}, {"expertStep", "inspect"}});
    } else if (topic == "managed_runtime_pointer_chain") {
        actions.append(QVariantMap{{"id", "open_clr_inspector"}, {"label", "Ouvrir CLR Inspector"}});
    }
    return actions;
}

QVariantMap makeInvestigationPlaybookResponse(const QString& topic) {
    struct Entry {
        const char* title;
        const char* hypotheses;
        const char* tool;
        const char* prerequisites;
        const char* risk;
        const char* nextAction;
        const char* fallback;
    };

    Entry entry{
        "Enquête guidée",
        "Symptôme reconnu, mais il manque encore une cible actionnable.",
        "Consulter le playbook d'enquête.",
        "Décrire la valeur, l'adresse ou le contexte observé.",
        "Aucun : cette réponse est strictement lecture seule.",
        "Suivre le plan proposé avant toute action à risque.",
        "Donner une valeur, une adresse ou une observation plus précise."
    };

    if (topic == "simple_visible_value") {
        entry = {
            "Valeur numérique simple visible",
            "La valeur est peut-être stockée telle quelle en mémoire, sans obfuscation ni recalcul d'affichage.",
            "Commencer par exact_scan, puis réduire avec next_scan quand la valeur change.",
            "Processus attaché et valeur actuellement visible à l'écran.",
            "Aucun pour le scan : lecture seule. Seule une écriture ou un freeze ultérieur demandera confirmation.",
            "Donne la valeur affichée actuelle, puis fais-la changer pour réduire les candidats.",
            "Si le scan exact ne converge pas, passer au chemin valeur affichée introuvable / Trace UI string."
        };
    } else if (topic == "displayed_value_not_found") {
        entry = {
            "Valeur affichée introuvable",
            "La valeur peut être une string UI, une copie d'affichage, ou une représentation transformée.",
            "Utiliser Trace UI string, puis analyser les sources numériques autour des strings suivies.",
            "Valeur visible à l'écran sous forme de texte lisible.",
            "Aucun pour Trace UI string / analyse des sources : lecture seule.",
            "Confirme la valeur affichée exacte, puis observe son évolution avant toute écriture.",
            "Si aucune source fiable n'apparaît, escalader vers 'qui écrit cette valeur' avec confirmation debugger."
        };
    } else if (topic == "unstable_address") {
        entry = {
            "Adresse instable au redémarrage",
            "L'adresse absolue est probablement invalidée par l'ASLR ou par une réallocation d'objet.",
            "Transformer la trouvaille en locator Trainer : AOB pour du code, pointer chain pour une donnée.",
            "Adresse déjà validée comme correcte dans la session actuelle.",
            "Aucun pour générer/chercher un locator ; ne pas promettre de stabilité si seul absolute fonctionne.",
            "Stabilise l'adresse via AOB ou pointer chain avant d'en faire une feature Trainer durable.",
            "Si rien n'est unique/stable, garder absolute en indiquant clairement que ça ne survivra probablement pas."
        };
    } else if (topic == "freeze_flickers") {
        entry = {
            "Freeze qui clignote",
            "La cible peut réécrire plus vite que le polling, ou l'adresse peut être un champ affiché dérivé.",
            "Analyser la stabilité du champ avant d'envisager un freeze breakpoint matériel.",
            "Adresse candidate déjà identifiée.",
            "Analyse de stabilité : lecture seule. Freeze BP : debugger, confirmation obligatoire.",
            "Vérifie d'abord si l'adresse est une vraie source ou seulement un affichage recalculé.",
            "Si c'est un affichage dérivé, chercher l'origine de l'écriture plutôt que freezer cette copie."
        };
    } else if (topic == "code_patch_request") {
        entry = {
            "Patch de code demandé",
            "L'objectif touche probablement une instruction machine plutôt qu'une simple donnée.",
            "Si l'adresse vient d'un hit 'Écrit par' : désassembler en arrière D'ABORD (le RIP capturé pointe sur "
            "l'instruction suivante, pas l'écriture elle-même — sémantique standard d'un breakpoint matériel), "
            "PUIS générer une AOB sur la vraie instruction trouvée, puis suggérer un patch.",
            "Adresse de code valide. Si elle vient d'un hit 'Écrit par' : c'est le RIP capturé, pas encore l'adresse "
            "réelle de l'instruction à patcher — désassembler en arrière d'abord pour la retrouver.",
            "Élevé pour l'application réelle : patch=confirmation humaine, jamais auto-exécuté depuis le chat.",
            "Ne jamais générer d'AOB directement sur un RIP brut issu d'un hit 'Écrit par' : localise d'abord la "
            "vraie instruction d'écriture, vérifie l'unicité de la signature, puis applique seulement depuis "
            "Expert/Trainer après confirmation.",
            "Si la signature est ambiguë ou bloquée par l'environnement, revenir à un write/freeze moins invasif."
        };
    } else if (topic == "what_writes_value") {
        entry = {
            "Comprendre qui écrit une valeur",
            "Plusieurs sites de code peuvent écrire la même adresse ; il faut identifier la vraie source gameplay.",
            "Utiliser 'Écrit par' / find_what_writes depuis l'UI Expert, avec confirmation.",
            "Adresse stable déjà connue, pas un slot trop chaud ou générique. Si tu n'as pas encore d'adresse : "
            "fais d'abord un scan classique (donne-moi la valeur affichée à l'écran) pour en trouver une et la "
            "sélectionner comme candidat — 'Écrit par' ne peut rien capturer sans ça.",
            "Debugger : peut perturber la cible, confirmation obligatoire.",
            "Si tu as déjà une adresse : prépare-la, lance la capture confirmée, puis interagis avec le jeu pendant "
            "la fenêtre. Sinon : commence par le scan décrit ci-dessus, reviens ensuite avec l'adresse trouvée.",
            "Si aucun hit n'apparaît, élargir la fenêtre ou revérifier que l'adresse est bien stable."
        };
    } else if (topic == "save_file_or_uwp") {
        entry = {
            "Valeur dans sauvegarde ou LocalSettings",
            "La valeur peut vivre sur disque ou dans une ruche UWP plutôt qu'en RAM exploitable.",
            "Découvrir les fichiers de sauvegarde, lire le texte, inspecter LocalSettings, puis comparer avant/après.",
            "Jeu avec fichier de sauvegarde identifiable ou processus UWP attaché.",
            "Lecture seule pour inspection. Toute écriture disque nécessite une action explicite séparée.",
            "Compare un état avant/après une action utilisateur pour isoler le champ modifié.",
            "Si une source plus autoritaire réécrit le fichier, il faudra un protocole d'enquête plus large hors 120-A."
        };
    } else if (topic == "managed_runtime_pointer_chain") {
        entry = {
            "Cible sur runtime managé (.NET/Mono) — scan de pointeurs natif aveugle",
            "Le processus charge coreclr.dll/clrjit.dll (ou mono*.dll) : les données de gameplay vivent sur un tas géré "
            "par le GC, pas dans les sections .data/.bss d'un module PE natif. Un scanPointerChains, même borné serré, "
            "ne trouvera structurellement aucune chaîne depuis un module natif — signe distinctif : réponse rapide "
            "mais chainCount:0, quel que soit le module d'ancrage essayé.",
            "Basculer sur le CLR Inspector : attachClrInspector, puis chercher l'objet par type/valeur de champ "
            "(findClrObjectsByType/findClrObjectsByFieldValue) et descendre la hiérarchie des champs (readClrObject) "
            "jusqu'au champ primitif, plutôt que de deviner une adresse brute.",
            "Cible confirmée managée (getProcessModules montre coreclr.dll/clrjit.dll ou mono*.dll) ; CLR Inspector attaché au bon PID.",
            "Aucun pour attachClrInspector/findClrObjectsBy*/readClrObject : lecture seule. writeClrPrimitivePath demande une confirmation comme toute écriture classique.",
            "Ne pas répéter scanPointerChains avec des bornes toujours plus larges sur ce type de cible : un résultat "
            "vide et rapide est déjà le signal qu'il faut changer d'outil. Si le type de premier niveau n'a pas le "
            "champ attendu, chercher un mot-clé de domaine plus large (le studio range parfois la donnée sur un objet conteneur).",
            "Si aucun mot-clé de domaine ne donne de type candidat, élargir avec des synonymes techniques du genre de "
            "jeu concerné."
        };
    }

    QVariantMap result;
    result["status"] = "needs_clarification";
    result["actionStatus"] = "not_executed";
    result["message"] = QString(
        "D'après ma méthode d'enquête intégrée, je traiterais ça comme : %1\n\n"
        "Hypothèses : %2\n"
        "Outil conseillé : %3\n"
        "Prérequis : %4\n"
        "Risque : %5\n"
        "Prochaine action humaine : %6\n"
        "Fallback : %7\n\n"
        "Je n'exécute rien automatiquement depuis cette réponse : pas d'action lancée toute seule, pas de contournement de la confirmation.")
        .arg(QString::fromUtf8(entry.title),
             QString::fromUtf8(entry.hypotheses),
             QString::fromUtf8(entry.tool),
             QString::fromUtf8(entry.prerequisites),
             QString::fromUtf8(entry.risk),
             QString::fromUtf8(entry.nextAction),
             QString::fromUtf8(entry.fallback));
    result["investigationTopic"] = topic;
    result["source"] = "docs/INVESTIGATION_PLAYBOOK.md";
    result["state"] = "Idle";
    result["aiBackend"] = "deterministic_investigation_playbook";
    const QVariantList recoveryActions = recoveryActionsForTopic(topic);
    if (!recoveryActions.isEmpty()) {
        result["recoveryActions"] = recoveryActions;
    }
    return result;
}

struct TrainerToolMatch {
    QString tool;
    QVariantMap args;
    QString rationale;
};

TrainerToolMatch matchTrainerTool(const QString& query) {
    const QString q = query.toLower();
    if (!wantsTrainerQuery(query)) {
        return {};
    }

    const bool wantsApply = q.contains("apply") || q.contains("activer") || q.contains("active ")
        || q.contains("lance") || q.contains("enabled");
    const bool wantsRestore = q.contains("restore") || q.contains("restaur") || q.contains("désactiv")
        || q.contains("desactiv") || q.contains("coupe");
    const bool wantsDelete = q.contains("delete") || q.contains("remove") || q.contains("supprim")
        || q.contains("efface");
    const bool wantsCreate = q.contains("create") || q.contains("add ") || q.contains("ajoute")
        || q.contains("crée") || q.contains("cree") || q.contains("sauve") || q.contains("sauvegarde");
    const bool wantsList = q.contains("list") || q.contains("liste") || q.contains("affiche")
        || q.contains("show") || q.contains("voir") || q.contains("snapshot") || q.contains("status");

    if (wantsApply) {
        return {"trainer_apply_request",
            {{"id", firstDecimalOutsideHex(q)}, {"all", q.contains("all") || q.contains("tout")}},
            "Activation Trainer demandee: je prepare une confirmation UI, sans appeler directement le RiskGate."};
    }
    if (wantsRestore) {
        return {"trainer_restore_request",
            {{"id", firstDecimalOutsideHex(q)}, {"all", q.contains("all") || q.contains("tout")}},
            "Restauration Trainer demandee: je prepare une confirmation UI, sans appeler directement le RiskGate."};
    }
    if (wantsDelete) {
        return {"trainer_delete_feature",
            {{"id", firstDecimalOutsideHex(q)}},
            "Suppression d'une feature Trainer demandee."};
    }
    if (wantsCreate) {
        const QString address = firstHexAddressIn(query);
        const QString value = firstDecimalOutsideHex(query);
        QVariantMap args;
        args["action"] = "write";
        args["address"] = address;
        args["value"] = value;
        args["valueType"] = q.contains("float") ? QString("Float32") : QString("Int32");
        args["name"] = "Assistant Trainer write";
        return {"trainer_create_write", args,
            "Creation d'une feature Trainer write demandee depuis une adresse et une valeur explicites."};
    }
    if (wantsList) {
        return {"trainer_list_features", {},
            "Je liste les features Trainer locales via le pont UI lecture seule."};
    }

    return {"trainer_list_features", {},
        "Demande Trainer generale: je commence par lister l'etat actuel des features."};
}

struct FieldStabilityToolMatch {
    QString tool;
    QVariantMap args;
    QString rationale;
};

// PHASE 130 : reconnait une demande explicite de classification "champ
// affiche vs champ source" pour une adresse candidate, avant de la figer/
// patcher (core/scanner/display_source_classifier.*, expose via
// ApplicationController::analyzeFieldStability). Observation passive
// (findWhatWrites), aucune ecriture -- meme esprit que matchOffMemoryTool
// ci-dessus pour la liste de mots-cles.
FieldStabilityToolMatch matchFieldStabilityTool(const QString& query) {
    if (!wantsFieldStabilityQuery(query)) {
        return {};
    }

    const QString address = firstHexAddressIn(query);
    if (address.isEmpty()) {
        return {};
    }

    QVariantMap args;
    args["address"] = address;
    return {"analyze_field_stability", args,
        "J'observe passivement les écritures sur cette adresse (aucune écriture de ma part) pour juger si "
        "elle ressemble à un champ affiché recalculé ou à une source événementielle."};
}

struct AutoReportToolMatch {
    QString tool;
    QString rationale;
};

// PHASE 140 : demande explicite d'un resume/bilan de l'auto-resolution
// (getAutoResolveReport -- lecture seule, cout quasi nul, aucun argument
// requis). Mots-cles volontairement assez specifiques (pas juste "rapport"
// ou "resume" seuls) pour eviter de se declencher sur une phrase qui parle
// d'autre chose.
AutoReportToolMatch matchAutoReportTool(const QString& query) {
    const QString q = query.toLower();
    const bool wantsReport =
        q.contains("rapport auto") || q.contains("rapport d'auto")
        || q.contains("bilan auto-résolution") || q.contains("bilan auto-resolution")
        || q.contains("résumé auto-résolution") || q.contains("resume auto-resolution")
        || q.contains("stratégie recommandée") || q.contains("strategie recommandee")
        || q.contains("auto report") || q.contains("auto-resolve report")
        || q.contains("get auto report");
    if (!wantsReport) {
        return {};
    }
    return {"get_auto_report",
        "Je résume le contexte, la stratégie recommandée et les événements récents (lecture seule)."};
}

struct UiSourcesToolMatch {
    QString tool;
    QVariantMap args;
    QString rationale;
};

// PHASE 140 : suite naturelle de trace_ui_string (deja fast-pathe plus haut
// dans processQuery/deterministicPlanWithContext) -- demande explicite
// d'analyser les sources numeriques autour d'une string UI deja localisee.
// Ne fournit que "value" (la valeur affichee actuelle) : le dispatch cote
// ApplicationController::startSmartSearch reutilise m_pendingUiStringCandidates
// (meme etat que trace_ui_string/AnswerTraceUiFilterPrompt) si aucune adresse
// explicite n'est donnee, plutot que d'exiger une adresse dans la phrase --
// une seule phrase NL fournit rarement une adresse ET une byteLength.
UiSourcesToolMatch matchUiSourcesTool(const QString& query) {
    if (!wantsUiSourcesQuery(query)) {
        return {};
    }

    QVariantMap args;
    const QString value = firstDecimalOutsideHex(query);
    if (!value.isEmpty()) {
        args["value"] = value;
    }
    const QString address = firstHexAddressIn(query);
    if (!address.isEmpty()) {
        args["address"] = address;
    }
    return {"analyze_ui_sources", args,
        "Je cherche les sources numériques probables près de la dernière string UI localisée (lecture seule)."};
}

// PHASE 140 : generate_aob/suggest_patch/disassemble_backward -- les 3 restants
// du "workflow patch" reclasses lecture seule (tool_registry.cpp). Meme
// mecanique que matchFieldStabilityTool : mot-cle + extraction d'adresse.
AddressToolMatch matchGenerateAobTool(const QString& query) {
    if (!wantsGenerateAobQuery(query)) {
        return {};
    }
    QVariantMap args;
    const QString address = firstHexAddressIn(query);
    if (!address.isEmpty()) {
        args["address"] = address;
    }
    return {"generate_aob", args, "Je génère une signature AOB pour cette instruction (lecture seule)."};
}

AddressToolMatch matchSuggestPatchTool(const QString& query) {
    if (!wantsSuggestPatchQuery(query)) {
        return {};
    }
    QVariantMap args;
    const QString address = firstHexAddressIn(query);
    if (!address.isEmpty()) {
        args["address"] = address;
    }
    return {"suggest_patch", args,
        "Je suggère des patchs de code possibles pour cette instruction, sans en appliquer aucun."};
}

AddressToolMatch matchDisassembleBackwardTool(const QString& query) {
    if (!wantsDisassembleBackwardQuery(query)) {
        return {};
    }
    QVariantMap args;
    const QString address = firstHexAddressIn(query);
    if (!address.isEmpty()) {
        args["address"] = address;
    }
    return {"disassemble_backward", args,
        "Je désassemble en arrière depuis cette instruction pour repérer les champs sources candidats (lecture seule)."};
}

// PHASE 140 : find_what_writes/test_candidate_fields -- contrairement aux 3
// ci-dessus, le dispatch cote ApplicationController::startSmartSearch ne les
// execute JAMAIS directement (attache un debugger / ecrit une valeur test,
// voir application_controller.cpp) : il redirige systematiquement vers l'UI
// quels que soient les args. Le matching ici sert seulement a router vers le
// bon message de redirection -- pas besoin d'une extraction d'adresse fiable.
AddressToolMatch matchFindWhatWritesTool(const QString& query) {
    if (!wantsFindWhatWritesQuery(query)) {
        return {};
    }
    QVariantMap args;
    const QString address = firstHexAddressIn(query);
    if (!address.isEmpty()) {
        args["address"] = address;
    }
    return {"find_what_writes", args, ""};
}

AddressToolMatch matchTestCandidateFieldsTool(const QString& query) {
    if (!wantsTestCandidateFieldsQuery(query)) {
        return {};
    }
    return {"test_candidate_fields", {}, ""};
}

} // namespace

AIEngine::AIEngine(QObject* parent) : QObject(parent) {}
AIEngine::~AIEngine() {}

bool AIEngine::init() {
    // Keep startup non-blocking and crash-proof: embedded AI runtime is
    // initialized lazily by the first AI flow, not during application boot.
    KE_LOG_INFO() << "AIEngine::init() - embedded AI init deferred";
    m_ready = true;
    return true;
}

bool AIEngine::isReady() const { return m_ready; }

void AIEngine::noteOutcome(const QString& query, const QString& tool, const QString& outcome) {
    QVariantMap turn;
    turn["query"] = query;
    turn["tool"] = tool;
    turn["outcome"] = outcome;
    m_history.append(turn);
    while (m_history.size() > kMaxHistoryTurns) {
        m_history.removeFirst();
    }
}

void AIEngine::clearHistory() {
    m_history.clear();
}

QVariantMap AIEngine::lastHistoryTurn() const {
    return m_history.isEmpty() ? QVariantMap{} : m_history.last().toMap();
}

bool AIEngine::ensureLlamaInitialized() {
    if (qEnvironmentVariable("KILLENGINE_DISABLE_LLAMA") == "1") {
        return false;
    }
    const bool runningUnitTests = QCoreApplication::applicationFilePath().contains("killengine_unit_tests", Qt::CaseInsensitive);
    if (runningUnitTests && QProcessEnvironment::systemEnvironment().value("KILLENGINE_ENABLE_LLAMA_IN_TESTS") != "1") {
        return false;
    }
    if (!QSettings().value("ai/modelEnabled", true).toBool()) {
        return false;
    }
    if (m_llama.isAvailable()) {
        return true;
    }
    const bool ok = m_llama.init();
    if (ok) {
        KE_LOG_INFO() << "AIEngine llama.cpp runtime available.";
    } else if (!m_llama.info().errorMessage.isEmpty()) {
        KE_LOG_INFO() << "AIEngine llama.cpp unavailable: " << m_llama.info().errorMessage.toStdString();
    }
    return ok;
}

QVariantMap AIEngine::modelToolCallWithRetry(const QString& query, const QVariantMap& context, QString* backend) {
    auto generated = m_llama.planToolCall(query, m_registry, context);
    QString error;
    QVariantMap call = generated.success ? LlamaRuntime::extractToolCallJson(generated.output, &error) : QVariantMap{};

    // Retry correctif borne: une seconde tentative si le modele a repondu
    // mais sans JSON exploitable (hallucination de format, bavardage...).
    if (generated.success && call.isEmpty()) {
        KE_LOG_INFO() << "AIEngine retrying model tool call after invalid JSON: " << error.toStdString();
        const QString correctiveQuery = query + "\n(Rappel: reponds UNIQUEMENT par l'objet JSON du schema, sans texte autour.)";
        generated = m_llama.planToolCall(correctiveQuery, m_registry, context);
        if (generated.success) {
            call = LlamaRuntime::extractToolCallJson(generated.output, &error);
        }
    }

    if (backend) *backend = generated.backend;
    if (!generated.success) {
        if (backend) backend->append(QString("|error:%1").arg(generated.errorMessage));
        return {};
    }
    return call;
}

QVariantMap AIEngine::modelIntentWithRetry(const QString& query, QString* backend) {
    auto generated = m_llama.planIntent(query);
    QString error;
    QVariantMap intent = generated.success ? LlamaRuntime::extractIntentJson(generated.output, &error) : QVariantMap{};

    if (generated.success && intent.isEmpty()) {
        KE_LOG_INFO() << "AIEngine retrying model intent after invalid JSON: " << error.toStdString();
        const QString correctiveQuery = query + "\n(Rappel: reponds UNIQUEMENT par l'objet JSON du schema, sans texte autour.)";
        generated = m_llama.planIntent(correctiveQuery);
        if (generated.success) {
            intent = LlamaRuntime::extractIntentJson(generated.output, &error);
        }
    }

    if (backend) *backend = generated.backend;
    if (!generated.success) {
        if (backend) backend->append(QString("|error:%1").arg(generated.errorMessage));
        return {};
    }
    return intent;
}

QVariantMap AIEngine::processIntent(const QString& query) {
    if (!m_ready) {
        QVariantMap result;
        result["status"] = "not_ready";
        result["message"] = "AIEngine is not initialized.";
        return result;
    }

    const QString q = query.toLower();
    QStringList detectedNumbers;
    const QRegularExpression guardedNumberRe(R"([-+]?\d+(?:[\.,]\d+)?)");
    auto guardedNumberIt = guardedNumberRe.globalMatch(query);
    while (guardedNumberIt.hasNext()) {
        detectedNumbers.append(guardedNumberIt.next().captured(0).replace(',', '.'));
    }
    if (looksLikeBadTargets(q)) {
        QVariantMap result;
        result["status"] = "intent";
        result["intent"] = "ReportBadTargets";
        result["value"] = "";
        result["targetValue"] = "";
        result["addresses"] = QVariantList{};
        result["confidence"] = 0.9;
        result["missing"] = "";
        result["aiBackend"] = "deterministic_guard";
        return result;
    }

    const QString firstDetectedNumber = firstNumber(query);
    const bool looksLikeWriteWithoutValue =
        (q.contains("passe") || q.contains("passer") || q.contains("mets") || q.contains("met ")
         || q.contains("veux") || q.contains("voudrais") || q.contains("augmente") || q.contains("remplace")
         || q.contains("write") || q.contains("écri") || q.contains("ecri"))
        && firstDetectedNumber.isEmpty();
    if (looksLikeWriteWithoutValue) {
        QVariantMap result;
        result["status"] = "needs_clarification";
        result["intent"] = "Unknown";
        result["value"] = "";
        result["targetValue"] = "";
        result["addresses"] = QVariantList{};
        result["confidence"] = 0.9;
        result["missing"] = "Tu veux le passer à quelle valeur ?";
        result["message"] = result["missing"];
        result["aiBackend"] = "deterministic_guard";
        return result;
    }

    const bool looksLikeRewriteWithValue =
        !firstDetectedNumber.isEmpty()
        && (q.contains("passe") || q.contains("passer") || q.contains("mets") || q.contains("met ")
            || q.contains("veux") || q.contains("voudrais") || q.contains("augmente") || q.contains("remplace"))
        && (q.contains(" le ") || q.contains(" les ") || q.contains("ça") || q.contains("ca")
            || q.contains("adresse") || q.contains("derni"));
    if (looksLikeRewriteWithValue && detectedNumbers.size() == 1) {
        QVariantMap result;
        result["status"] = "intent";
        result["intent"] = "RewriteLastTargets";
        result["value"] = firstDetectedNumber;
        result["targetValue"] = "";
        result["addresses"] = QVariantList{};
        result["confidence"] = 0.9;
        result["missing"] = "";
        result["aiBackend"] = "deterministic_guard";
        return result;
    }

    if (ensureLlamaInitialized()) {
        QString backend;
        const QVariantMap intent = modelIntentWithRetry(query, &backend);
        if (!intent.isEmpty()) {
            QString error;
            const bool valid = IntentContract::validate(intent, &error);
            intent["status"] = valid ? "intent" : "needs_clarification";
            intent["aiBackend"] = backend.isEmpty() ? QString("llama.cpp") : backend;
            intent["error"] = error;
            if (!valid && intent.value("message").toString().isEmpty()) {
                intent["message"] = intent.value("missing").toString().isEmpty()
                    ? QString("Je dois préciser l'intention avant d'agir.")
                    : intent.value("missing").toString();
            }
            return intent;
        }
        KE_LOG_INFO() << "AIEngine model intent rejected: " << backend.toStdString();
    }

    QVariantMap fallback = deterministicIntent(query);
    fallback["aiBackend"] = "deterministic";
    if (m_llama.info().available == false && !m_llama.info().errorMessage.isEmpty()) {
        fallback["aiBackendNote"] = m_llama.info().errorMessage;
    }
    return fallback;
}

QVariantMap AIEngine::processQuery(const QString& query) {
    return processQuery(query, {});
}

QVariantMap AIEngine::proposeInvestigationNotebookPlan(const QString& symptom, const QVariantMap& context) {
    const QString trimmed = symptom.trimmed();
    if (!m_ready) {
        QVariantMap result;
        result["success"] = false;
        result["error"] = "AIEngine is not initialized.";
        result["source"] = "not_ready";
        result["modelUsed"] = false;
        return result;
    }
    if (trimmed.isEmpty()) {
        QVariantMap result;
        result["success"] = false;
        result["error"] = "Symptome vide.";
        result["source"] = "validation";
        result["modelUsed"] = false;
        return result;
    }

    if (context.value("useModel", true).toBool() && ensureLlamaInitialized()) {
        auto generated = m_llama.planInvestigationNotebook(trimmed, context);
        QString error;
        QVariantMap parsed = generated.success ? extractInvestigationNotebookPlanJson(generated.output, &error) : QVariantMap{};

        if (generated.success && parsed.isEmpty()) {
            KE_LOG_INFO() << "AIEngine retrying investigation notebook plan after invalid JSON: " << error.toStdString();
            QVariantMap retryContext = context;
            retryContext["formatReminder"] = "Réponds uniquement par l'objet JSON du schéma, sans score numérique.";
            generated = m_llama.planInvestigationNotebook(trimmed, retryContext);
            if (generated.success) {
                parsed = extractInvestigationNotebookPlanJson(generated.output, &error);
            }
        }

        if (!parsed.isEmpty()) {
            QVariantMap plan = normalizeInvestigationNotebookPlan(
                parsed,
                trimmed,
                generated.backend.isEmpty() ? QString("llama.cpp") : generated.backend);
            plan["aiBackend"] = plan.value("source");
            return plan;
        }

        KE_LOG_INFO() << "AIEngine model investigation plan rejected: "
                      << (generated.success ? error : generated.errorMessage).toStdString();
    }

    QVariantMap fallback = makeFallbackInvestigationNotebookPlan(trimmed);
    if (m_llama.info().available == false && !m_llama.info().errorMessage.isEmpty()) {
        fallback["aiBackendNote"] = m_llama.info().errorMessage;
    }
    fallback["aiBackend"] = fallback.value("source");
    return fallback;
}

QVariantMap AIEngine::processQuery(const QString& query, const QVariantMap& context) {
    const QString q = query.toLower();

    if (!m_ready) {
        QVariantMap result;
        result["status"] = "not_ready";
        result["message"] = "AIEngine is not initialized.";
        return result;
    }

    // PHASE 99 : fast-path pour une demande explicite d'investigation "hors
    // memoire" -- ces requetes n'ont pas besoin d'une inference LLM et
    // n'avaient jusqu'ici droit a l'outil deterministe qu'APRES un aller-retour
    // au modele local (potentiellement plusieurs dizaines de secondes) qui
    // finissait de toute facon par echouer/retomber sur ce meme outil via
    // deterministicPlanWithContext. Court-circuite ce retard, avant meme
    // ensureLlamaInitialized(). Ne s'applique que process attache (sinon on
    // laisse le chemin normal produire le message de clarification usuel).
    if (looksLikePureSocialQuery(query)) {
        QVariantMap result;
        result["status"] = "needs_clarification";
        result["message"] =
            "Salut ! Dis-moi ce que tu veux chercher ou comprendre : une valeur affichée, une adresse, "
            "un freeze, un trainer, un script Lua, ou une investigation plus guidée.";
        result["state"] = m_stateMachine.currentStateName();
        result["aiBackend"] = "deterministic_social_guard";
        return result;
    }

    if (const QString topic = investigationPlaybookTopic(query); !topic.isEmpty()) {
        // PHASE 120-C (29/08/2026, accord propriétaire explicite) : sur un
        // sous-ensemble prudent de sujets où l'outil recommandé est déjà un
        // tool_call lecture seule approuvé SANS confirmation
        // (analyze_field_stability/discover_save_files, voir leurs
        // commentaires PHASE 130/140), enchaîner directement si le fast-path
        // dédié trouve aussi assez d'info dans le même message -- sinon (pas
        // d'adresse, mots-clés insuffisants) revenir à la simple citation
        // comme avant. "code_patch_request" délibérément absent de cette
        // liste : son propre matcher de sujet exige `!directReadOnlyTool`,
        // qui inclut `wantsGenerateAobQuery` -- structurellement, ce sujet ne
        // matche JAMAIS en même temps que le fast-path generate_aob
        // correspondant, un enchaînement ici serait du code mort par
        // construction (constaté en écrivant ce correctif). Explicitement
        // exclu de tout enchaînement, sans exception : "what_writes_value"
        // (attache un debugger, jamais autonome, PHASE 140) et tous les
        // autres sujets (pas d'argument fiable extractible sans élargir la
        // surface d'outils de l'Assistant au-delà de son registre actuel,
        // docs/KILLENGINE_ASSISTANT_TOOLS_MAP.md).
        if (topic == "freeze_flickers") {
            if (const auto stabilityMatch = matchFieldStabilityTool(query); !stabilityMatch.tool.isEmpty()) {
                QVariantMap result = makeToolCall(stabilityMatch.tool, stabilityMatch.args,
                    "D'après le playbook d'enquête (freeze qui clignote) : " + stabilityMatch.rationale);
                result["aiBackend"] = "deterministic_investigation_playbook_autochain";
                result["investigationTopic"] = topic;
                return result;
            }
        } else if (topic == "save_file_or_uwp") {
            if (const auto offMemoryMatch = matchOffMemoryTool(q); !offMemoryMatch.tool.isEmpty()) {
                QVariantMap result = makeToolCall(offMemoryMatch.tool, {},
                    "D'après le playbook d'enquête (valeur en sauvegarde/UWP) : " + offMemoryMatch.rationale);
                result["aiBackend"] = "deterministic_investigation_playbook_autochain";
                result["investigationTopic"] = topic;
                return result;
            }
        }

        QVariantMap result = makeInvestigationPlaybookResponse(topic);
        result["state"] = m_stateMachine.currentStateName();
        return result;
    }

    if (context.value("processAttached", true).toBool()) {
        if (const auto moduleScanMatch = matchModuleExactScanTool(query); !moduleScanMatch.tool.isEmpty()) {
            QVariantMap result = makeToolCall(moduleScanMatch.tool, moduleScanMatch.args, moduleScanMatch.rationale);
            result["aiBackend"] = "deterministic_module_scan_fastpath";
            return result;
        }
        if (context.value("scanActive", false).toBool() && wantsModuleSourcePivot(q)) {
            QVariantMap result = makeToolCall("list_process_modules", {},
                "Je liste les modules/DLL charges pour identifier le module applicatif avant de poursuivre vers AOB/desassemblage ou Trace UI string/Changed Pages.");
            result["aiBackend"] = "deterministic_module_listing_fastpath";
            return result;
        }
        if (wantsProcessModuleListing(q)) {
            QVariantMap result = makeToolCall("list_process_modules", {},
                "Je liste les modules/DLL charges par le processus attache (lecture seule).");
            result["aiBackend"] = "deterministic_module_listing_fastpath";
            return result;
        }
        if (const auto trainerMatch = matchTrainerTool(query); !trainerMatch.tool.isEmpty()) {
            QVariantMap result = makeToolCall(trainerMatch.tool, trainerMatch.args, trainerMatch.rationale);
            result["aiBackend"] = "deterministic_trainer_fastpath";
            return result;
        }
        if (const auto stabilityMatch = matchFieldStabilityTool(query); !stabilityMatch.tool.isEmpty()) {
            QVariantMap result = makeToolCall(stabilityMatch.tool, stabilityMatch.args, stabilityMatch.rationale);
            result["aiBackend"] = "deterministic_field_stability_fastpath";
            return result;
        }
        if (const auto autoReportMatch = matchAutoReportTool(query); !autoReportMatch.tool.isEmpty()) {
            QVariantMap result = makeToolCall(autoReportMatch.tool, {}, autoReportMatch.rationale);
            result["aiBackend"] = "deterministic_auto_report_fastpath";
            return result;
        }
        if (const auto uiSourcesMatch = matchUiSourcesTool(query); !uiSourcesMatch.tool.isEmpty()) {
            QVariantMap result = makeToolCall(uiSourcesMatch.tool, uiSourcesMatch.args, uiSourcesMatch.rationale);
            result["aiBackend"] = "deterministic_ui_sources_fastpath";
            return result;
        }
        if (const auto aobMatch = matchGenerateAobTool(query); !aobMatch.tool.isEmpty()) {
            QVariantMap result = makeToolCall(aobMatch.tool, aobMatch.args, aobMatch.rationale);
            result["aiBackend"] = "deterministic_generate_aob_fastpath";
            return result;
        }
        if (const auto suggestMatch = matchSuggestPatchTool(query); !suggestMatch.tool.isEmpty()) {
            QVariantMap result = makeToolCall(suggestMatch.tool, suggestMatch.args, suggestMatch.rationale);
            result["aiBackend"] = "deterministic_suggest_patch_fastpath";
            return result;
        }
        if (const auto backwardMatch = matchDisassembleBackwardTool(query); !backwardMatch.tool.isEmpty()) {
            QVariantMap result = makeToolCall(backwardMatch.tool, backwardMatch.args, backwardMatch.rationale);
            result["aiBackend"] = "deterministic_disassemble_backward_fastpath";
            return result;
        }
        if (const auto findWritesMatch = matchFindWhatWritesTool(query); !findWritesMatch.tool.isEmpty()) {
            QVariantMap result = makeToolCall(findWritesMatch.tool, findWritesMatch.args, findWritesMatch.rationale);
            result["aiBackend"] = "deterministic_find_what_writes_fastpath";
            return result;
        }
        if (const auto testFieldsMatch = matchTestCandidateFieldsTool(query); !testFieldsMatch.tool.isEmpty()) {
            QVariantMap result = makeToolCall(testFieldsMatch.tool, testFieldsMatch.args, testFieldsMatch.rationale);
            result["aiBackend"] = "deterministic_test_candidate_fields_fastpath";
            return result;
        }
        if (const auto offMemoryMatch = matchOffMemoryTool(q); !offMemoryMatch.tool.isEmpty()) {
            QVariantMap result = makeToolCall(offMemoryMatch.tool, {}, offMemoryMatch.rationale);
            result["aiBackend"] = "deterministic_offmemory_fastpath";
            return result;
        }
    }

    if (ensureLlamaInitialized()) {
        // L'historique conversationnel enrichit le contexte transmis au modele:
        // il sait ce qui a deja echoue et peut proposer une vraie alternative.
        QVariantMap modelContext = context;
        if (!m_history.isEmpty()) {
            modelContext["history"] = m_history;
        }
        QString backend;
        const QVariantMap call = modelToolCallWithRetry(query, modelContext, &backend);
        if (!call.isEmpty()) {
            QString error;
            QVariantMap result;
            result["status"] = m_validator.validate(call, &error) ? "tool_call" : "invalid_tool_call";
            result["tool"] = call.value("tool").toString();
            result["args"] = call.value("args").toMap();
            result["rationale"] = "Plan généré par le modèle local llama.cpp/Qwen.";
            result["state"] = m_stateMachine.currentStateName();
            result["aiBackend"] = backend.isEmpty() ? QString("llama.cpp") : backend;
            result["error"] = error;
            if (result.value("status").toString() == "tool_call") {
                return result;
            }
            KE_LOG_INFO() << "AIEngine model tool call rejected: " << error.toStdString();
        } else {
            KE_LOG_INFO() << "AIEngine model produced no tool call: " << backend.toStdString();
        }
    }

    auto fallback = deterministicPlanWithContext(query, context);
    fallback["aiBackend"] = "deterministic";
    if (m_llama.info().available == false && !m_llama.info().errorMessage.isEmpty()) {
        fallback["aiBackendNote"] = m_llama.info().errorMessage;
    }
    return fallback;
}

QVariantMap AIEngine::deterministicIntent(const QString& query) {
    const QString q = query.toLower();
    QVariantMap result;
    result["status"] = "intent";
    result["intent"] = "Unknown";
    result["value"] = "";
    result["targetValue"] = "";
    result["addresses"] = QVariantList{};
    result["confidence"] = 0.5;
    result["missing"] = "";

    QVariantList addresses;
    const QRegularExpression addressRe(R"(0x[0-9a-fA-F]{5,16})");
    auto addressIt = addressRe.globalMatch(query);
    while (addressIt.hasNext()) {
        addresses.append(addressIt.next().captured(0));
    }

    QStringList numbers;
    const QRegularExpression numberRe(R"([-+]?\d+(?:[\.,]\d+)?)");
    auto numberIt = numberRe.globalMatch(query);
    while (numberIt.hasNext()) {
        numbers.append(numberIt.next().captured(0).replace(',', '.'));
    }

    if (looksLikeBadTargets(q)) {
        result["intent"] = "ReportBadTargets";
        result["confidence"] = 0.8;
    } else if (q.contains("autre") || q.contains("nouveau") || q.contains("reset") || q.contains("recommence")) {
        result["intent"] = numbers.isEmpty() ? "ResetContext" : "ExactScan";
        if (!numbers.isEmpty()) result["value"] = numbers.first();
        result["confidence"] = 0.75;
    } else if (!addresses.isEmpty()) {
        result["intent"] = numbers.isEmpty() ? "ActivateMemoryTargets" : "WriteMemoryTargets";
        result["addresses"] = addresses;
        if (!numbers.isEmpty()) result["value"] = numbers.first();
        result["confidence"] = 0.9;
    } else if (numbers.size() >= 2) {
        result["intent"] = "GuidedScan";
        result["value"] = numbers.at(0);
        result["targetValue"] = numbers.at(1);
        result["confidence"] = 0.85;
    } else if (numbers.size() == 1) {
        if (q.contains("passe") || q.contains("passer") || q.contains("mets") || q.contains("met ")
            || q.contains("veux") || q.contains("voudrais") || q.contains("augmente") || q.contains("remplace")) {
            result["intent"] = "RewriteLastTargets";
        } else {
            result["intent"] = "ExactScan";
        }
        result["value"] = numbers.first();
        result["confidence"] = 0.7;
    } else if (q.contains("passe") || q.contains("passer") || q.contains("mets") || q.contains("met ")
               || q.contains("veux") || q.contains("voudrais") || q.contains("augmente") || q.contains("remplace")) {
        result["status"] = "needs_clarification";
        result["missing"] = "Tu veux le passer à quelle valeur ?";
        result["message"] = result["missing"];
    } else {
        result["status"] = "needs_clarification";
        result["missing"] = "Quelle valeur veux-tu chercher ?";
        result["message"] = result["missing"];
    }

    QString error;
    if (result.value("status").toString() == "intent" && !IntentContract::validate(result, &error)) {
        result["status"] = "needs_clarification";
        result["error"] = error;
        result["message"] = result.value("missing").toString().isEmpty()
            ? QString("Il manque une information pour continuer.")
            : result.value("missing").toString();
    }
    return result;
}

QVariantMap AIEngine::deterministicPlan(const QString& query) {
    const QString q = query.toLower();

    if (q.contains("unknown") || q.contains("inconnue")) {
        if (q.contains("capture") || q.contains("initial")) {
            m_stateMachine.setState(AIState::WaitingForUserChange);
            return makeToolCall("unknown_capture", {}, "Capture initiale pour valeur inconnue.");
        }

        QString mode = "changed";
        if (q.contains("augment") || q.contains("increased")) mode = "increased";
        if (q.contains("diminu") || q.contains("decreased")) mode = "decreased";
        if (q.contains("pareil") || q.contains("unchanged")) mode = "unchanged";
        return makeToolCall("unknown_compare", {{"mode", mode}, {"valueType", inferValueType(query)}}, "Comparaison unknown initial value.");
    }

    if (q.contains("next") || q.contains("changed") || q.contains("change") ||
        q.contains("increased") || q.contains("augment") ||
        q.contains("decreased") || q.contains("diminu") ||
        q.contains("inchang")) {
        QString mode = "changed";
        if (q.contains("exact")) mode = "exact";
        if (q.contains("increased") || q.contains("augment")) mode = "increased";
        if (q.contains("decreased") || q.contains("diminu")) mode = "decreased";
        if (q.contains("unchanged") || q.contains("inchang")) mode = "unchanged";
        m_stateMachine.setState(AIState::Refining);
        return makeToolCall("next_scan", {{"mode", mode}, {"value", firstNumber(query)}}, "Réduction des candidats.");
    }

    if ((q.contains("freeze") || q.contains("geler")) && !hasNegatedFreezeInstruction(q)) {
        return makeToolCall("freeze_value", {
            {"address", firstHexAddress(query)},
            {"valueType", inferValueType(query)},
            {"value", firstNumber(query)},
            {"enabled", true},
        }, "Freeze demandé par l'utilisateur.");
    }

    // Demande explicite du kernel avant le check d'ecriture generique
    // ci-dessous, sinon "ecris X via le kernel" tombe dans write_value et le
    // choix explicite de l'utilisateur est perdu.
    const bool wantsKernel = q.contains("kernel") || q.contains("noyau");
    if (wantsKernel && (q.contains("write") || q.contains("écri") || q.contains("mettre"))) {
        return makeToolCall("kernel_write", {
            {"address", firstHexAddress(query)},
            {"valueType", inferValueType(query)},
            {"value", firstNumber(query)},
        }, "Écriture kernel demandée explicitement par l'utilisateur.");
    }

    // Speedhack : placé avant les checks génériques ci-dessous pour la même
    // raison que wantsKernel plus haut — "ralentis le jeu" ne doit jamais
    // tomber dans exact_scan juste parce qu'aucun nombre n'est fourni.
    {
        const bool wantsSpeedOff = q.contains("désactiv") || q.contains("desactiv") ||
            ((q.contains("stop") || q.contains("arrêt") || q.contains("normal")) &&
             (q.contains("vitesse") || q.contains("speed") || q.contains("temps") || q.contains("speedhack")));
        const bool wantsPause = q.contains("pause") &&
            (q.contains("temps") || q.contains("vitesse") || q.contains("speedhack") || q.contains("jeu") || q.contains("game"));
        const bool wantsSlow = q.contains("ralent") || q.contains("slow");
        const bool wantsFast = q.contains("accélér") || q.contains("acceler") || q.contains("speed up") || q.contains("speedup");
        const bool wantsSpeedGeneric = q.contains("vitesse") || q.contains("speed");
        if (wantsSpeedOff) {
            return makeToolCall("speedhack_set", {{"mode", "off"}}, "Désactivation du speedhack demandée.");
        }
        if (wantsPause) {
            return makeToolCall("speedhack_set", {{"mode", "set"}, {"factor", 0.0}}, "Pause du temps demandée (speedhack).");
        }
        if (wantsSlow || wantsFast || wantsSpeedGeneric) {
            double factor = wantsSlow ? 0.5 : 2.0;
            bool parsedOk = false;
            const double parsed = firstNumber(query).toDouble(&parsedOk);
            if (parsedOk && parsed > 0.0) factor = parsed;
            return makeToolCall("speedhack_set", {{"mode", "set"}, {"factor", factor}},
                wantsSlow ? "Ralentissement demandé (speedhack)." : "Accélération demandée (speedhack).");
        }
    }

    // PHASE 148 : block_process_network -- meme raison que wantsKernel plus
    // haut (n'utiliser que sur demande explicite, jamais choisi seul par le
    // modele local -- absent de la ligne "Schema obligatoire" de
    // llama_runtime.cpp, voir sa justification). Place avant les checks
    // generiques ci-dessous pour la meme raison.
    {
        const bool wantsNetworkOff = q.contains("rétablis") || q.contains("retablis")
            || q.contains("restore network") || q.contains("unblock network")
            || ((q.contains("réseau") || q.contains("reseau") || q.contains("network"))
                && (q.contains("rétabli") || q.contains("retabli") || q.contains("remets")));
        const bool wantsNetworkOn = !wantsNetworkOff
            && (q.contains("coupe le réseau") || q.contains("coupe le reseau")
                || q.contains("bloque le réseau") || q.contains("bloque le reseau")
                || q.contains("isole le réseau") || q.contains("isole le reseau")
                || q.contains("block network") || q.contains("cut network")
                || q.contains("block the network"));
        if (wantsNetworkOff) {
            return makeToolCall("block_process_network", {{"mode", "off"}}, "Rétablissement du réseau demandé.");
        }
        if (wantsNetworkOn) {
            return makeToolCall("block_process_network", {{"mode", "on"}}, "Coupure réseau demandée par l'utilisateur.");
        }
    }

    if (q.contains("write") || q.contains("écri") || q.contains("mettre")) {
        return makeToolCall("write_value", {
            {"address", firstHexAddress(query)},
            {"valueType", inferValueType(query)},
            {"value", firstNumber(query)},
        }, "Écriture mémoire demandée.");
    }

    const QString value = firstNumber(query);
    if (!value.isEmpty()) {
        m_stateMachine.setState(AIState::FirstScanRunning);
        return makeToolCall("exact_scan", {{"value", value}, {"valueType", inferValueType(query)}}, "Premier scan exact depuis une valeur détectée.");
    }

    QVariantMap result;
    result["status"] = "needs_clarification";
    result["message"] = "Je n'ai pas trouvé de valeur ou d'action claire.";
    result["state"] = m_stateMachine.currentStateName();
    result["availableTools"] = m_registry.availableTools();
    return result;
}

QVariantMap AIEngine::deterministicPlanWithContext(const QString& query, const QVariantMap& context) {
    const QString q = query.toLower();
    const bool processAttached = context.value("processAttached", true).toBool();
    const bool scanActive = context.value("scanActive").toBool();
    const bool unknownSnapshotActive = context.value("unknownSnapshotActive", false).toBool();
    const auto candidateCount = context.value("candidateCount").toULongLong();
    const QString contextTargetValue = context.value("targetValue").toString();
    const QString contextInitialValue = context.value("initialValue").toString();
    const QString value = firstNumber(query);
    const QStringList numbers = allNumbers(query);
    const bool describesVariation = describesIncrease(q) || describesDecrease(q) || describesChange(q) || describesStable(q);
    const bool inspectorOrUiCopy = wantsInspectorMode(q) || describesUiCopyOrBuffer(q);

    // Garde-fou : sans processus attache, aucun scan n'a de sens.
    if (!processAttached) {
        QVariantMap result;
        result["status"] = "needs_clarification";
        result["message"] = "Attache d'abord un processus dans l'onglet Processus, puis relance ta recherche.";
        result["state"] = m_stateMachine.currentStateName();
        return result;
    }

    // PHASE 91/99 : demande explicite d'investigation "hors memoire" (voir
    // matchOffMemoryTool ci-dessus pour la liste de mots-cles).
    if (const auto trainerMatch = matchTrainerTool(query); !trainerMatch.tool.isEmpty()) {
        return makeToolCall(trainerMatch.tool, trainerMatch.args, trainerMatch.rationale);
    }
    if (const auto stabilityMatch = matchFieldStabilityTool(query); !stabilityMatch.tool.isEmpty()) {
        return makeToolCall(stabilityMatch.tool, stabilityMatch.args, stabilityMatch.rationale);
    }
    if (const auto moduleScanMatch = matchModuleExactScanTool(query); !moduleScanMatch.tool.isEmpty()) {
        return makeToolCall(moduleScanMatch.tool, moduleScanMatch.args, moduleScanMatch.rationale);
    }
    if (const auto autoReportMatch = matchAutoReportTool(query); !autoReportMatch.tool.isEmpty()) {
        return makeToolCall(autoReportMatch.tool, {}, autoReportMatch.rationale);
    }
    if (const auto uiSourcesMatch = matchUiSourcesTool(query); !uiSourcesMatch.tool.isEmpty()) {
        return makeToolCall(uiSourcesMatch.tool, uiSourcesMatch.args, uiSourcesMatch.rationale);
    }
    if (const auto aobMatch = matchGenerateAobTool(query); !aobMatch.tool.isEmpty()) {
        return makeToolCall(aobMatch.tool, aobMatch.args, aobMatch.rationale);
    }
    if (const auto suggestMatch = matchSuggestPatchTool(query); !suggestMatch.tool.isEmpty()) {
        return makeToolCall(suggestMatch.tool, suggestMatch.args, suggestMatch.rationale);
    }
    if (const auto backwardMatch = matchDisassembleBackwardTool(query); !backwardMatch.tool.isEmpty()) {
        return makeToolCall(backwardMatch.tool, backwardMatch.args, backwardMatch.rationale);
    }
    if (const auto findWritesMatch = matchFindWhatWritesTool(query); !findWritesMatch.tool.isEmpty()) {
        return makeToolCall(findWritesMatch.tool, findWritesMatch.args, findWritesMatch.rationale);
    }
    if (const auto testFieldsMatch = matchTestCandidateFieldsTool(query); !testFieldsMatch.tool.isEmpty()) {
        return makeToolCall(testFieldsMatch.tool, testFieldsMatch.args, testFieldsMatch.rationale);
    }
    if (const auto offMemoryMatch = matchOffMemoryTool(q); !offMemoryMatch.tool.isEmpty()) {
        return makeToolCall(offMemoryMatch.tool, {}, offMemoryMatch.rationale);
    }

    // Mode Inspecteur: obtenir une preuve nouvelle avant toute ecriture.
    if (inspectorOrUiCopy || q.contains("diff pages") || q.contains("pages modifiees") || q.contains("pages modifiées")) {
        if (numbers.size() >= 2 && (q.contains("compare") || q.contains("compar") || q.contains("maintenant")
            || q.contains("avant") || q.contains("apres") || q.contains("après"))) {
            return makeToolCall("finish_changed_pages_diff", {
                {"previousValue", numbers.at(0)},
                {"currentValue", numbers.at(1)},
            }, "Mode Inspecteur: je compare les pages modifiees entre l'ancienne et la nouvelle valeur affichee.");
        }

        if (q.contains("fenetre") || q.contains("fenêtre") || q.contains("window") || q.contains("uwp") || q.contains("store")) {
            QVariantMap args;
            if (q.contains("solitaire")) {
                args["titleContains"] = "Solitaire";
                args["includeAllVisible"] = true;
            }
            return makeToolCall("read_window_text", args,
                "Mode Inspecteur: je verifie la fenetre visible pour synchroniser l'observation.");
        }

        return makeToolCall("start_changed_pages_diff", {},
            "Mode Inspecteur: je capture un snapshot lecture seule avant la prochaine variation.");
    }

    if (scanActive && wantsModuleSourcePivot(q)) {
        return makeModuleSourcePivotResponse(m_stateMachine.currentStateName());
    }

    // Recherche active + nouvelle valeur observee => reduction plutot que nouveau scan.
    if (scanActive && !value.isEmpty()) {
        m_stateMachine.setState(AIState::Refining);
        return makeToolCall("next_scan", {{"mode", "exact"}, {"value", value}}, "Une recherche est deja active: je reduis les candidats avec la nouvelle valeur observee.");
    }

    // Recherche active + variation decrite sans valeur => next_scan increased/decreased/changed.
    if (scanActive && value.isEmpty() && describesVariation) {
        m_stateMachine.setState(AIState::Refining);
        return makeToolCall("next_scan", {{"mode", variationMode(q)}},
            "Variation decrite pendant une recherche active: je reduis les candidats par comparaison.");
    }

    // Snapshot unknown capture + variation decrite => unknown_compare.
    if (unknownSnapshotActive && value.isEmpty() && describesVariation) {
        m_stateMachine.setState(AIState::Refining);
        return makeToolCall("unknown_compare", {{"mode", variationMode(q)}, {"valueType", "Auto"}},
            "Snapshot unknown actif: je compare avec la variation decrite.");
    }

    // Intentions speciales valorisees avant le scan brut.
    if ((q.contains("freeze") || q.contains("geler")) && !hasNegatedFreezeInstruction(q)) {
        return makeToolCall("freeze_value", {
            {"address", firstHexAddress(query)},
            {"valueType", inferValueType(query)},
            {"value", value},
            {"enabled", true},
        }, "Freeze demande par l'utilisateur.");
    }
    const bool wantsKernel = q.contains("kernel") || q.contains("noyau");
    if (wantsKernel && (q.contains("write") || q.contains("mettre")) && !value.isEmpty()) {
        return makeToolCall("kernel_write", {
            {"address", firstHexAddress(query)},
            {"valueType", inferValueType(query)},
            {"value", value},
        }, "Ecriture kernel demandee explicitement par l'utilisateur.");
    }
    // Speedhack : place avant les checks generiques ci-dessous, meme raison
    // que wantsKernel plus haut.
    {
        const bool wantsSpeedOff = q.contains("désactiv") || q.contains("desactiv") ||
            ((q.contains("stop") || q.contains("arrêt") || q.contains("normal")) &&
             (q.contains("vitesse") || q.contains("speed") || q.contains("temps") || q.contains("speedhack")));
        const bool wantsPause = q.contains("pause") &&
            (q.contains("temps") || q.contains("vitesse") || q.contains("speedhack") || q.contains("jeu") || q.contains("game"));
        const bool wantsSlow = q.contains("ralent") || q.contains("slow");
        const bool wantsFast = q.contains("accélér") || q.contains("acceler") || q.contains("speed up") || q.contains("speedup");
        const bool wantsSpeedGeneric = q.contains("vitesse") || q.contains("speed");
        if (wantsSpeedOff) {
            return makeToolCall("speedhack_set", {{"mode", "off"}}, "Desactivation du speedhack demandee.");
        }
        if (wantsPause) {
            return makeToolCall("speedhack_set", {{"mode", "set"}, {"factor", 0.0}}, "Pause du temps demandee (speedhack).");
        }
        if (wantsSlow || wantsFast || wantsSpeedGeneric) {
            double factor = wantsSlow ? 0.5 : 2.0;
            bool parsedOk = false;
            const double parsed = value.toDouble(&parsedOk);
            if (parsedOk && parsed > 0.0) factor = parsed;
            return makeToolCall("speedhack_set", {{"mode", "set"}, {"factor", factor}},
                wantsSlow ? "Ralentissement demande (speedhack)." : "Acceleration demandee (speedhack).");
        }
    }
    // PHASE 148 : block_process_network, meme raison que wantsKernel plus haut.
    {
        const bool wantsNetworkOff = q.contains("rétablis") || q.contains("retablis")
            || q.contains("restore network") || q.contains("unblock network")
            || ((q.contains("réseau") || q.contains("reseau") || q.contains("network"))
                && (q.contains("rétabli") || q.contains("retabli") || q.contains("remets")));
        const bool wantsNetworkOn = !wantsNetworkOff
            && (q.contains("coupe le réseau") || q.contains("coupe le reseau")
                || q.contains("bloque le réseau") || q.contains("bloque le reseau")
                || q.contains("isole le réseau") || q.contains("isole le reseau")
                || q.contains("block network") || q.contains("cut network")
                || q.contains("block the network"));
        if (wantsNetworkOff) {
            return makeToolCall("block_process_network", {{"mode", "off"}}, "Retablissement du reseau demande.");
        }
        if (wantsNetworkOn) {
            return makeToolCall("block_process_network", {{"mode", "on"}}, "Coupure reseau demandee par l'utilisateur.");
        }
    }
    if ((q.contains("write") || q.contains("mettre")) && !value.isEmpty()) {
        return makeToolCall("write_value", {
            {"address", firstHexAddress(query)},
            {"valueType", inferValueType(query)},
            {"value", value},
        }, "Ecriture memoire demandee.");
    }

    // Ecriture de la cible sans nouvelle valeur + peu de candidats =>
    // checkpoint safe (prepare) plutot que write direct.
    if ((q.contains("écri") || q.contains("ecri") || q.contains("write") || q.contains("checkpoint")
         || q.contains("finalis") || q.contains("valide"))
        && value.isEmpty() && scanActive && candidateCount > 0 && candidateCount <= 10
        && !contextTargetValue.isEmpty()) {
        return makeToolCall("prepare_write_checkpoint", {{"value", contextTargetValue}},
            "Peu de candidats et valeur cible connue: je prepare le checkpoint d'ecriture (sans ecrire).");
    }

    // Signalement d'echec: proposer une alternative adaptee plutot que refaire pareil.
    if (looksLikeBadTargets(q)) {
        const QVariantMap lastTurn = lastHistoryTurn();
        const QString lastOutcome = lastTurn.value("outcome").toString();
        if (!contextInitialValue.isEmpty()
            && (lastOutcome == "failed" || lastTurn.value("tool").toString() == "exact_scan")) {
            return makeToolCall("exact_scan_multi_type", {{"value", contextInitialValue}},
                "Les dernieres adresses ne marchent pas: je relance en multi-type pour couvrir d'autres representations.");
        }
        QVariantMap result;
        result["status"] = "needs_clarification";
        result["message"] = "Compris, ces adresses ne sont pas les bonnes. Donne-moi une valeur observee pour relancer "
                            "en multi-type, ou decris la valeur (affichee a l'ecran, chiffree, inconnue...).";
        result["state"] = m_stateMachine.currentStateName();
        return result;
    }

    if (q.contains("unknown") || q.contains("inconnue")
        || (q.contains("sais pas") && (q.contains("valeur") || q.contains("vaut")))
        || (q.contains("augmente") && value.isEmpty() && !scanActive)
        || (q.contains("diminue") && value.isEmpty() && !scanActive)) {
        if (q.contains("capture") || q.contains("initial") || value.isEmpty()) {
            m_stateMachine.setState(AIState::WaitingForUserChange);
            return makeToolCall("unknown_capture", {}, "Capture initiale pour valeur inconnue.");
        }
        QString mode = "changed";
        if (q.contains("augment") || q.contains("increased")) mode = "increased";
        if (q.contains("diminu") || q.contains("decreased")) mode = "decreased";
        return makeToolCall("unknown_compare", {{"mode", mode}, {"valueType", "Auto"}}, "Comparaison unknown initial value.");
    }
    // Valeur affichee a l'ecran introuvable en numerique.
    if ((q.contains("affich") || q.contains("texte")) && !value.isEmpty()) {
        return makeToolCall("trace_ui_string", {{"value", value}}, "Valeur affichee a l'ecran: je cherche la string UI puis ses sources.");
    }
    // Valeur potentiellement chiffree/obfusquee.
    if ((q.contains("chiffr") || q.contains("obfusqu") || q.contains("crypt") || q.contains("xor")) && !value.isEmpty()) {
        QVariantMap args;
        args["value"] = value;
        args["valueType"] = inferValueType(query);
        args["mode"] = "xor";
        args["keySearchBits"] = 16;
        return makeToolCall("encrypted_scan", args, "Valeur possiblement chiffree: scan XOR/Add/Sub borne.");
    }

    if (!value.isEmpty()) {
        if (scanActive && candidateCount > 0) {
            m_stateMachine.setState(AIState::Refining);
            return makeToolCall("next_scan", {{"mode", "exact"}, {"value", value}}, "Recherche active avec candidats: reduction avec la nouvelle valeur.");
        }
        m_stateMachine.setState(AIState::FirstScanRunning);
        return makeToolCall("exact_scan", {{"value", value}, {"valueType", inferValueType(query)}}, "Premier scan exact depuis une valeur detectee.");
    }

    // Demande de conseil sur une valeur affichee, mais sans valeur concrete :
    // ne pas lancer auto_resolve qui echouerait aussitot faute de nombre.
    if (q.contains("valeur affich") && value.isEmpty()
        && (q.contains("comment") || q.contains("conseil") || q.contains("trouve")
            || q.contains("chercher") || q.contains("trace ui") || q.contains("changed pages"))) {
        QVariantMap result;
        result["status"] = "needs_clarification";
        result["actionStatus"] = "not_executed";
        result["message"] = "Pour une valeur affichée, il me faut d'abord le nombre exact visible à l'écran. "
                            "Ensuite je peux chercher le texte affiché (Trace UI string) ou capturer les pages modifiées avant/après une variation (Changed Pages), sans écrire ni freezer.";
        result["state"] = m_stateMachine.currentStateName();
        QVariantList recoveryActions;
        recoveryActions.append(QVariantMap{{"id", "trace_ui_string"}, {"label", "Trace UI string"}});
        recoveryActions.append(QVariantMap{{"id", "start_changed_pages_diff"}, {"label", "Changed Pages"}});
        result["recoveryActions"] = recoveryActions;
        return result;
    }

    // Aucune valeur: objectifs complets ou guidance plutot que message brut.
    if (q.contains("trouve") || q.contains("cherche") || q.contains("objectif") || q.contains("guide")) {
        return makeToolCall("auto_resolve", {{"query", query}}, "Objectif complet sans valeur directe: mini-boucle safe Auto.");
    }

    QVariantMap result;
    result["status"] = "needs_clarification";
    result["message"] = "Je n'ai pas trouve de valeur ou d'action claire. Donne-moi la valeur affichee (ex: 41250), decris ce que tu cherches (ca augmente quand...), ou colle une adresse 0x....";
    result["state"] = m_stateMachine.currentStateName();
    result["availableTools"] = m_registry.availableTools();
    return result;
}
QVariantMap AIEngine::makeToolCall(const QString& tool, const QVariantMap& args, const QString& rationale) {
    QVariantMap call;
    call["tool"] = tool;
    call["args"] = args;

    QString error;
    QVariantMap result;
    result["status"] = m_validator.validate(call, &error) ? "tool_call" : "invalid_tool_call";
    result["tool"] = tool;
    result["args"] = args;
    result["rationale"] = rationale;
    result["state"] = m_stateMachine.currentStateName();
    result["error"] = error;
    return result;
}

QString AIEngine::inferValueType(const QString& query) {
    const QString q = query.toLower();
    if (q.contains("double") || q.contains("float64")) return "Float64";
    if (q.contains("float") || q.contains("float32")) return "Float32";
    if (q.contains("int64") || q.contains("long")) return "Int64";
    return "Int32";
}

QString AIEngine::firstNumber(const QString& query) {
    const QRegularExpression re(R"([-+]?\d+(?:[\.,]\d+)?)");
    const auto match = re.match(query);
    if (!match.hasMatch()) return {};
    return match.captured(0).replace(',', '.');
}

QStringList AIEngine::allNumbers(const QString& query) {
    QStringList numbers;
    const QRegularExpression re(R"([-+]?\d+(?:[\.,]\d+)?)");
    auto it = re.globalMatch(query);
    while (it.hasNext()) {
        numbers.append(it.next().captured(0).replace(',', '.'));
    }
    return numbers;
}

QString AIEngine::firstHexAddress(const QString& query) {
    const QRegularExpression re(R"(0x[0-9a-fA-F]+)");
    const auto match = re.match(query);
    return match.hasMatch() ? match.captured(0) : QString();
}

} // namespace killai
