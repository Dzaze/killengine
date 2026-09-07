#include "anthropic_tool_schema.h"

#include <QJsonObject>
#include <QVariantMap>

namespace killai {

QJsonArray toolsToAnthropicSchema(const QVariantList& tools) {
    QJsonArray result;

    for (const auto& toolVariant : tools) {
        const QVariantMap tool = toolVariant.toMap();

        QJsonObject properties;
        QJsonArray required;
        const QVariantList args = tool.value("args").toList();
        for (const auto& argVariant : args) {
            const QVariantMap argMap = argVariant.toMap();
            const QString argName = argMap.value("name").toString();
            if (argName.isEmpty()) {
                continue;
            }

            QJsonObject property;
            property["type"] = argMap.value("type").toString();
            property["description"] = argMap.value("description").toString();
            properties[argName] = property;
            required.append(argName);
        }

        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        inputSchema["properties"] = properties;
        if (!required.isEmpty()) {
            inputSchema["required"] = required;
        }

        QJsonObject toolObject;
        toolObject["name"] = tool.value("name").toString();
        toolObject["description"] = tool.value("description").toString();
        toolObject["input_schema"] = inputSchema;

        result.append(toolObject);
    }

    return result;
}

} // namespace killai
