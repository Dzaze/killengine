#include <gtest/gtest.h>

#include "anthropic_tool_schema.h"
#include "tool_registry.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

TEST(AnthropicToolSchemaTest, ProducesOneEntryPerRegisteredTool) {
    killai::ToolRegistry registry;
    const QJsonArray schema = killai::toolsToAnthropicSchema(registry.availableTools());

    EXPECT_EQ(schema.size(), registry.availableTools().size());
}

TEST(AnthropicToolSchemaTest, ToolWithArgsGetsTypedPropertiesAndRequired) {
    killai::ToolRegistry registry;
    const QJsonArray schema = killai::toolsToAnthropicSchema(registry.availableTools());

    bool found = false;
    for (const auto& entry : schema) {
        const QJsonObject tool = entry.toObject();
        if (tool.value("name").toString() != "write_value") {
            continue;
        }
        found = true;

        EXPECT_FALSE(tool.value("description").toString().isEmpty());

        const QJsonObject inputSchema = tool.value("input_schema").toObject();
        EXPECT_EQ(inputSchema.value("type").toString(), "object");

        const QJsonObject properties = inputSchema.value("properties").toObject();
        ASSERT_TRUE(properties.contains("address"));
        ASSERT_TRUE(properties.contains("valueType"));
        ASSERT_TRUE(properties.contains("value"));
        EXPECT_EQ(properties.value("address").toObject().value("type").toString(), "string");
        EXPECT_FALSE(properties.value("address").toObject().value("description").toString().isEmpty());

        const QJsonArray required = inputSchema.value("required").toArray();
        QSet<QString> requiredNames;
        for (const auto& r : required) {
            requiredNames.insert(r.toString());
        }
        EXPECT_TRUE(requiredNames.contains("address"));
        EXPECT_TRUE(requiredNames.contains("valueType"));
        EXPECT_TRUE(requiredNames.contains("value"));
    }
    EXPECT_TRUE(found);
}

TEST(AnthropicToolSchemaTest, ToolWithoutArgsGetsEmptyPropertiesAndNoRequired) {
    killai::ToolRegistry registry;
    const QJsonArray schema = killai::toolsToAnthropicSchema(registry.availableTools());

    bool found = false;
    for (const auto& entry : schema) {
        const QJsonObject tool = entry.toObject();
        if (tool.value("name").toString() != "get_auto_report") {
            continue;
        }
        found = true;

        const QJsonObject inputSchema = tool.value("input_schema").toObject();
        EXPECT_EQ(inputSchema.value("type").toString(), "object");
        EXPECT_TRUE(inputSchema.value("properties").toObject().isEmpty());
        EXPECT_FALSE(inputSchema.contains("required"));
    }
    EXPECT_TRUE(found);
}

TEST(AnthropicToolSchemaTest, BooleanAndNumericArgsKeepTheirJsonSchemaType) {
    killai::ToolRegistry registry;
    const QJsonArray schema = killai::toolsToAnthropicSchema(registry.availableTools());

    bool found = false;
    for (const auto& entry : schema) {
        const QJsonObject tool = entry.toObject();
        if (tool.value("name").toString() != "freeze_value") {
            continue;
        }
        found = true;

        const QJsonObject properties = tool.value("input_schema").toObject().value("properties").toObject();
        ASSERT_TRUE(properties.contains("enabled"));
        EXPECT_EQ(properties.value("enabled").toObject().value("type").toString(), "boolean");
    }
    EXPECT_TRUE(found);

    found = false;
    for (const auto& entry : schema) {
        const QJsonObject tool = entry.toObject();
        if (tool.value("name").toString() != "start_http_proxy") {
            continue;
        }
        found = true;

        const QJsonObject properties = tool.value("input_schema").toObject().value("properties").toObject();
        ASSERT_TRUE(properties.contains("port"));
        EXPECT_EQ(properties.value("port").toObject().value("type").toString(), "integer");
    }
    EXPECT_TRUE(found);
}
