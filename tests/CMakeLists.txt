# =============================================================================
# KillEngine — Tests (Google Test)
# =============================================================================

# Unit tests
add_executable(killengine_unit_tests
    unit/test_main.cpp
    unit/test_value_parser.cpp
    unit/test_value_variants.cpp
    unit/test_ui_string_tracker.cpp
    unit/test_aob_scanner.cpp
    unit/test_logger.cpp
    unit/test_candidate_store.cpp
    unit/test_ai_tools.cpp
    unit/test_query_text_utils.cpp
    unit/test_ai_adaptive.cpp
    unit/test_auto_resolver.cpp
    unit/test_snapshot_codec.cpp
    unit/test_pointer_chain.cpp
    unit/test_performance_profile.cpp
    unit/test_worker_pool.cpp
    unit/test_power_up_modules.cpp
    unit/test_breakpoint_arbiter.cpp
    unit/test_kernel_driver_bridge.cpp
    unit/test_speedhack_clock.cpp
    unit/test_package_storage.cpp
    unit/test_file_watch.cpp
    unit/test_display_source_classifier.cpp
    unit/test_profile_store.cpp
    unit/test_ghidra_bridge.cpp
    unit/test_investigation_notebook.cpp
    unit/test_investigation_notebook_planner.cpp
    unit/test_changed_pages_consensus.cpp
    unit/test_page_diff_analyzer.cpp
    unit/test_smart_watchdog.cpp
    unit/test_lua_repl_protocol.cpp
    unit/test_memory_timeline_analyzer.cpp
    unit/test_memory_heatmap_collector.cpp
    unit/test_memory_timeline_collector.cpp
    unit/test_pattern_learning_engine.cpp
    unit/test_stealth_profiler.cpp
    unit/test_feature_extractor.cpp
    unit/test_game_profile_database.cpp
    unit/test_auto_dissect.cpp
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

# Integration tests (Phase 13 + power-up runtime)
add_executable(killengine_integration_tests
    integration/test_main.cpp
    integration/test_memory_scan.cpp
    integration/test_power_up_runtime.cpp
    integration/test_breakpoint_arbiter_runtime.cpp
    integration/test_debug_detach_reproducer.cpp
    integration/test_display_vs_source_target.cpp
    integration/test_pointer_scanner_runtime.cpp
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

# =============================================================================
# KillEngineBenchmark — Benchmark de performance (Phase 15)
# =============================================================================
# Exécutable autonome qui mesure scan exact, scan multi-type, unknown snapshot
# et gros volumes de candidats. Voir docs/PERFORMANCE_BENCHMARKS.md.
add_executable(KillEngineBenchmark
    benchmarks/benchmark_main.cpp
)
target_link_libraries(KillEngineBenchmark PRIVATE
    killcore
    Qt6::Core
)
set_target_properties(KillEngineBenchmark PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_dependencies(KillEngineBenchmark KillEngineTestTarget)
