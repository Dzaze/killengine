#include "ai_engine.h"
#include "investigation_notebook_planner.h"
#include "intent_contract.h"
#include "query_text_utils.h"
#include "localization/localization.h"
#include "logging/logger.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QVariantList>

#include <algorithm>

namespace killai {

namespace {

constexpr int kMaxHistoryTurns = 12;

// PHASE (11/09/2026, calibration du prechauffage) : seuil au-dela duquel on
// propose un choix explicite (attendre / desactiver pour la session) plutot
// que de tenter silencieusement un prechauffage qui risque fortement de
// timeout -- voir l'enquete goulot d'etranglement dans docs/PHASE_TRACKER.md.
constexpr double kSlowWarmupThresholdSeconds = 60.0;
// Marge de securite appliquee a l'estimation (bruit de mesure sur un petit
// echantillon de calibration) avant de choisir/comparer un budget de temps.
constexpr double kWarmupTimeoutSafetyMargin = 1.5;
// Meme valeur que kDefaultCompletionTimeoutMs (ai/llama_server.cpp, prive a ce
// fichier) : plancher pour ne jamais reduire le budget en dessous du defaut
// deja eprouve, meme si une estimation calibree ressort plus courte (bruit).
constexpr int kMinWarmupTimeoutMs = 45000;
// Taille de l'echantillon de calibration (prefixe du vrai prompt de
// prechauffage) : assez grand pour amortir le cout fixe par requete (mesure
// peu fiable sur un texte trop court, cf. enquete du 11/09/2026), assez petit
// pour rester rapide meme sur une machine tres lente.
constexpr int kCalibrationSampleChars = 2000;

bool looksLikeBadTargets(const QString& q) {
    return q.contains("marche pas") || q.contains("marché pas") || q.contains("pas marché")
        || q.contains("ne marche pas") || q.contains("mauvaise adresse")
        || q.contains("pas bon") || q.contains("rien change")
        // Equivalents anglais (localisation du chat IA, 09/09/2026).
        || q.contains("doesn't work") || q.contains("doesnt work") || q.contains("does not work")
        || q.contains("not working") || q.contains("wrong address")
        || q.contains("not good") || q.contains("no change");
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

bool wantsChangedPages(const QString& q) {
    return q.contains("changed pages")
        || q.contains("diff pages")
        || q.contains("pages modifiees")
        || q.contains("pages modifiées")
        || q.contains("pages qui changent")
        || q.contains("pages changées")
        || q.contains("pages changees")
        || q.contains("comparaison de pages")
        || q.contains("compare les pages")
        || q.contains("comparer les pages");
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

bool wantsModuleExplorationWithoutValue(const QString& q) {
    const bool mentionsModule = q.contains("dll") || q.contains("module") || q.contains("modules");
    const bool asksDiscovery = q.contains("trouve") || q.contains("trouver")
        || q.contains("cherche") || q.contains("chercher") || q.contains("localise")
        || q.contains("localiser") || q.contains("falloir") || q.contains("find");
    const bool mentionsTarget = q.contains("xp") || q.contains("experience") || q.contains("expérience")
        || q.contains("score") || q.contains("niveau") || q.contains("level")
        || q.contains("argent") || q.contains("money") || q.contains("minerai")
        || q.contains("mineral") || q.contains("ressource");
    return mentionsModule && asksDiscovery && mentionsTarget;
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
        KE_TXT("Je limite le scan exact au module/DLL indiqué au lieu de scanner tout le processus.",
               "I'm limiting the exact scan to the specified module/DLL instead of scanning the whole process.")};
}

QVariantMap makeModuleSourcePivotResponse(const QString& stateName) {
    QVariantMap result;
    result["status"] = "needs_clarification";
    result["actionStatus"] = "not_executed";
    result["message"] = KE_TXT(
        "D'accord, on arrête de réduire en exact/increased : tu demandes un pivot vers les DLL/modules et la vraie source XP. "
        "Liste d'abord les modules du processus, repère le module applicatif Solitaire/WebView pertinent, puis utilise AOB/désassemblage ou Trace UI string/Changed Pages pour relier l'affichage XP à la source.",
        "Okay, stopping the exact/increased reduction: you're asking to pivot toward the DLLs/modules and the real XP source. "
        "First list the process modules, spot the relevant Solitaire/WebView application module, then use AOB/disassembly or Trace UI string/Changed Pages to link the XP display to its source.");
    result["state"] = stateName;
    QVariantList recoveryActions;
    recoveryActions.append(QVariantMap{{"id", "open_expert"}, {"label", KE_TXT("Ouvrir Expert", "Open Expert")}, {"expertStep", "inspect"}});
    recoveryActions.append(QVariantMap{{"id", "trace_ui_string"}, {"label", KE_TXT("Trace UI string", "Trace UI string")}});
    recoveryActions.append(QVariantMap{{"id", "start_changed_pages_diff"}, {"label", KE_TXT("Changed Pages", "Changed Pages")}});
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
            KE_TXT("J'inspecte en lecture seule la ruche LocalSettings/settings.dat du package UWP attache.",
                   "I'm inspecting the attached UWP package's LocalSettings/settings.dat hive, read-only.")};
    }

    // Sans chemin de fichier deja connu, "surveiller" n'est pas actionnable
    // directement (watch_save_file exige un path) -- on route d'abord vers
    // la decouverte, etape necessaire de toute facon avant de surveiller.
    if (q.contains("watch fichier") || q.contains("surveille fichier") || q.contains("surveiller fichier")
        || q.contains("surveillance fichier") || q.contains("watch file") || q.contains("file watch")) {
        return {"discover_save_files",
            KE_TXT("Je cherche d'abord les fichiers de sauvegarde disponibles -- tu pourras ensuite me demander de surveiller l'un d'eux.",
                   "I'm first looking for the available save files -- you can then ask me to watch one of them.")};
    }

    if (q.contains("fichier de sauvegarde") || q.contains("fichiers de sauvegarde")
        || q.contains("sauvegarde disque") || q.contains("sur le disque") || q.contains("on disk")
        || q.contains("dans un fichier") || q.contains("save file") || q.contains("savefile")
        || q.contains("localstate")) {
        return {"discover_save_files",
            KE_TXT("Je cherche les fichiers de sauvegarde/etat du processus attache sur le disque.",
                   "I'm looking for the attached process's save/state files on disk.")};
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
        actions.append(QVariantMap{{"id", "open_expert"}, {"label", KE_TXT("Ouvrir Expert pour lancer le scan", "Open Expert to start the scan")}, {"expertStep", "find"}});
    } else if (topic == "displayed_value_not_found") {
        actions.append(QVariantMap{{"id", "trace_ui_string"}, {"label", KE_TXT("Lancer Trace UI string", "Launch Trace UI string")}});
    } else if (topic == "unstable_address") {
        actions.append(QVariantMap{{"id", "open_pointer_scan"}, {"label", KE_TXT("Ouvrir Expert, section Pointeurs", "Open Expert, Pointers section")}});
    } else if (topic == "freeze_flickers") {
        actions.append(QVariantMap{{"id", "open_expert"}, {"label", KE_TXT("Ouvrir Expert pour analyser la stabilité du champ", "Open Expert to analyze field stability")}, {"expertStep", "inspect"}});
    } else if (topic == "code_patch_request") {
        actions.append(QVariantMap{{"id", "open_expert"}, {"label", KE_TXT("Ouvrir Expert pour générer l'AOB", "Open Expert to generate the AOB")}, {"expertStep", "persist"}});
    } else if (topic == "what_writes_value") {
        actions.append(QVariantMap{
            {"id", "open_expert"}, {"label", KE_TXT("Ouvrir Expert pour lancer Écrit par", "Open Expert to launch What writes")},
            {"expertStep", "find"}, {"expertAnchor", "expert-anchor-find-what-writes"}});
    } else if (topic == "save_file_or_uwp") {
        actions.append(QVariantMap{{"id", "open_expert"}, {"label", KE_TXT("Ouvrir Expert, section fichiers de sauvegarde", "Open Expert, save files section")}, {"expertStep", "inspect"}});
    } else if (topic == "managed_runtime_pointer_chain") {
        actions.append(QVariantMap{{"id", "open_clr_inspector"}, {"label", KE_TXT("Ouvrir CLR Inspector", "Open CLR Inspector")}});
    }
    return actions;
}

QVariantMap makeInvestigationPlaybookResponse(const QString& topic) {
    // PHASE (08/09/2026, docs/AI_CHAT_LOCALIZATION_ROADMAP.md, L3) : chaque
    // champ porte desormais une paire {fr, en} au lieu d'un seul const char*
    // -- localizedText() choisit la bonne variante au moment de construire le
    // message final (voir `pick` plus bas), sans toucher a la structure des
    // 9 entrees (une par sujet reconnu + le fallback generique).
    struct Text { const char* fr; const char* en; };
    struct Entry {
        Text title;
        Text hypotheses;
        Text tool;
        Text prerequisites;
        Text risk;
        Text nextAction;
        Text fallback;
    };

    Entry entry{
        {"Enquête guidée", "Guided investigation"},
        {"Symptôme reconnu, mais il manque encore une cible actionnable.",
         "Symptom recognized, but an actionable target is still missing."},
        {"Consulter le playbook d'enquête.", "Consult the investigation playbook."},
        {"Décrire la valeur, l'adresse ou le contexte observé.",
         "Describe the value, address, or context observed."},
        {"Aucun : cette réponse est strictement lecture seule.",
         "None: this response is strictly read-only."},
        {"Suivre le plan proposé avant toute action à risque.",
         "Follow the proposed plan before any risky action."},
        {"Donner une valeur, une adresse ou une observation plus précise.",
         "Provide a more precise value, address, or observation."}
    };

    if (topic == "simple_visible_value") {
        entry = {
            {"Valeur numérique simple visible", "Simple visible numeric value"},
            {"La valeur est peut-être stockée telle quelle en mémoire, sans obfuscation ni recalcul d'affichage.",
             "The value may be stored as-is in memory, without obfuscation or display recomputation."},
            {"Commencer par exact_scan, puis réduire avec next_scan quand la valeur change.",
             "Start with exact_scan, then narrow down with next_scan when the value changes."},
            {"Processus attaché et valeur actuellement visible à l'écran.",
             "Process attached and value currently visible on screen."},
            {"Aucun pour le scan : lecture seule. Seule une écriture ou un freeze ultérieur demandera confirmation.",
             "None for scanning: read-only. Only a later write or freeze will require confirmation."},
            {"Donne la valeur affichée actuelle, puis fais-la changer pour réduire les candidats.",
             "Give the currently displayed value, then change it to narrow down the candidates."},
            {"Si le scan exact ne converge pas, passer au chemin valeur affichée introuvable / Trace UI string.",
             "If the exact scan doesn't converge, switch to the 'displayed value not found' path / Trace UI string."}
        };
    } else if (topic == "displayed_value_not_found") {
        entry = {
            {"Valeur affichée introuvable", "Displayed value not found"},
            {"La valeur peut être une string UI, une copie d'affichage, ou une représentation transformée.",
             "The value may be a UI string, a display copy, or a transformed representation."},
            {"Utiliser Trace UI string, puis analyser les sources numériques autour des strings suivies.",
             "Use Trace UI string, then analyze the numeric sources around the tracked strings."},
            {"Valeur visible à l'écran sous forme de texte lisible.",
             "Value visible on screen as readable text."},
            {"Aucun pour Trace UI string / analyse des sources : lecture seule.",
             "None for Trace UI string / source analysis: read-only."},
            {"Confirme la valeur affichée exacte, puis observe son évolution avant toute écriture.",
             "Confirm the exact displayed value, then observe how it changes before any write."},
            {"Si aucune source fiable n'apparaît, escalader vers 'qui écrit cette valeur' avec confirmation debugger.",
             "If no reliable source appears, escalate to 'what writes this value' with debugger confirmation."}
        };
    } else if (topic == "unstable_address") {
        entry = {
            {"Adresse instable au redémarrage", "Address unstable across restarts"},
            {"L'adresse absolue est probablement invalidée par l'ASLR ou par une réallocation d'objet.",
             "The absolute address is probably invalidated by ASLR or an object reallocation."},
            {"Transformer la trouvaille en locator Trainer : AOB pour du code, pointer chain pour une donnée.",
             "Turn the finding into a Trainer locator: AOB for code, pointer chain for data."},
            {"Adresse déjà validée comme correcte dans la session actuelle.",
             "Address already validated as correct in the current session."},
            {"Aucun pour générer/chercher un locator ; ne pas promettre de stabilité si seul absolute fonctionne.",
             "None for generating/searching a locator; don't promise stability if only the absolute address works."},
            {"Stabilise l'adresse via AOB ou pointer chain avant d'en faire une feature Trainer durable.",
             "Stabilize the address via AOB or pointer chain before turning it into a lasting Trainer feature."},
            {"Si rien n'est unique/stable, garder absolute en indiquant clairement que ça ne survivra probablement pas.",
             "If nothing is unique/stable, keep the absolute address while clearly noting it likely won't survive."}
        };
    } else if (topic == "freeze_flickers") {
        entry = {
            {"Freeze qui clignote", "Flickering freeze"},
            {"La cible peut réécrire plus vite que le polling, ou l'adresse peut être un champ affiché dérivé.",
             "The target may rewrite faster than the polling rate, or the address may be a derived display field."},
            {"Analyser la stabilité du champ avant d'envisager un freeze breakpoint matériel.",
             "Analyze the field's stability before considering a hardware breakpoint freeze."},
            {"Adresse candidate déjà identifiée.", "Candidate address already identified."},
            {"Analyse de stabilité : lecture seule. Freeze BP : debugger, confirmation obligatoire.",
             "Stability analysis: read-only. Freeze BP: debugger, confirmation required."},
            {"Vérifie d'abord si l'adresse est une vraie source ou seulement un affichage recalculé.",
             "First check whether the address is a real source or just a recomputed display."},
            {"Si c'est un affichage dérivé, chercher l'origine de l'écriture plutôt que freezer cette copie.",
             "If it's a derived display, look for the write's origin rather than freezing this copy."}
        };
    } else if (topic == "code_patch_request") {
        entry = {
            {"Patch de code demandé", "Code patch requested"},
            {"L'objectif touche probablement une instruction machine plutôt qu'une simple donnée.",
             "The goal likely involves a machine instruction rather than a simple piece of data."},
            {"Si l'adresse vient d'un hit 'Écrit par' : désassembler en arrière D'ABORD (le RIP capturé pointe sur "
             "l'instruction suivante, pas l'écriture elle-même — sémantique standard d'un breakpoint matériel), "
             "PUIS générer une AOB sur la vraie instruction trouvée, puis suggérer un patch.",
             "If the address comes from a 'What writes' hit: disassemble backward FIRST (the captured RIP points to "
             "the following instruction, not the write itself — standard hardware breakpoint semantics), THEN "
             "generate an AOB on the actual instruction found, then suggest a patch."},
            {"Adresse de code valide. Si elle vient d'un hit 'Écrit par' : c'est le RIP capturé, pas encore l'adresse "
             "réelle de l'instruction à patcher — désassembler en arrière d'abord pour la retrouver.",
             "Valid code address. If it comes from a 'What writes' hit: it's the captured RIP, not yet the actual "
             "address of the instruction to patch — disassemble backward first to find it."},
            {"Élevé pour l'application réelle : patch=confirmation humaine, jamais auto-exécuté depuis le chat.",
             "High for actually applying it: patching requires human confirmation, never auto-executed from chat."},
            {"Ne jamais générer d'AOB directement sur un RIP brut issu d'un hit 'Écrit par' : localise d'abord la "
             "vraie instruction d'écriture, vérifie l'unicité de la signature, puis applique seulement depuis "
             "Expert/Trainer après confirmation.",
             "Never generate an AOB directly on a raw RIP from a 'What writes' hit: first locate the actual write "
             "instruction, verify the signature is unique, then only apply it from Expert/Trainer after confirmation."},
            {"Si la signature est ambiguë ou bloquée par l'environnement, revenir à un write/freeze moins invasif.",
             "If the signature is ambiguous or blocked by the environment, fall back to a less invasive write/freeze."}
        };
    } else if (topic == "what_writes_value") {
        entry = {
            {"Comprendre qui écrit une valeur", "Understand what writes a value"},
            {"Plusieurs sites de code peuvent écrire la même adresse ; il faut identifier la vraie source gameplay.",
             "Multiple code sites can write to the same address; the real gameplay source needs to be identified."},
            {"Utiliser 'Écrit par' / find_what_writes depuis l'UI Expert, avec confirmation.",
             "Use 'What writes' / find_what_writes from the Expert UI, with confirmation."},
            {"Adresse stable déjà connue, pas un slot trop chaud ou générique. Si tu n'as pas encore d'adresse : "
             "fais d'abord un scan classique (donne-moi la valeur affichée à l'écran) pour en trouver une et la "
             "sélectionner comme candidat — 'Écrit par' ne peut rien capturer sans ça.",
             "A stable address already known, not an overly hot or generic slot. If you don't have an address yet: "
             "first run a regular scan (give me the value displayed on screen) to find one and select it as a "
             "candidate — 'What writes' can't capture anything without that."},
            {"Debugger : peut perturber la cible, confirmation obligatoire.",
             "Debugger: can disturb the target, confirmation required."},
            {"Si tu as déjà une adresse : prépare-la, lance la capture confirmée, puis interagis avec le jeu pendant "
             "la fenêtre. Sinon : commence par le scan décrit ci-dessus, reviens ensuite avec l'adresse trouvée.",
             "If you already have an address: prepare it, launch the confirmed capture, then interact with the game "
             "during the window. Otherwise: start with the scan described above, then come back with the address "
             "you found."},
            {"Si aucun hit n'apparaît, élargir la fenêtre ou revérifier que l'adresse est bien stable.",
             "If no hit appears, widen the window or double-check that the address is actually stable."}
        };
    } else if (topic == "save_file_or_uwp") {
        entry = {
            {"Valeur dans sauvegarde ou LocalSettings", "Value in a save file or LocalSettings"},
            {"La valeur peut vivre sur disque ou dans une ruche UWP plutôt qu'en RAM exploitable.",
             "The value may live on disk or in a UWP hive rather than in exploitable RAM."},
            {"Découvrir les fichiers de sauvegarde, lire le texte, inspecter LocalSettings, puis comparer avant/après.",
             "Discover save files, read the text, inspect LocalSettings, then compare before/after."},
            {"Jeu avec fichier de sauvegarde identifiable ou processus UWP attaché.",
             "Game with an identifiable save file, or a UWP process attached."},
            {"Lecture seule pour inspection. Toute écriture disque nécessite une action explicite séparée.",
             "Read-only for inspection. Any disk write requires a separate, explicit action."},
            {"Compare un état avant/après une action utilisateur pour isoler le champ modifié.",
             "Compare a before/after state around a user action to isolate the modified field."},
            {"Si une source plus autoritaire réécrit le fichier, il faudra un protocole d'enquête plus large hors 120-A.",
             "If a more authoritative source rewrites the file, a broader investigation protocol beyond 120-A will be needed."}
        };
    } else if (topic == "managed_runtime_pointer_chain") {
        entry = {
            {"Cible sur runtime managé (.NET/Mono) — scan de pointeurs natif aveugle",
             "Target on a managed runtime (.NET/Mono) — blind native pointer scan"},
            {"Le processus charge coreclr.dll/clrjit.dll (ou mono*.dll) : les données de gameplay vivent sur un tas géré "
             "par le GC, pas dans les sections .data/.bss d'un module PE natif. Un scanPointerChains, même borné serré, "
             "ne trouvera structurellement aucune chaîne depuis un module natif — signe distinctif : réponse rapide "
             "mais chainCount:0, quel que soit le module d'ancrage essayé.",
             "The process loads coreclr.dll/clrjit.dll (or mono*.dll): gameplay data lives on a GC-managed heap, not "
             "in a native PE module's .data/.bss sections. A scanPointerChains, even tightly bounded, will "
             "structurally never find a chain from a native module — telltale sign: a fast response but "
             "chainCount:0, regardless of which anchor module is tried."},
            {"Basculer sur le CLR Inspector : attachClrInspector, puis chercher l'objet par type/valeur de champ "
             "(findClrObjectsByType/findClrObjectsByFieldValue) et descendre la hiérarchie des champs (readClrObject) "
             "jusqu'au champ primitif, plutôt que de deviner une adresse brute.",
             "Switch to the CLR Inspector: attachClrInspector, then search for the object by type/field value "
             "(findClrObjectsByType/findClrObjectsByFieldValue) and walk down the field hierarchy (readClrObject) to "
             "the primitive field, instead of guessing a raw address."},
            {"Cible confirmée managée (getProcessModules montre coreclr.dll/clrjit.dll ou mono*.dll) ; CLR Inspector attaché au bon PID.",
             "Target confirmed as managed (getProcessModules shows coreclr.dll/clrjit.dll or mono*.dll); CLR Inspector attached to the right PID."},
            {"Aucun pour attachClrInspector/findClrObjectsBy*/readClrObject : lecture seule. writeClrPrimitivePath demande une confirmation comme toute écriture classique.",
             "None for attachClrInspector/findClrObjectsBy*/readClrObject: read-only. writeClrPrimitivePath requires confirmation like any regular write."},
            {"Ne pas répéter scanPointerChains avec des bornes toujours plus larges sur ce type de cible : un résultat "
             "vide et rapide est déjà le signal qu'il faut changer d'outil. Si le type de premier niveau n'a pas le "
             "champ attendu, chercher un mot-clé de domaine plus large (le studio range parfois la donnée sur un objet conteneur).",
             "Don't keep repeating scanPointerChains with ever-wider bounds on this kind of target: a fast, empty "
             "result is already the signal to switch tools. If the top-level type doesn't have the expected field, "
             "try a broader domain keyword (the studio sometimes stores the data on a container object)."},
            {"Si aucun mot-clé de domaine ne donne de type candidat, élargir avec des synonymes techniques du genre de "
             "jeu concerné.",
             "If no domain keyword yields a candidate type, widen the search with technical synonyms for the game's genre."}
        };
    }

    auto pick = [](const Text& t) { return killcore::localizedText(QString::fromUtf8(t.fr), QString::fromUtf8(t.en)); };

    QVariantMap result;
    result["status"] = "needs_clarification";
    result["actionStatus"] = "not_executed";
    result["message"] = KE_TXT(
        "D'après ma méthode d'enquête intégrée, je traiterais ça comme : %1\n\n"
        "Hypothèses : %2\n"
        "Outil conseillé : %3\n"
        "Prérequis : %4\n"
        "Risque : %5\n"
        "Prochaine action humaine : %6\n"
        "Fallback : %7\n\n"
        "Je n'exécute rien automatiquement depuis cette réponse : pas d'action lancée toute seule, pas de contournement de la confirmation.",
        "Based on my built-in investigation method, I'd treat this as: %1\n\n"
        "Hypotheses: %2\n"
        "Recommended tool: %3\n"
        "Prerequisites: %4\n"
        "Risk: %5\n"
        "Next human action: %6\n"
        "Fallback: %7\n\n"
        "I'm not executing anything automatically from this response: no action launched on its own, no bypassing confirmation.")
        .arg(pick(entry.title),
             pick(entry.hypotheses),
             pick(entry.tool),
             pick(entry.prerequisites),
             pick(entry.risk),
             pick(entry.nextAction),
             pick(entry.fallback));
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
            KE_TXT("Activation Trainer demandee: je prepare une confirmation UI, sans appeler directement le RiskGate.",
                   "Trainer activation requested: I'm preparing a UI confirmation, without calling the RiskGate directly.")};
    }
    if (wantsRestore) {
        return {"trainer_restore_request",
            {{"id", firstDecimalOutsideHex(q)}, {"all", q.contains("all") || q.contains("tout")}},
            KE_TXT("Restauration Trainer demandee: je prepare une confirmation UI, sans appeler directement le RiskGate.",
                   "Trainer restore requested: I'm preparing a UI confirmation, without calling the RiskGate directly.")};
    }
    if (wantsDelete) {
        return {"trainer_delete_feature",
            {{"id", firstDecimalOutsideHex(q)}},
            KE_TXT("Suppression d'une feature Trainer demandee.", "Trainer feature deletion requested.")};
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
            KE_TXT("Creation d'une feature Trainer write demandee depuis une adresse et une valeur explicites.",
                   "Trainer write feature creation requested from an explicit address and value.")};
    }
    if (wantsList) {
        return {"trainer_list_features", {},
            KE_TXT("Je liste les features Trainer locales via le pont UI lecture seule.",
                   "I'm listing the local Trainer features via the read-only UI bridge.")};
    }

    return {"trainer_list_features", {},
        KE_TXT("Demande Trainer generale: je commence par lister l'etat actuel des features.",
               "General Trainer request: I'm starting by listing the current state of the features.")};
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
        KE_TXT("J'observe passivement les écritures sur cette adresse (aucune écriture de ma part) pour juger si "
               "elle ressemble à un champ affiché recalculé ou à une source événementielle.",
               "I'm passively observing the writes to this address (no writes of my own) to judge whether "
               "it looks like a recomputed display field or an event-driven source.")};
}

struct WebView2ToolMatch {
    QString tool;
    QVariantMap args;
    QString rationale;
};

bool wantsWebView2Query(const QString& q) {
    return q.contains("webview2") || q.contains("webview")
        || q.contains("cdp") || q.contains("devtools")
        || q.contains("javascript") || q.contains("script js")
        || q.contains("runtime.evaluate") || q.contains("dom");
}

int webView2PidFromQueryOrContext(const QString& query, const QVariantMap& context) {
    const QRegularExpression pidRe(R"(\bpid\s*[:=]?\s*(\d{2,10})\b)", QRegularExpression::CaseInsensitiveOption);
    const auto pidMatch = pidRe.match(query);
    if (pidMatch.hasMatch()) {
        bool ok = false;
        const int pid = pidMatch.captured(1).toInt(&ok);
        if (ok && pid > 0) {
            return pid;
        }
    }
    bool ok = false;
    const int contextPid = context.value("pid", context.value("processId")).toInt(&ok);
    return ok && contextPid > 0 ? contextPid : 0;
}

QString firstQuotedText(const QString& query) {
    const QRegularExpression quotedRe(QStringLiteral("\"([^\"]+)\"|'([^']+)'"));
    const auto match = quotedRe.match(query);
    if (!match.hasMatch()) {
        return {};
    }
    return match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
}

QString javascriptExpressionFromQuery(const QString& query) {
    const QString quoted = firstQuotedText(query);
    if (!quoted.isEmpty()) {
        return quoted;
    }
    const QRegularExpression afterMarkerRe(R"((?:js|javascript|eval|evalue|évalue|runtime\.evaluate)\s*[:=]\s*(.+)$)",
        QRegularExpression::CaseInsensitiveOption);
    const auto match = afterMarkerRe.match(query);
    return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

WebView2ToolMatch matchWebView2Tool(const QString& query, const QVariantMap& context) {
    const QString q = query.toLower();
    if (!wantsWebView2Query(q)) {
        return {};
    }

    if (q.contains("status") || q.contains("statut") || q.contains("etat") || q.contains("état")) {
        return {"getWebView2InspectorStatus", {},
            KE_TXT("Je verifie l'etat courant de l'inspecteur WebView2/CDP.", "I'm checking the current state of the WebView2/CDP inspector.")};
    }

    const int browserProcessId = webView2PidFromQueryOrContext(query, context);
    const QVariantMap pidArgs{{"browserProcessId", browserProcessId}};
    if (q.contains("liste") || q.contains("lister") || q.contains("list")
        || q.contains("target") || q.contains("targets") || q.contains("/msedge")
        || q.contains("json")) {
        return {"listWebView2CdpTargets", pidArgs,
            KE_TXT("Je liste d'abord les targets CDP WebView2 disponibles avant toute connexion.",
                   "I'm first listing the available WebView2 CDP targets before connecting.")};
    }

    if (q.contains("disconnect") || q.contains("deconnect") || q.contains("déconnect")) {
        return {"disconnectWebView2Inspector", {},
            KE_TXT("Je deconnecte l'inspecteur WebView2/CDP courant.", "I'm disconnecting the current WebView2/CDP inspector.")};
    }

    if (q.contains("connect") || q.contains("attache") || q.contains("attach")
        || q.contains("branche")) {
        if (browserProcessId <= 0 && !q.contains("websocketdebuggerurl")) {
            return {"listWebView2CdpTargets", pidArgs,
                KE_TXT("Avant de connecter WebView2, je liste les targets CDP disponibles pour choisir le bon PID/target.",
                       "Before connecting to WebView2, I'm listing the available CDP targets to pick the right PID/target.")};
        }
        return {"connectWebView2Inspector", pidArgs,
            KE_TXT("Je prepare la connexion a une target WebView2/CDP ; cette action passe par le RiskGate.",
                   "I'm preparing the connection to a WebView2/CDP target; this action goes through the RiskGate.")};
    }

    if (q.contains("sonde") || q.contains("sonder") || q.contains("reconnaissance")
        || q.contains("globales") || q.contains("quelles fonctions") || q.contains("quelles variables")
        || q.contains("qu'est-ce qui est disponible") || q.contains("quoi evaluer") || q.contains("quoi évaluer")) {
        return {"probeWebView2GlobalScope", {},
            KE_TXT("Je sonde le scope JS global de la target connectee pour voir ce qui est disponible avant de deviner du JS a l'aveugle.",
                   "I'm probing the connected target's global JS scope to see what's available before guessing at JS blindly.")};
    }

    const QString expression = javascriptExpressionFromQuery(query);
    if (!expression.isEmpty()) {
        return {"evaluateWebView2JavaScript", {{"expression", expression}},
            KE_TXT("J'evalue le JavaScript demande dans la target WebView2 connectee ; cette action passe par le RiskGate.",
                   "I'm evaluating the requested JavaScript in the connected WebView2 target; this action goes through the RiskGate.")};
    }

    if ((q.contains("valeur") || q.contains("value")) && !decimalNumbersFromQuery(query).isEmpty()) {
        return {"findWebView2DisplayedValues", {{"value", decimalNumbersFromQuery(query).first()}},
            KE_TXT("Je cherche cette valeur affichee dans le DOM WebView2 connecte.", "I'm searching for this displayed value in the connected WebView2 DOM.")};
    }

    if (q.contains("texte") || q.contains("text") || q.contains("dom")) {
        const QString text = firstQuotedText(query);
        if (!text.isEmpty()) {
            return {"findWebView2DisplayedText", {{"text", text}},
                KE_TXT("Je cherche ce texte dans le DOM WebView2 connecte.", "I'm searching for this text in the connected WebView2 DOM.")};
        }
    }

    return {"listWebView2CdpTargets", pidArgs,
        KE_TXT("Demande WebView2/CDP detectee : je commence par lister les targets disponibles.",
               "WebView2/CDP request detected: I'm starting by listing the available targets.")};
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
        KE_TXT("Je résume le contexte, la stratégie recommandée et les événements récents (lecture seule).",
               "I'm summarizing the context, the recommended strategy, and recent events (read-only).")};
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
        KE_TXT("Je cherche les sources numériques probables près de la dernière string UI localisée (lecture seule).",
               "I'm looking for probable numeric sources near the last located UI string (read-only).")};
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
    return {"generate_aob", args, KE_TXT("Je génère une signature AOB pour cette instruction (lecture seule).", "I'm generating an AOB signature for this instruction (read-only).")};
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
        KE_TXT("Je suggère des patchs de code possibles pour cette instruction, sans en appliquer aucun.",
               "I'm suggesting possible code patches for this instruction, without applying any of them.")};
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
        KE_TXT("Je désassemble en arrière depuis cette instruction pour repérer les champs sources candidats (lecture seule).",
               "I'm disassembling backward from this instruction to spot candidate source fields (read-only).")};
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

LlamaGenerationResult AIEngine::warmupLocalModel(const std::function<void(const QString&)>& onStageChanged) {
    if (onStageChanged) onStageChanged(QStringLiteral("initializing"));
    if (!ensureLlamaInitialized()) {
        LlamaGenerationResult result;
        result.errorMessage = m_llama.info().errorMessage;
        return result;
    }
    const auto warmup = m_llama.warmup(m_registry, onStageChanged);
    if (warmup.success) {
        KE_LOG_INFO() << "AIEngine warmup: llama.cpp prompt cache primed (" << warmup.backend.toStdString() << ").";
    } else if (!warmup.errorMessage.isEmpty()) {
        KE_LOG_INFO() << "AIEngine warmup skipped/failed (non-fatal): " << warmup.errorMessage.toStdString();
    }
    return warmup;
}

AIEngine::WarmupCalibrationResult AIEngine::warmupLocalModelWithCalibration(const std::function<void(const QString&)>& onStageChanged) {
    WarmupCalibrationResult result;

    if (onStageChanged) onStageChanged(QStringLiteral("initializing"));
    if (!ensureLlamaInitialized()) {
        result.completion.errorMessage = m_llama.info().errorMessage;
        return result;
    }

    const QString fullPrompt = LlamaRuntime::buildPrompt(QString(), m_registry, QVariantMap());
    const int fullTokenCount = m_llama.tokenCount(fullPrompt);
    if (fullTokenCount <= 0) {
        // Tokenize indisponible (best-effort) : repli sur l'ancien
        // comportement, timeout par defaut plutot que d'echouer tout le flux.
        KE_LOG_INFO() << "AIEngine warmup calibration: tokenize unavailable, falling back to default timeout.";
        result.completion = m_llama.warmup(m_registry, onStageChanged);
        return result;
    }

    if (onStageChanged) onStageChanged(QStringLiteral("measuringSpeed"));
    const QString sample = fullPrompt.left(kCalibrationSampleChars);
    const auto calibration = m_llama.measurePrefillSpeed(sample, onStageChanged);
    if (!calibration.success || calibration.promptTokensPerSecond <= 0.0) {
        KE_LOG_INFO() << "AIEngine warmup calibration failed (non-fatal), falling back to default timeout: "
                      << calibration.errorMessage.toStdString();
        result.completion = m_llama.warmup(m_registry, onStageChanged);
        return result;
    }

    const double estimatedSeconds = (fullTokenCount / calibration.promptTokensPerSecond) * kWarmupTimeoutSafetyMargin;
    KE_LOG_INFO() << "AIEngine warmup calibration: " << calibration.promptTokensPerSecond << " tok/s measured, "
                  << fullTokenCount << " tokens to prefill, estimated " << estimatedSeconds << "s (with margin).";

    if (estimatedSeconds > kSlowWarmupThresholdSeconds) {
        m_pendingWarmupEstimateSeconds = estimatedSeconds;
        result.outcome = WarmupCalibrationResult::Outcome::NeedsDecision;
        result.estimatedSeconds = estimatedSeconds;
        result.estimatedTokenCount = fullTokenCount;
        return result;
    }

    const int timeoutMs = std::max(kMinWarmupTimeoutMs, static_cast<int>(estimatedSeconds * 1000.0));
    result.completion = m_llama.warmup(m_registry, onStageChanged, timeoutMs);
    return result;
}

LlamaGenerationResult AIEngine::continueWarmupAfterEstimate(const std::function<void(const QString&)>& onStageChanged) {
    const int timeoutMs = std::max(kMinWarmupTimeoutMs, static_cast<int>(m_pendingWarmupEstimateSeconds * 1000.0));
    if (onStageChanged) onStageChanged(QStringLiteral("warmingPrompt"));
    return m_llama.warmup(m_registry, onStageChanged, timeoutMs);
}

void AIEngine::disableForSession() {
    m_sessionDisabled = true;
    KE_LOG_INFO() << "AIEngine: local model disabled for this session (user choice after slow warmup estimate).";
}

bool AIEngine::isSessionDisabled() const {
    return m_sessionDisabled;
}

QVariantMap AIEngine::lastHistoryTurn() const {
    return m_history.isEmpty() ? QVariantMap{} : m_history.last().toMap();
}

bool AIEngine::ensureLlamaInitialized() {
    if (m_sessionDisabled) {
        return false;
    }
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
    // PHASE (08/09/2026) : timing loggue pour surveiller la latence reelle du
    // modele local en conditions terrain -- a servi a diagnostiquer un
    // enchainement de timeouts/retries qui pouvait cumuler plusieurs minutes
    // sur une requete texte libre ambigue (voir docs/PHASE_TRACKER.md), fixe
    // en reduisant le budget de generation (LlamaRuntime::planToolCall).
    QElapsedTimer callTimer;
    callTimer.start();
    auto generated = m_llama.planToolCall(query, m_registry, context);
    KE_LOG_INFO() << "AIEngine model tool call: attempt 1 took " << callTimer.elapsed()
                  << "ms, success=" << generated.success << " backend=" << generated.backend.toStdString();
    QString error;
    QVariantMap call = generated.success ? LlamaRuntime::extractToolCallJson(generated.output, &error) : QVariantMap{};

    // Retry correctif borne: une seconde tentative si le modele a repondu
    // mais sans JSON exploitable (hallucination de format, bavardage...).
    if (generated.success && call.isEmpty()) {
        KE_LOG_INFO() << "AIEngine retrying model tool call after invalid JSON: " << error.toStdString();
        const QString correctiveQuery = query + "\n(Rappel: reponds UNIQUEMENT par l'objet JSON du schema, sans texte autour.)";
        callTimer.restart();
        generated = m_llama.planToolCall(correctiveQuery, m_registry, context);
        KE_LOG_INFO() << "AIEngine model tool call: attempt 2 (corrective) took " << callTimer.elapsed()
                      << "ms, success=" << generated.success << " backend=" << generated.backend.toStdString();
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
        result["message"] = KE_TXT("Moteur IA non initialisé.", "AI engine not initialized.");
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
        result["missing"] = KE_TXT("Tu veux le passer à quelle valeur ?", "What value do you want to set it to?");
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
                    ? KE_TXT("Je dois préciser l'intention avant d'agir.", "I need to clarify the intent before acting.")
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
        result["error"] = KE_TXT("Moteur IA non initialisé.", "AI engine not initialized.");
        result["source"] = "not_ready";
        result["modelUsed"] = false;
        return result;
    }
    if (trimmed.isEmpty()) {
        QVariantMap result;
        result["success"] = false;
        result["error"] = KE_TXT("Symptôme vide.", "Empty symptom.");
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
        result["message"] = KE_TXT("Moteur IA non initialisé.", "AI engine not initialized.");
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
        result["message"] = KE_TXT(
            "Salut ! Dis-moi ce que tu veux chercher ou comprendre : une valeur affichée, une adresse, "
            "un freeze, un trainer, un script Lua, ou une investigation plus guidée.",
            "Hi! Tell me what you want to find or understand: a displayed value, an address, "
            "a freeze, a trainer, a Lua script, or a more guided investigation.");
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
                    KE_TXT("D'après le playbook d'enquête (freeze qui clignote) : ", "From the investigation playbook (flickering freeze): ") + stabilityMatch.rationale);
                result["aiBackend"] = "deterministic_investigation_playbook_autochain";
                result["investigationTopic"] = topic;
                return result;
            }
        } else if (topic == "save_file_or_uwp") {
            if (const auto offMemoryMatch = matchOffMemoryTool(q); !offMemoryMatch.tool.isEmpty()) {
                QVariantMap result = makeToolCall(offMemoryMatch.tool, {},
                    KE_TXT("D'après le playbook d'enquête (valeur en sauvegarde/UWP) : ", "From the investigation playbook (value in a save file/UWP): ") + offMemoryMatch.rationale);
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
        if (wantsModuleExplorationWithoutValue(q) && decimalNumbersFromQuery(query).isEmpty()) {
            QVariantMap result = makeToolCall("list_process_modules", {},
                KE_TXT("Tu demandes une cible de gameplay dans les DLL/modules sans valeur affichée exploitable : je liste d'abord les modules chargés, puis il faudra donner l'XP visible ou passer par Trace UI string/Changed Pages.",
                       "You're asking for a gameplay target in the DLLs/modules without a usable displayed value: I'm first listing the loaded modules, then you'll need to give the visible XP or go through Trace UI string/Changed Pages."));
            result["aiBackend"] = "deterministic_module_listing_fastpath";
            return result;
        }
        if (context.value("scanActive", false).toBool() && wantsModuleSourcePivot(q)) {
            QVariantMap result = makeToolCall("list_process_modules", {},
                KE_TXT("Je liste les modules/DLL charges pour identifier le module applicatif avant de poursuivre vers AOB/desassemblage ou Trace UI string/Changed Pages.",
                       "I'm listing the loaded modules/DLLs to identify the application module before moving on to AOB/disassembly or Trace UI string/Changed Pages."));
            result["aiBackend"] = "deterministic_module_listing_fastpath";
            return result;
        }
        if (wantsProcessModuleListing(q)) {
            QVariantMap result = makeToolCall("list_process_modules", {},
                KE_TXT("Je liste les modules/DLL charges par le processus attache (lecture seule).",
                       "I'm listing the modules/DLLs loaded by the attached process (read-only)."));
            result["aiBackend"] = "deterministic_module_listing_fastpath";
            return result;
        }
        if (const auto webView2Match = matchWebView2Tool(query, context); !webView2Match.tool.isEmpty()) {
            QVariantMap result = makeToolCall(webView2Match.tool, webView2Match.args, webView2Match.rationale);
            result["aiBackend"] = "deterministic_webview2_fastpath";
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
        KE_LOG_INFO() << "AIEngine model tool call resolved: empty=" << call.isEmpty()
                      << " tool=" << call.value("tool").toString().toStdString();
        if (!call.isEmpty()) {
            QString error;
            QVariantMap result;
            result["status"] = m_validator.validate(call, &error) ? "tool_call" : "invalid_tool_call";
            result["tool"] = call.value("tool").toString();
            result["args"] = call.value("args").toMap();
            result["rationale"] = KE_TXT("Plan généré par le modèle local llama.cpp/Qwen.", "Plan generated by the local llama.cpp/Qwen model.");
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
        result["missing"] = KE_TXT("Tu veux le passer à quelle valeur ?", "What value do you want to set it to?");
        result["message"] = result["missing"];
    } else {
        result["status"] = "needs_clarification";
        result["missing"] = KE_TXT("Quelle valeur veux-tu chercher ?", "What value do you want to search for?");
        result["message"] = result["missing"];
    }

    QString error;
    if (result.value("status").toString() == "intent" && !IntentContract::validate(result, &error)) {
        result["status"] = "needs_clarification";
        result["error"] = error;
        result["message"] = result.value("missing").toString().isEmpty()
            ? KE_TXT("Il manque une information pour continuer.", "Some information is missing to continue.")
            : result.value("missing").toString();
    }
    return result;
}

QVariantMap AIEngine::deterministicPlan(const QString& query) {
    const QString q = query.toLower();

    if (q.contains("unknown") || q.contains("inconnue")) {
        if (q.contains("capture") || q.contains("initial")) {
            m_stateMachine.setState(AIState::WaitingForUserChange);
            return makeToolCall("unknown_capture", {}, KE_TXT("Capture initiale pour valeur inconnue.", "Initial capture for an unknown value."));
        }

        QString mode = "changed";
        if (q.contains("augment") || q.contains("increased")) mode = "increased";
        if (q.contains("diminu") || q.contains("decreased")) mode = "decreased";
        if (q.contains("pareil") || q.contains("unchanged")) mode = "unchanged";
        return makeToolCall("unknown_compare", {{"mode", mode}, {"valueType", inferValueType(query)}}, KE_TXT("Comparaison unknown initial value.", "Unknown initial value comparison."));
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
        return makeToolCall("next_scan", {{"mode", mode}, {"value", firstNumber(query)}}, KE_TXT("Réduction des candidats.", "Reducing the candidates."));
    }

    if ((q.contains("freeze") || q.contains("geler")) && !hasNegatedFreezeInstruction(q)) {
        return makeToolCall("freeze_value", {
            {"address", firstHexAddress(query)},
            {"valueType", inferValueType(query)},
            {"value", firstNumber(query)},
            {"enabled", true},
        }, KE_TXT("Freeze demandé par l'utilisateur.", "Freeze requested by the user."));
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
        }, KE_TXT("Écriture kernel demandée explicitement par l'utilisateur.", "Kernel write explicitly requested by the user."));
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
            return makeToolCall("speedhack_set", {{"mode", "off"}}, KE_TXT("Désactivation du speedhack demandée.", "Speedhack deactivation requested."));
        }
        if (wantsPause) {
            return makeToolCall("speedhack_set", {{"mode", "set"}, {"factor", 0.0}}, KE_TXT("Pause du temps demandée (speedhack).", "Time pause requested (speedhack)."));
        }
        if (wantsSlow || wantsFast || wantsSpeedGeneric) {
            double factor = wantsSlow ? 0.5 : 2.0;
            bool parsedOk = false;
            const double parsed = firstNumber(query).toDouble(&parsedOk);
            if (parsedOk && parsed > 0.0) factor = parsed;
            return makeToolCall("speedhack_set", {{"mode", "set"}, {"factor", factor}},
                wantsSlow ? KE_TXT("Ralentissement demandé (speedhack).", "Slowdown requested (speedhack).") : KE_TXT("Accélération demandée (speedhack).", "Speed-up requested (speedhack)."));
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
            return makeToolCall("block_process_network", {{"mode", "off"}}, KE_TXT("Rétablissement du réseau demandé.", "Network restoration requested."));
        }
        if (wantsNetworkOn) {
            return makeToolCall("block_process_network", {{"mode", "on"}}, KE_TXT("Coupure réseau demandée par l'utilisateur.", "Network cut requested by the user."));
        }
    }

    // Réseau — outils de diagnostic (lecture seule, exécutés directement)
    if (q.contains("connexion") || q.contains("connection") || q.contains("tcp") || q.contains("udp") || q.contains("network connection")) {
        return makeToolCall("get_process_network_connections", {}, KE_TXT("Liste des connexions réseau demandée.", "Network connections list requested."));
    }
    if (q.contains("module réseau") || q.contains("network module") || q.contains("dll réseau") || q.contains("network dll") || q.contains("wininet") || q.contains("winhttp") || q.contains("ws2_32")) {
        return makeToolCall("get_process_network_modules", {}, KE_TXT("Liste des modules réseau demandée.", "Network modules list requested."));
    }

    // Réseau — Proxy HTTP
    if (q.contains("proxy http") || q.contains("http proxy") || q.contains("intercept") || q.contains("requete http") || q.contains("http request")) {
        if (q.contains("stop") || q.contains("arrête") || q.contains("arrete") || q.contains("coupe")) {
            return makeToolCall("stop_http_proxy", {}, KE_TXT("Arrêt du proxy HTTP demandé.", "HTTP proxy stop requested."));
        }
        if (q.contains("start") || q.contains("démarre") || q.contains("demarre") || q.contains("active") || q.contains("lance")) {
            QVariantMap args;
            args["port"] = 8080;
            args["interceptHttps"] = q.contains("https");
            return makeToolCall("start_http_proxy", args, KE_TXT("Démarrage du proxy HTTP demandé.", "HTTP proxy start requested."));
        }
        if (q.contains("liste") || q.contains("list") || q.contains("voir") || q.contains("affiche")) {
            return makeToolCall("get_http_proxy_requests", {}, KE_TXT("Liste des requêtes interceptées demandée.", "Intercepted requests list requested."));
        }
        if (q.contains("modif") || q.contains("change") || q.contains("edit")) {
            return makeToolCall("modify_http_request", {{"requestId", ""}, {"newBody", ""}}, KE_TXT("Modification de requête HTTP demandée (préciser requestId et body).", "HTTP request modification requested (specify requestId and body)."));
        }
    }

    // Réseau — Spoof DNS
    if (q.contains("spoof dns") || q.contains("dns spoof") || q.contains("redirige dns") || q.contains("redirect dns") || q.contains("hosts file")) {
        if (q.contains("retir") || q.contains("restor") || q.contains("supprim") || q.contains("remove")) {
            return makeToolCall("restore_dns", {{"domain", ""}}, KE_TXT("Restauration DNS demandée (préciser le domaine).", "DNS restore requested (specify the domain)."));
        }
        return makeToolCall("spoof_dns", {{"domain", ""}, {"targetIp", "127.0.0.1"}}, KE_TXT("Spoof DNS demandé (préciser le domaine et l'IP cible).", "DNS spoof requested (specify the domain and target IP)."));
    }

    // Réseau — Lag switch
    if (q.contains("lag switch") || q.contains("lagswitch") || q.contains("retard") || q.contains("delay network") || q.contains("slow network") || q.contains("network lag")) {
        if (q.contains("stop") || q.contains("arrête") || q.contains("arrete") || q.contains("désactive") || q.contains("desactive")) {
            return makeToolCall("set_lag_switch", {{"enabled", false}, {"delayMs", 0}}, KE_TXT("Désactivation du lag switch demandée.", "Lag switch deactivation requested."));
        }
        return makeToolCall("set_lag_switch", {{"enabled", true}, {"delayMs", 1000}}, KE_TXT("Activation du lag switch demandée (délai par défaut 1000ms).", "Lag switch activation requested (default delay 1000ms)."));
    }

    if (q.contains("write") || q.contains("écri") || q.contains("mettre")) {
        return makeToolCall("write_value", {
            {"address", firstHexAddress(query)},
            {"valueType", inferValueType(query)},
            {"value", firstNumber(query)},
        }, KE_TXT("Écriture mémoire demandée.", "Memory write requested."));
    }

    const QString value = firstNumber(query);
    if (!value.isEmpty()) {
        m_stateMachine.setState(AIState::FirstScanRunning);
        return makeToolCall("exact_scan", {{"value", value}, {"valueType", inferValueType(query)}}, KE_TXT("Premier scan exact depuis une valeur détectée.", "First exact scan from a detected value."));
    }

    QVariantMap result;
    result["status"] = "needs_clarification";
    result["message"] = KE_TXT("Je n'ai pas trouvé de valeur ou d'action claire.", "I couldn't find a clear value or action.");
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
        result["message"] = KE_TXT("Attache d'abord un processus dans l'onglet Processus, puis relance ta recherche.", "First attach a process in the Process tab, then run your search again.");
        result["state"] = m_stateMachine.currentStateName();
        return result;
    }

    // Demande de conseil sur une valeur affichee, mais sans valeur concrete : a verifier
    // avant le Mode Inspecteur (wantsChangedPages ci-dessous) sinon une phrase qui cite
    // "Changed Pages" comme option parmi d'autres serait interceptee comme une commande.
    if (q.contains("valeur affich") && value.isEmpty()
        && (q.contains("comment") || q.contains("conseil") || q.contains("trouve")
            || q.contains("chercher") || q.contains("trace ui") || q.contains("changed pages"))) {
        QVariantMap result;
        result["status"] = "needs_clarification";
        result["actionStatus"] = "not_executed";
        result["message"] = KE_TXT(
            "Pour une valeur affichée, il me faut d'abord le nombre exact visible à l'écran. "
            "Ensuite je peux chercher le texte affiché (Trace UI string) ou capturer les pages modifiées avant/après une variation (Changed Pages), sans écrire ni freezer.",
            "For a displayed value, I first need the exact number visible on screen. "
            "Then I can search for the displayed text (Trace UI string) or capture the modified pages before/after a change (Changed Pages), without writing or freezing anything.");
        result["state"] = m_stateMachine.currentStateName();
        QVariantList recoveryActions;
        recoveryActions.append(QVariantMap{{"id", "trace_ui_string"}, {"label", KE_TXT("Trace UI string", "Trace UI string")}});
        recoveryActions.append(QVariantMap{{"id", "start_changed_pages_diff"}, {"label", KE_TXT("Changed Pages", "Changed Pages")}});
        result["recoveryActions"] = recoveryActions;
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
    if (wantsModuleExplorationWithoutValue(q) && numbers.isEmpty()) {
        return makeToolCall("list_process_modules", {},
            KE_TXT("Tu demandes une cible de gameplay dans les DLL/modules sans valeur affichée exploitable : je liste d'abord les modules chargés, puis il faudra donner l'XP visible ou passer par Trace UI string/Changed Pages.",
                   "You're asking for a gameplay target in the DLLs/modules without a usable displayed value: I'm first listing the loaded modules, then you'll need to give the visible XP or go through Trace UI string/Changed Pages."));
    }
    if (const auto webView2Match = matchWebView2Tool(query, context); !webView2Match.tool.isEmpty()) {
        return makeToolCall(webView2Match.tool, webView2Match.args, webView2Match.rationale);
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
    if (inspectorOrUiCopy || wantsChangedPages(q)) {
        if (numbers.size() >= 2 && (q.contains("compare") || q.contains("compar") || q.contains("maintenant")
            || q.contains("avant") || q.contains("apres") || q.contains("après"))) {
            return makeToolCall("finish_changed_pages_diff", {
                {"previousValue", numbers.at(0)},
                {"currentValue", numbers.at(1)},
            }, KE_TXT("Mode Inspecteur: je compare les pages modifiees entre l'ancienne et la nouvelle valeur affichee.",
                      "Inspector mode: I'm comparing the modified pages between the old and new displayed value."));
        }

        if (q.contains("fenetre") || q.contains("fenêtre") || q.contains("window") || q.contains("uwp") || q.contains("store")) {
            QVariantMap args;
            if (q.contains("solitaire")) {
                args["titleContains"] = "Solitaire";
                args["includeAllVisible"] = true;
            }
            return makeToolCall("read_window_text", args,
                KE_TXT("Mode Inspecteur: je verifie la fenetre visible pour synchroniser l'observation.",
                       "Inspector mode: I'm checking the visible window to synchronize the observation."));
        }

        return makeToolCall("start_changed_pages_diff", {},
            KE_TXT("Mode Inspecteur: je capture un snapshot lecture seule avant la prochaine variation.",
                   "Inspector mode: I'm capturing a read-only snapshot before the next change."));
    }

    if (scanActive && wantsModuleSourcePivot(q)) {
        return makeModuleSourcePivotResponse(m_stateMachine.currentStateName());
    }

    // Recherche active + nouvelle valeur observee => reduction plutot que nouveau scan.
    if (scanActive && !value.isEmpty()) {
        m_stateMachine.setState(AIState::Refining);
        return makeToolCall("next_scan", {{"mode", "exact"}, {"value", value}}, KE_TXT("Une recherche est deja active: je reduis les candidats avec la nouvelle valeur observee.", "A search is already active: I'm narrowing down the candidates with the newly observed value."));
    }

    // Recherche active + variation decrite sans valeur => next_scan increased/decreased/changed.
    if (scanActive && value.isEmpty() && describesVariation) {
        m_stateMachine.setState(AIState::Refining);
        return makeToolCall("next_scan", {{"mode", variationMode(q)}},
            KE_TXT("Variation decrite pendant une recherche active: je reduis les candidats par comparaison.",
                   "Variation described during an active search: I'm narrowing down the candidates by comparison."));
    }

    // Snapshot unknown capture + variation decrite => unknown_compare.
    if (unknownSnapshotActive && value.isEmpty() && describesVariation) {
        m_stateMachine.setState(AIState::Refining);
        return makeToolCall("unknown_compare", {{"mode", variationMode(q)}, {"valueType", "Auto"}},
            KE_TXT("Snapshot unknown actif: je compare avec la variation decrite.",
                   "Unknown snapshot active: I'm comparing against the described variation."));
    }

    // Intentions speciales valorisees avant le scan brut.
    if ((q.contains("freeze") || q.contains("geler")) && !hasNegatedFreezeInstruction(q)) {
        return makeToolCall("freeze_value", {
            {"address", firstHexAddress(query)},
            {"valueType", inferValueType(query)},
            {"value", value},
            {"enabled", true},
        }, KE_TXT("Freeze demande par l'utilisateur.", "Freeze requested by the user."));
    }
    const bool wantsKernel = q.contains("kernel") || q.contains("noyau");
    if (wantsKernel && (q.contains("write") || q.contains("mettre")) && !value.isEmpty()) {
        return makeToolCall("kernel_write", {
            {"address", firstHexAddress(query)},
            {"valueType", inferValueType(query)},
            {"value", value},
        }, KE_TXT("Ecriture kernel demandee explicitement par l'utilisateur.", "Kernel write explicitly requested by the user."));
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
            return makeToolCall("speedhack_set", {{"mode", "off"}}, KE_TXT("Desactivation du speedhack demandee.", "Speedhack deactivation requested."));
        }
        if (wantsPause) {
            return makeToolCall("speedhack_set", {{"mode", "set"}, {"factor", 0.0}}, KE_TXT("Pause du temps demandee (speedhack).", "Time pause requested (speedhack)."));
        }
        if (wantsSlow || wantsFast || wantsSpeedGeneric) {
            double factor = wantsSlow ? 0.5 : 2.0;
            bool parsedOk = false;
            const double parsed = value.toDouble(&parsedOk);
            if (parsedOk && parsed > 0.0) factor = parsed;
            return makeToolCall("speedhack_set", {{"mode", "set"}, {"factor", factor}},
                wantsSlow ? KE_TXT("Ralentissement demande (speedhack).", "Slowdown requested (speedhack).") : KE_TXT("Acceleration demandee (speedhack).", "Speed-up requested (speedhack)."));
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
            return makeToolCall("block_process_network", {{"mode", "off"}}, KE_TXT("Retablissement du reseau demande.", "Network restoration requested."));
        }
        if (wantsNetworkOn) {
            return makeToolCall("block_process_network", {{"mode", "on"}}, KE_TXT("Coupure reseau demandee par l'utilisateur.", "Network cut requested by the user."));
        }
    }

    // Réseau — outils de diagnostic (lecture seule)
    if (q.contains("connexion") || q.contains("connection") || q.contains("tcp") || q.contains("udp") || q.contains("network connection")) {
        return makeToolCall("get_process_network_connections", {}, KE_TXT("Liste des connexions reseau demandee.", "Network connections list requested."));
    }
    if (q.contains("module reseau") || q.contains("network module") || q.contains("dll reseau") || q.contains("network dll") || q.contains("wininet") || q.contains("winhttp") || q.contains("ws2_32")) {
        return makeToolCall("get_process_network_modules", {}, KE_TXT("Liste des modules reseau demandee.", "Network modules list requested."));
    }

    // Réseau — Proxy HTTP
    if (q.contains("proxy http") || q.contains("http proxy") || q.contains("intercept") || q.contains("requete http") || q.contains("http request")) {
        if (q.contains("stop") || q.contains("arrete") || q.contains("coupe")) {
            return makeToolCall("stop_http_proxy", {}, KE_TXT("Arret du proxy HTTP demande.", "HTTP proxy stop requested."));
        }
        if (q.contains("start") || q.contains("demarre") || q.contains("active") || q.contains("lance")) {
            QVariantMap args;
            args["port"] = 8080;
            args["interceptHttps"] = q.contains("https");
            return makeToolCall("start_http_proxy", args, KE_TXT("Demarrage du proxy HTTP demande.", "HTTP proxy start requested."));
        }
        if (q.contains("liste") || q.contains("list") || q.contains("voir") || q.contains("affiche")) {
            return makeToolCall("get_http_proxy_requests", {}, KE_TXT("Liste des requetes interceptees demandee.", "Intercepted requests list requested."));
        }
    }

    // Réseau — Spoof DNS
    if (q.contains("spoof dns") || q.contains("dns spoof") || q.contains("redirige dns") || q.contains("redirect dns") || q.contains("hosts file")) {
        if (q.contains("retir") || q.contains("restor") || q.contains("supprim") || q.contains("remove")) {
            return makeToolCall("restore_dns", {{"domain", ""}}, KE_TXT("Restauration DNS demandee (preciser le domaine).", "DNS restore requested (specify the domain)."));
        }
        return makeToolCall("spoof_dns", {{"domain", ""}, {"targetIp", "127.0.0.1"}}, KE_TXT("Spoof DNS demande (preciser le domaine et l'IP cible).", "DNS spoof requested (specify the domain and target IP)."));
    }

    // Réseau — Lag switch
    if (q.contains("lag switch") || q.contains("lagswitch") || q.contains("retard") || q.contains("delay network") || q.contains("slow network") || q.contains("network lag")) {
        if (q.contains("stop") || q.contains("arrete") || q.contains("desactive")) {
            return makeToolCall("set_lag_switch", {{"enabled", false}, {"delayMs", 0}}, KE_TXT("Desactivation du lag switch demandee.", "Lag switch deactivation requested."));
        }
        return makeToolCall("set_lag_switch", {{"enabled", true}, {"delayMs", 1000}}, KE_TXT("Activation du lag switch demandee (delai par defaut 1000ms).", "Lag switch activation requested (default delay 1000ms)."));
    }

    // PHASE 271 : Mode discret / Stealth mode - fast-paths explicites
    {
        const bool wantsStealthOff = q.contains("désactive le mode discret") || q.contains("desactive le mode discret")
            || q.contains("restaure le mode normal") || q.contains("retire le mode discret")
            || q.contains("restore stealth") || q.contains("disable stealth")
            || ((q.contains("désactiv") || q.contains("desactiv") || q.contains("restore") || q.contains("retire"))
                && (q.contains("mode discret") || q.contains("stealth")));
        const bool wantsStealthOn = !wantsStealthOff
            && (q.contains("active le mode discret") || q.contains("active le mode discret")
                || q.contains("masque killengine") || q.contains("masque le processus")
                || q.contains("jeu détecte") || q.contains("jeu detecte")
                || q.contains("anti-détection") || q.contains("anti-detection")
                || q.contains("enable stealth") || q.contains("apply stealth")
                || ((q.contains("active") || q.contains("enable"))
                    && (q.contains("mode discret") || q.contains("stealth"))));
        if (wantsStealthOff) {
            return makeToolCall("restore_stealth_mode", {}, KE_TXT("Désactivation du mode discret demandée.", "Stealth mode deactivation requested."));
        }
        if (wantsStealthOn) {
            QString profile = "default";
            if (q.contains("sc2") || q.contains("starcraft")) profile = "sc2";
            else if (q.contains("minimal")) profile = "minimal";
            return makeToolCall("apply_stealth_mode", {{"profile", profile}}, KE_TXT("Activation du mode discret demandée.", "Stealth mode activation requested."));
        }
    }
    if ((q.contains("write") || q.contains("mettre")) && !value.isEmpty()) {
        return makeToolCall("write_value", {
            {"address", firstHexAddress(query)},
            {"valueType", inferValueType(query)},
            {"value", value},
        }, KE_TXT("Ecriture memoire demandee.", "Memory write requested."));
    }

    // Ecriture de la cible sans nouvelle valeur + peu de candidats =>
    // checkpoint safe (prepare) plutot que write direct.
    if ((q.contains("écri") || q.contains("ecri") || q.contains("write") || q.contains("checkpoint")
         || q.contains("finalis") || q.contains("valide"))
        && value.isEmpty() && scanActive && candidateCount > 0 && candidateCount <= 10
        && !contextTargetValue.isEmpty()) {
        return makeToolCall("prepare_write_checkpoint", {{"value", contextTargetValue}},
            KE_TXT("Peu de candidats et valeur cible connue: je prepare le checkpoint d'ecriture (sans ecrire).",
                   "Few candidates and a known target value: I'm preparing the write checkpoint (without writing)."));
    }

    // Signalement d'echec: proposer une alternative adaptee plutot que refaire pareil.
    if (looksLikeBadTargets(q)) {
        const QVariantMap lastTurn = lastHistoryTurn();
        const QString lastOutcome = lastTurn.value("outcome").toString();
        if (!contextInitialValue.isEmpty()
            && (lastOutcome == "failed" || lastTurn.value("tool").toString() == "exact_scan")) {
            return makeToolCall("exact_scan_multi_type", {{"value", contextInitialValue}},
                KE_TXT("Les dernieres adresses ne marchent pas: je relance en multi-type pour couvrir d'autres representations.",
                       "The last addresses don't work: I'm restarting in multi-type to cover other representations."));
        }
        QVariantMap result;
        result["status"] = "needs_clarification";
        result["message"] = KE_TXT(
            "Compris, ces adresses ne sont pas les bonnes. Donne-moi une valeur observee pour relancer "
            "en multi-type, ou decris la valeur (affichee a l'ecran, chiffree, inconnue...).",
            "Got it, those addresses aren't the right ones. Give me an observed value to restart "
            "in multi-type, or describe the value (displayed on screen, encrypted, unknown...).");
        result["state"] = m_stateMachine.currentStateName();
        return result;
    }

    if (q.contains("unknown") || q.contains("inconnue")
        || (q.contains("sais pas") && (q.contains("valeur") || q.contains("vaut")))
        || (q.contains("augmente") && value.isEmpty() && !scanActive)
        || (q.contains("diminue") && value.isEmpty() && !scanActive)) {
        if (q.contains("capture") || q.contains("initial") || value.isEmpty()) {
            m_stateMachine.setState(AIState::WaitingForUserChange);
            return makeToolCall("unknown_capture", {}, KE_TXT("Capture initiale pour valeur inconnue.", "Initial capture for an unknown value."));
        }
        QString mode = "changed";
        if (q.contains("augment") || q.contains("increased")) mode = "increased";
        if (q.contains("diminu") || q.contains("decreased")) mode = "decreased";
        return makeToolCall("unknown_compare", {{"mode", mode}, {"valueType", "Auto"}}, KE_TXT("Comparaison unknown initial value.", "Unknown initial value comparison."));
    }
    // Valeur affichee a l'ecran introuvable en numerique.
    if ((q.contains("affich") || q.contains("texte")) && !value.isEmpty()) {
        return makeToolCall("trace_ui_string", {{"value", value}}, KE_TXT("Valeur affichee a l'ecran: je cherche la string UI puis ses sources.", "Value displayed on screen: I'm searching for the UI string and then its sources."));
    }
    // Valeur potentiellement chiffree/obfusquee.
    if ((q.contains("chiffr") || q.contains("obfusqu") || q.contains("crypt") || q.contains("xor")) && !value.isEmpty()) {
        QVariantMap args;
        args["value"] = value;
        args["valueType"] = inferValueType(query);
        args["mode"] = "xor";
        args["keySearchBits"] = 16;
        return makeToolCall("encrypted_scan", args, KE_TXT("Valeur possiblement chiffree: scan XOR/Add/Sub borne.", "Possibly encrypted value: bounded XOR/Add/Sub scan."));
    }

    if (!value.isEmpty()) {
        if (scanActive && candidateCount > 0) {
            m_stateMachine.setState(AIState::Refining);
            return makeToolCall("next_scan", {{"mode", "exact"}, {"value", value}}, KE_TXT("Recherche active avec candidats: reduction avec la nouvelle valeur.", "Active search with candidates: narrowing down with the new value."));
        }
        m_stateMachine.setState(AIState::FirstScanRunning);
        return makeToolCall("exact_scan", {{"value", value}, {"valueType", inferValueType(query)}}, KE_TXT("Premier scan exact depuis une valeur detectee.", "First exact scan from a detected value."));
    }

    // Aucune valeur: objectifs complets ou guidance plutot que message brut.
    if (q.contains("trouve") || q.contains("cherche") || q.contains("objectif") || q.contains("guide")) {
        return makeToolCall("auto_resolve", {{"query", query}}, KE_TXT("Objectif complet sans valeur directe: mini-boucle safe Auto.", "Full goal without a direct value: safe Auto mini-loop."));
    }

    QVariantMap result;
    result["status"] = "needs_clarification";
    result["message"] = KE_TXT(
        "Je n'ai pas trouve de valeur ou d'action claire. Donne-moi la valeur affichee (ex: 41250), decris ce que tu cherches (ca augmente quand...), ou colle une adresse 0x....",
        "I couldn't find a clear value or action. Give me the displayed value (e.g. 41250), describe what you're looking for (it increases when...), or paste a 0x... address.");
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
