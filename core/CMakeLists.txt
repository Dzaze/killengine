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
    process/process_suspend.cpp

    # Optional kernel driver bridge (probe/capabilities only)
    kernel/kernel_driver_bridge.h
    kernel/kernel_driver_bridge.cpp

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
    scanner/memory_window_search.h
    scanner/memory_window_search.cpp
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

    # Pointer chains (Phase 14 — jeux modernes / applications dynamiques)
    pointer/pointer_chain.cpp
    pointer/pointer_scanner.cpp

    # Debug / Hardware breakpoints (Phase 17 — Find What Writes)
    debug/hardware_breakpoint.h
    debug/hardware_breakpoint.cpp

    # Patch / AOB signatures (Phase 18 — trainer engine)
    patch/aob_scanner.h
    patch/aob_scanner.cpp
    patch/code_patch.h
    patch/code_patch.cpp
    patch/instruction_patch_suggester.h
    patch/instruction_patch_suggester.cpp
    patch/profile_patch_state.h
    patch/profile_patch_state.cpp

    # Phase 19 — Freeze par hardware breakpoint
    debug/breakpoint_freeze.h
    debug/breakpoint_freeze.cpp

    # Phase 19 — Page Guards (sans canal de debug Win32)
    debug/page_guard.h
    debug/page_guard.cpp

    # Roadmap section F, niveau 2 — Breakpoint matériel in-process (DR0-DR7
    # posés depuis un composant chargé dans la cible, sans dépendre du canal
    # de debug Win32 DebugActiveProcess)
    debug/inprocess_breakpoint.h
    debug/inprocess_breakpoint.cpp

    # Propriétaire logique unique des registres de debug (DR0-DR7) par PID,
    # partagé entre hardware_breakpoint.cpp, breakpoint_freeze.cpp et
    # inprocess_breakpoint.cpp — incident du 19-20/08/2026, voir
    # docs/STRATEGY_ROOM.md
    debug/breakpoint_arbiter.h
    debug/breakpoint_arbiter.cpp

    # Phase 19 — Scan de valeurs chiffrées/obfusquées
    scanner/encrypted_scan.h
    scanner/encrypted_scan.cpp

    # Phase 19 — Analyseur de structures
    scanner/structure_analyzer.h
    scanner/structure_analyzer.cpp

    # Phase 20 — Injection DLL + Hooking
    inject/dll_injector.h
    inject/dll_injector.cpp
    inject/function_hook.h
    inject/function_hook.cpp

    # Phase 20 — Auto-assembler
    scripting/auto_assembler.h
    scripting/auto_assembler.cpp

    # Phase 20 — Hotkeys globales
    input/global_hotkey.h
    input/global_hotkey.cpp
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

if(TARGET Zydis::Zydis)
    target_link_libraries(killcore PUBLIC Zydis::Zydis)
    target_compile_definitions(killcore PUBLIC KILLENGINE_HAS_ZYDIS=1)
elseif(TARGET Zydis)
    target_link_libraries(killcore PUBLIC Zydis)
    target_compile_definitions(killcore PUBLIC KILLENGINE_HAS_ZYDIS=1)
endif()

# ---------------------------------------------------------------------------
# Phase 19 — Page Guard : DLL injectee dans le processus cible
#
# Volontairement une DLL a part, SANS lien vers killcore/Qt : elle est chargee
# dans le processus surveille (le jeu), pas dans KillEngine.exe. Une
# dependance Qt ici forcerait le processus cible a resoudre les DLL Qt de
# KillEngine, risque de charge/echec inutile dans un processus qu'on ne
# controle pas. Voir core/debug/page_guard_handler/page_guard_handler.cpp.
# ---------------------------------------------------------------------------
add_library(KillEnginePageGuardHandler SHARED
    debug/page_guard_ipc.h
    debug/page_guard_handler/page_guard_handler.cpp
)
target_include_directories(KillEnginePageGuardHandler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(KillEnginePageGuardHandler PROPERTIES OUTPUT_NAME "KillEnginePageGuardHandler")
if(WIN32)
    target_compile_definitions(KillEnginePageGuardHandler PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

# ---------------------------------------------------------------------------
# Roadmap section F, niveau 2 — Breakpoint matériel in-process : DLL injectee
# dans le processus cible, meme raisonnement que KillEnginePageGuardHandler
# ci-dessus (SANS lien vers killcore/Qt). Voir
# core/debug/inprocess_breakpoint_handler/inprocess_breakpoint_handler.cpp.
# ---------------------------------------------------------------------------
add_library(KillEngineInProcessBreakpointHandler SHARED
    debug/inprocess_breakpoint_ipc.h
    debug/inprocess_breakpoint_handler/inprocess_breakpoint_handler.cpp
)
target_include_directories(KillEngineInProcessBreakpointHandler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(KillEngineInProcessBreakpointHandler PROPERTIES OUTPUT_NAME "KillEngineInProcessBreakpointHandler")
if(WIN32)
    target_compile_definitions(KillEngineInProcessBreakpointHandler PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()
