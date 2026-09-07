#pragma once

#include <QJsonArray>
#include <QVariantList>

namespace killai {

// Convertit le format interne de ToolRegistry::availableTools() (QVariantList
// de QVariantMap avec un champ "args" liste de {name,type,description}) en
// tableau `tools` au format attendu par l'API Messages Anthropic
// (name/description/input_schema JSON Schema). Tous les arguments du
// registre interne sont obligatoires (voir tool_registry.cpp) : chacun
// apparaît donc à la fois dans "properties" et "required" du schéma généré.
QJsonArray toolsToAnthropicSchema(const QVariantList& tools);

} // namespace killai
