#include <gtest/gtest.h>

#include "query_text_utils.h"

TEST(QueryTextUtilsStandaloneTest, ExtractsFirstDecimalOutsideHex) {
    EXPECT_EQ(killai::firstDecimalOutsideHex("salut cherche 41250").toStdString(), "41250");
    EXPECT_EQ(killai::firstDecimalOutsideHex("valeur -12,5 autour de 0x1234").toStdString(), "-12.5");
    EXPECT_TRUE(killai::firstDecimalOutsideHex("adresse 0x1234 uniquement").isEmpty());
}

TEST(QueryTextUtilsStandaloneTest, ExtractsFirstHexAddress) {
    EXPECT_EQ(killai::firstHexAddressIn("capture ce qui ecrit 0x1A2b3C").toStdString(), "0x1A2b3C");
    EXPECT_TRUE(killai::firstHexAddressIn("aucune adresse ici").isEmpty());
}

TEST(QueryTextUtilsStandaloneTest, SocialPredicateRejectsActionableMessages) {
    EXPECT_TRUE(killai::looksLikePureSocialQuery("Salut mon pote !"));
    EXPECT_TRUE(killai::looksLikePureSocialQuery("merci beaucoup"));
    EXPECT_FALSE(killai::looksLikePureSocialQuery("salut cherche 41250"));
    EXPECT_FALSE(killai::looksLikePureSocialQuery("merci 0x12345"));
    EXPECT_FALSE(killai::looksLikePureSocialQuery(""));
}

TEST(QueryTextUtilsStandaloneTest, TrainerPredicateCoversAllSharedBypassPhrases) {
    EXPECT_TRUE(killai::wantsTrainerQuery("liste le trainer"));
    EXPECT_TRUE(killai::wantsTrainerQuery("ouvre la cheat table"));
    EXPECT_TRUE(killai::wantsTrainerQuery("ajoute une feature trainer"));
    EXPECT_TRUE(killai::wantsTrainerQuery("ajoute une fonction trainer"));
    EXPECT_FALSE(killai::wantsTrainerQuery("cherche 100"));
}

TEST(QueryTextUtilsStandaloneTest, FieldStabilityPredicateCoversFrenchAndEnglish) {
    EXPECT_TRUE(killai::wantsFieldStabilityQuery("est-ce une valeur affichee ?"));
    EXPECT_TRUE(killai::wantsFieldStabilityQuery("est-ce une vraie source ?"));
    EXPECT_TRUE(killai::wantsFieldStabilityQuery("check field stability"));
    EXPECT_TRUE(killai::wantsFieldStabilityQuery("derived display field"));
    EXPECT_FALSE(killai::wantsFieldStabilityQuery("scan exact 100"));
}

TEST(QueryTextUtilsStandaloneTest, UiSourcesPredicateCoversFrenchAndEnglish) {
    EXPECT_TRUE(killai::wantsUiSourcesQuery("analyse les sources numeriques"));
    EXPECT_TRUE(killai::wantsUiSourcesQuery("analyse la source"));
    EXPECT_TRUE(killai::wantsUiSourcesQuery("analyze the sources"));
    EXPECT_TRUE(killai::wantsUiSourcesQuery("numeric source"));
    EXPECT_FALSE(killai::wantsUiSourcesQuery("trace le texte 100"));
}

TEST(QueryTextUtilsStandaloneTest, AobPatchWorkflowAggregatesAllReadOnlyPatchPredicates) {
    EXPECT_TRUE(killai::wantsGenerateAobQuery("genere une signature aob pour 0x1234"));
    EXPECT_TRUE(killai::wantsSuggestPatchQuery("suggere un patch pour 0x1234"));
    EXPECT_TRUE(killai::wantsDisassembleBackwardQuery("desassemble en arriere 0x1234"));
    EXPECT_TRUE(killai::wantsAobOrPatchWorkflowQuery("generate a signature for 0x1234"));
    EXPECT_TRUE(killai::wantsAobOrPatchWorkflowQuery("patch suggestions for 0x1234"));
    EXPECT_TRUE(killai::wantsAobOrPatchWorkflowQuery("source fields around 0x1234"));
    EXPECT_FALSE(killai::wantsAobOrPatchWorkflowQuery("freeze 0x1234 a 100"));
}

TEST(QueryTextUtilsStandaloneTest, DebugWorkflowAggregatesFindWritesAndCandidateFieldTests) {
    EXPECT_TRUE(killai::wantsFindWhatWritesQuery("capture ce qui ecrit 0x1234"));
    EXPECT_TRUE(killai::wantsFindWhatWritesQuery("what writes to 0x1234"));
    EXPECT_TRUE(killai::wantsTestCandidateFieldsQuery("teste les champs candidats"));
    EXPECT_TRUE(killai::wantsTestCandidateFieldsQuery("test the candidate fields"));
    EXPECT_TRUE(killai::wantsFindWhatWritesOrTestFieldsQuery("qui ecrit cette adresse 0x1234"));
    EXPECT_TRUE(killai::wantsFindWhatWritesOrTestFieldsQuery("verifie quel champ tient"));
    EXPECT_FALSE(killai::wantsFindWhatWritesOrTestFieldsQuery("affiche les processus"));
}

TEST(QueryTextUtilsStandaloneTest, InvestigationPlaybookMatchesOnlyBroadReadOnlySymptoms) {
    EXPECT_EQ(killai::investigationPlaybookTopic("je ne trouve pas la valeur affichée").toStdString(),
        "displayed_value_not_found");
    EXPECT_EQ(killai::investigationPlaybookTopic("le freeze clignote et ne tient pas").toStdString(),
        "freeze_flickers");
    EXPECT_EQ(killai::investigationPlaybookTopic("cette adresse ne survit pas au redemarrage").toStdString(),
        "unstable_address");
    EXPECT_EQ(killai::investigationPlaybookTopic("je veux patcher le code proprement").toStdString(),
        "code_patch_request");
    EXPECT_EQ(killai::investigationPlaybookTopic("qui écrit cette valeur ?").toStdString(),
        "what_writes_value");

    EXPECT_TRUE(killai::investigationPlaybookTopic("la valeur 60 est affichee mais introuvable").isEmpty());
    EXPECT_TRUE(killai::investigationPlaybookTopic("capture ce qui ecrit 0x1234").isEmpty());
    EXPECT_TRUE(killai::investigationPlaybookTopic("suggere un patch pour 0x1234").isEmpty());
}
