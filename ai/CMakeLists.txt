# =============================================================================
# KillAI — Couche IA (llama.cpp + Qwen + tool calling)
# =============================================================================

# Phase 0: stubs uniquement
# Phase 9: intégration réelle de llama.cpp

add_library(killai STATIC
    ai_engine.cpp
    query_text_utils.cpp
    intent_contract.cpp
    llama_runtime.cpp
    llama_server.cpp
    model_locator.cpp
    tool_registry.cpp
    tool_validator.cpp
    state_machine.cpp
    auto_resolver.cpp
    investigation_notebook.cpp
)

target_include_directories(killai PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(killai PUBLIC
    killcore
    Qt6::Core
    Qt6::Network
    nlohmann_json::nlohmann_json
)
