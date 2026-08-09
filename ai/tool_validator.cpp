#include "tool_validator.h"

namespace killai {

ToolValidator::ToolValidator(QObject* parent)
    : QObject(parent) {
}

bool ToolValidator::validate(const QVariantMap& toolCall, QString* error) const {
    const QString tool = toolCall.value("tool").toString();
    if (tool.isEmpty()) {
        if (error) *error = "Tool call missing 'tool'.";
        return false;
    }

    if (!m_registry.hasTool(tool)) {
        if (error) *error = QString("Unknown tool '%1'.").arg(tool);
        return false;
    }

    const QVariant argsValue = toolCall.value("args");
    if (!argsValue.canConvert<QVariantMap>()) {
        if (error) *error = "Tool call missing object 'args'.";
        return false;
    }

    const QVariantMap args = argsValue.toMap();
    for (const auto& required : m_registry.requiredArgs(tool)) {
        if (!args.contains(required)) {
            if (error) *error = QString("Tool '%1' missing arg '%2'.").arg(tool, required);
            return false;
        }
    }

    return true;
}

} // namespace killai
