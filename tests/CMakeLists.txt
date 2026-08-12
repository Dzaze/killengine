# =============================================================================
# KillEngine — Tests (Google Test)
# =============================================================================

# Unit tests
add_executable(killengine_unit_tests
    unit/test_main.cpp
    unit/test_value_parser.cpp
    unit/test_value_variants.cpp
    unit/test_logger.cpp
    unit/test_candidate_store.cpp
    unit/test_ai_tools.cpp
    unit/test_snapshot_codec.cpp
    unit/test_pointer_chain.cpp
    unit/test_performance_profile.cpp
    unit/test_worker_pool.cpp
)

target_link_libraries(killengine_unit_tests PRIVATE
    killcore
    killai
    GTest::gtest
    GTest::gtest_main
    Qt6::Core
)

include(GoogleTest)
gtest_discover_tests(killengine_unit_tests DISCOVERY_MODE PRE_TEST)

# =============================================================================
# KillEngineTestTarget — Application de test autonome
# =============================================================================
# Expose des variables mémoire connues pour validation manuelle des scans.
# Voir KILLENGINE_PROJECT_SPEC.md sections 69-72.
add_executable(KillEngineTestTarget
    memory_targets/test_target_main.cpp
)
target_link_libraries(KillEngineTestTarget PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)
set_target_properties(KillEngineTestTarget PROPERTIES
    AUTOMOC ON
    WIN32_EXECUTABLE ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Integration tests (Phase 13)
add_executable(killengine_integration_tests
    integration/test_main.cpp
    integration/test_memory_scan.cpp
)
target_link_libraries(killengine_integration_tests PRIVATE
    killcore
    GTest::gtest
    Qt6::Core
)
set_target_properties(killengine_integration_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_dependencies(killengine_integration_tests KillEngineTestTarget)
gtest_discover_tests(killengine_integration_tests DISCOVERY_MODE PRE_TEST)
