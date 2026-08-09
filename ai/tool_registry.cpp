#include "tool_registry.h"

namespace killai {

namespace {

QVariantMap makeTool(const QString& name, const QString& description, const QStringList& requiredArgs) {
    QVariantMap tool;
    tool["name"] = name;
    tool["description"] = description;
    tool["requiredArgs"] = requiredArgs;
    return tool;
}

QVariantList toolDefinitions() {
    return {
        makeTool("exact_scan", "Scan exact sur le processus attaché.", {"value", "valueType"}),
        makeTool("next_scan", "Réduit les candidats existants.", {"mode"}),
        makeTool("unknown_capture", "Capture un snapshot unknown initial.", {}),
        makeTool("unknown_compare", "Compare le snapshot unknown initial.", {"mode", "valueType"}),
        makeTool("write_value", "Écrit une valeur typée à une adresse.", {"address", "valueType", "value"}),
        makeTool("freeze_value", "Active ou désactive un freeze.", {"address", "valueType", "value", "enabled"}),
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

} // namespace killai
