// PROPOSITIONS-1 #4 (Live Lua REPL) — tests de la logique pure du protocole
// (aucun process lua.exe requis) : decoupage buffer/sentinelle et extraction
// de l'autocompletion ke.* depuis le texte du helper.

#include "scripting/lua_repl_protocol.h"

#include <gtest/gtest.h>

#include <QByteArray>

using killcore::extractKeCompletions;
using killcore::extractReplOutput;

TEST(LuaReplProtocolTest, ExtractReplOutputNotFoundWhenSentinelMissing) {
    const auto result = extractReplOutput(QByteArray("2\n"), QByteArray("\1KE_REPL_END\1"));
    EXPECT_FALSE(result.found);
    EXPECT_TRUE(result.output.isEmpty());
    EXPECT_TRUE(result.remaining.isEmpty());
}

TEST(LuaReplProtocolTest, ExtractReplOutputSplitsAtSentinel) {
    const QByteArray sentinel("\1KE_REPL_END\1\n");
    const QByteArray buffer = "2\n" + sentinel;
    const auto result = extractReplOutput(buffer, sentinel);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.output, "2\n");
    EXPECT_TRUE(result.remaining.isEmpty());
}

TEST(LuaReplProtocolTest, ExtractReplOutputKeepsLeftoverBytesAfterSentinel) {
    const QByteArray sentinel("\1KE_REPL_END\1\n");
    // Deux reponses arrivees dans le meme read() -- le reliquat doit etre
    // conserve pour la prochaine commande, pas jete.
    const QByteArray buffer = "first\n" + sentinel + "second-partial";
    const auto result = extractReplOutput(buffer, sentinel);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.output, "first\n");
    EXPECT_EQ(result.remaining, QByteArray("second-partial"));
}

TEST(LuaReplProtocolTest, ExtractReplOutputHandlesEmptyCommandOutput) {
    const QByteArray sentinel("\1KE_REPL_END\1\n");
    const auto result = extractReplOutput(sentinel, sentinel);
    ASSERT_TRUE(result.found);
    EXPECT_TRUE(result.output.isEmpty());
}

TEST(LuaReplProtocolTest, ExtractKeCompletionsFindsAllWrapperFunctions) {
    const QString source = R"LUA(
local ke = {}
function ke.ping(message)
  return ke.call("ping", { message or "lua" })
end

function ke.attach(pid)
  return ke.call("attachProcess", { tonumber(pid) })
end

local function helper_not_exposed()
end

function ke.scan_exact(value, value_type)
  return ke.call("startExactScan", { tostring(value), value_type or "Int32" })
end
return ke
)LUA";

    const auto completions = extractKeCompletions(source);
    EXPECT_EQ(completions.size(), 3);
    EXPECT_TRUE(completions.contains("ke.ping"));
    EXPECT_TRUE(completions.contains("ke.attach"));
    EXPECT_TRUE(completions.contains("ke.scan_exact"));
    EXPECT_FALSE(completions.contains("ke.helper_not_exposed"));
    // Trie alphabetiquement.
    ASSERT_EQ(completions.size(), 3);
    EXPECT_TRUE(completions[0] <= completions[1]);
    EXPECT_TRUE(completions[1] <= completions[2]);
}

TEST(LuaReplProtocolTest, ExtractKeCompletionsFiltersByPrefixCaseInsensitive) {
    const QString source = R"LUA(
function ke.scan_exact(v) end
function ke.scan_exact_table(v) end
function ke.attach(pid) end
)LUA";

    const auto scanOnly = extractKeCompletions(source, "ke.SCAN");
    EXPECT_EQ(scanOnly.size(), 2);
    EXPECT_TRUE(scanOnly.contains("ke.scan_exact"));
    EXPECT_TRUE(scanOnly.contains("ke.scan_exact_table"));
    EXPECT_FALSE(scanOnly.contains("ke.attach"));
}

TEST(LuaReplProtocolTest, ExtractKeCompletionsDeduplicates) {
    const QString source = R"LUA(
function ke.ping(message) end
function ke.ping(message) end
)LUA";
    const auto completions = extractKeCompletions(source);
    EXPECT_EQ(completions.size(), 1);
}

TEST(LuaReplProtocolTest, ExtractKeCompletionsEmptySourceReturnsEmpty) {
    EXPECT_TRUE(extractKeCompletions("").isEmpty());
}
