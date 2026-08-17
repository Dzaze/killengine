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
        makeTool("unknown_capture", "Capture un snapshot unknown initial borne.", {}),
        makeTool("unknown_compare", "Compare le snapshot unknown initial apres variation utilisateur.", {"mode", "valueType"}),
        makeTool("prepare_write_checkpoint", "Prepare des candidats pour ecriture confirmee, sans ecrire.", {"value"}, "write", true),
        makeTool("write_value", "Écrit une valeur typée à une adresse apres confirmation explicite.", {"address", "valueType", "value"}, "write", true),
        makeTool("freeze_value", "Active ou désactive un freeze apres confirmation explicite.", {"address", "valueType", "value", "enabled"}, "write", true),
        makeTool("find_what_writes", "Capture l'instruction qui ecrit une adresse apres confirmation explicite.", {"address", "size"}, "debug", true),
        makeTool("generate_aob", "Genere une signature AOB depuis une instruction confirmee.", {"address"}, "patch", true),
        makeTool("suggest_patch", "Suggere un patch code sans application automatique.", {"address"}, "patch", true),
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
