# =============================================================================
# KillCore — Moteur déterministe C++20 (mémoire, scanner, candidats)
# =============================================================================

# ---------------------------------------------------------------------------
# Logging (utilisé partout)
# ---------------------------------------------------------------------------
add_library(killcore_logging STATIC
    logging/logger.cpp
)
target_include_directories(killcore_logging PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(killcore_logging PUBLIC Qt6::Core)

# ---------------------------------------------------------------------------
# KillCore — bibliothèque principale
# Phase 0: stubs et fondation uniquement
# Les sous-modules seront ajoutés au fur et à mesure des phases
# ---------------------------------------------------------------------------
add_library(killcore STATIC
    # Process
    process/process_enumerator.cpp
    process/process_handle.cpp
    process/architecture.cpp

    # Memory
    memory/memory_map.cpp
    memory/memory_reader.cpp
    memory/memory_writer.cpp
    memory/memory_region.cpp

    # Scanner (stubs Phase 0)
    scanner/scan_engine.cpp
    scanner/scan_types.cpp
    scanner/value_variants.cpp
    scanner/display_value_tracker.cpp
    scanner/candidate_confidence.cpp
    scanner/performance_profile.cpp
    scanner/worker_pool.cpp

    # Candidates (stubs Phase 0)
    candidates/candidate_store.cpp

    # Snapshot (stubs Phase 0)
    snapshot/snapshot_codec.cpp
    snapshot/snapshot_store.cpp

    # Freeze (stubs Phase 0)
    freeze/freeze_manager.cpp

    # Profiles (stubs Phase 0)
    profiles/profile_store.cpp
    profiles/locator.cpp

    # Pointer chains (Phase 14 — StarCraft 2 / jeux modernes)
    pointer/pointer_chain.cpp
    pointer/pointer_scanner.cpp

    # Debug / Hardware breakpoints (Phase 17 — Find What Writes)
    debug/hardware_breakpoint.h
    debug/hardware_breakpoint.cpp

    # Patch / AOB signatures (Phase 18 — trainer engine)
    patch/aob_scanner.h
    patch/aob_scanner.cpp
)

target_include_directories(killcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(killcore PUBLIC
    killcore_logging
    Qt6::Core
    nlohmann_json::nlohmann_json
    lz4_static
    # Windows libraries
    psapi
    dbghelp
)
