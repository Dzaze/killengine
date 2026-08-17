# Ajouter les tests du contexte IA a test_ai_tools.cpp
$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding($false)

$path = 'tests\unit\test_ai_tools.cpp'
$content = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)

if ($content.Contains('ContextualFallback')) { Write-Host 'Tests already present'; exit 0 }

$newTests = @'

// --- Contexte de session : le fallback deterministe choisit le bon outil ---

TEST(AIEngineContextualFallbackTest, AsksToAttachProcessWhenDetached) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    QVariantMap context;
    context["processAttached"] = false;
    const auto result = engine.processQuery("cherche 41250", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "needs_clarification");
    EXPECT_TRUE(result.value("message").toString().contains("processus"));
}

TEST(AIEngineContextualFallbackTest, RefinesWithNextScanWhenScanActive) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    QVariantMap context;
    context["processAttached"] = true;
    context["scanActive"] = true;
    context["candidateCount"] = static_cast<qulonglong>(120);
    const auto result = engine.processQuery("maintenant c'est 812", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "next_scan");
    EXPECT_EQ(result.value("args").toMap().value("mode").toString().toStdString(), "exact");
}

TEST(AIEngineContextualFallbackTest, ExactScanWhenNoActiveSearch) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    QVariantMap context;
    context["processAttached"] = true;
    context["scanActive"] = false;
    const auto result = engine.processQuery("j'ai 41250 argent", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "exact_scan");
}

TEST(AIEngineContextualFallbackTest, TraceUiStringForDisplayedValue) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("la valeur 60 est affichee mais introuvable", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "trace_ui_string");
    EXPECT_EQ(result.value("args").toMap().value("value").toString().toStdString(), "60");
}

TEST(AIEngineContextualFallbackTest, EncryptedScanForObfuscatedValue) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("la valeur 500 semble chiffree", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "encrypted_scan");
    EXPECT_EQ(result.value("args").toMap().value("mode").toString().toStdString(), "xor");
}

TEST(AIEngineContextualFallbackTest, UnknownCaptureWhenValueUnknown) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    QVariantMap context;
    context["processAttached"] = true;
    context["scanActive"] = false;
    const auto result = engine.processQuery("je ne sais pas la valeur, elle augmente quand je gagne", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "unknown_capture");
}

TEST(AIEngineContextualFallbackTest, AutoResolveForGuidedObjectiveWithoutValue) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("trouve cette valeur et guide-moi", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "auto_resolve");
}

TEST(AIEngineContextualFallbackTest, LegacyOverloadStillPlansExactScan) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    const auto result = engine.processQuery("j'ai 41250 argent");
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "exact_scan");
}
'@
$newTests = $newTests.Replace("`r`n", "`n")
$content = $content + $newTests
[System.IO.File]::WriteAllText($path, $content, $utf8)
Write-Host 'tests patched'