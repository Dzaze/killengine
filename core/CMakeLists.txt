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
    process/process_mask.h
    process/process_mask.cpp

    # Roadmap section I — Résolution de symbole par nom (table d'export PE lue
    # à distance, pour hooker/breaker une fonction connue sans passer par Find
    # What Writes)
    process/export_resolver.h
    process/export_resolver.cpp

    # PHASE 91 — Découverte des fichiers de sauvegarde/état d'un process UWP
    # (dossier %LOCALAPPDATA%\Packages\<familyName>\), pour proposer une
    # investigation "hors mémoire" à l'IA embarquée après PHASE 90.
    process/package_storage.h
    process/package_storage.cpp

    # PHASE 93 — Surveillance native (ReadDirectoryChangesW) d'un fichier de
    # sauvegarde/état, remplace l'usage manuel de Process Monitor de PHASE 90.
    process/file_watch.h
    process/file_watch.cpp

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
    scanner/changed_pages_consensus.h
    scanner/changed_pages_consensus.cpp
    scanner/display_source_classifier.h
    scanner/display_source_classifier.cpp
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
    profiles/ghidra_bridge.h
    profiles/ghidra_bridge.cpp
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

    # Roadmap section J — Speedhack (vitesse perçue accélérée/ralentie via un
    # composant injecté qui hooke les fonctions de temps de la cible)
    debug/speedhack_ipc.h
    debug/speedhack_clock.h
    debug/speedhack.h
    debug/speedhack.cpp

    # Propriétaire logique unique des registres de debug (DR0-DR7) par PID,
    # partagé entre hardware_breakpoint.cpp, breakpoint_freeze.cpp et
    # inprocess_breakpoint.cpp — incident du 19-20/08/2026, voir
    # docs/STRATEGY_ROOM.md
    debug/breakpoint_arbiter.h
    debug/breakpoint_arbiter.cpp

    # Phase 252 — Anti-anti-debug (contourner les mécanismes anti-debug de SC2)
    debug/anti_debug.h
    debug/anti_debug.cpp

    # Stealth Profiler (chantier Cline, repris 03/09/2026) — score de
    # détectabilité + recommandations au-dessus des modules stealth existants.
    debug/stealth_profiler.h
    debug/stealth_profiler.cpp

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
    inject/dll_mask.h
    inject/dll_mask.cpp

    # Phase 20 — Auto-assembler
    # Roadmap section B - interception de fonctions (KillEngineApiHookHandler.dll,
    # moteur MinHook, voir api_hook.h)
    inject/api_hook_ipc.h
    inject/api_hook.h
    inject/api_hook.cpp

    scripting/auto_assembler.h
    scripting/auto_assembler.cpp

    # Phase 20 — Hotkeys globales
    input/global_hotkey.h
    input/global_hotkey.cpp

    # WebView2 CDP Inspector
    webview2/cdp_client.h
    webview2/cdp_client.cpp
    webview2/webview2_inspector.h
    webview2/webview2_inspector.cpp

    # EXTMOD-2 — diff fin par pages 4K + classification de stabilite (session
    # timeline) pour ExternalToolProfiler (apps/desktop/external_tool_profiler.*)
    profiler/page_diff_analyzer.h
    profiler/page_diff_analyzer.cpp

    # SMART-WATCHDOG-1 — Detection auto de resynchronisation post-ecriture
    smart_watchdog/smart_watchdog.h
    smart_watchdog/smart_watchdog.cpp

    # PROPOSITIONS-1 #5 — Memory Heatmap : visualisation de l'activité mémoire
    visualization/memory_heatmap_collector.h
    visualization/memory_heatmap_collector.cpp

    # PROPOSITIONS-1 #3 — Memory Timeline : collecte de séries temporelles
    # mémoire pour un ensemble d'adresses explicitement surveillées, puis
    # analyse pure des patterns/corrélations/profils/prédictions.
    visualization/memory_timeline_collector.h
    visualization/memory_timeline_collector.cpp
    visualization/memory_timeline_analyzer.h
    visualization/memory_timeline_analyzer.cpp

    # PROPOSITIONS-1 #4 — Live Lua REPL : protocole stdin/stdout pur entre
    # LuaReplManager (apps/desktop) et scripts/killengine_repl_driver.lua
    scripting/lua_repl_protocol.h
    scripting/lua_repl_protocol.cpp

    # PROPOSITIONS-1 #2 — Pattern Learning : classification de patterns
    # mémoire (compteur/santé/flag/timer/coordonnée), détection de moteur de
    # jeu, clustering. Corrigé et enregistré ici le 02/09/2026 (Claude) —
    # n'était jamais compilé avant. Bugs trouvés : game_profile_database.cpp
    # dépendait de SQLite (absent du projet, jamais vendoré) — réécrit en
    # JSON-in-file (cohérent avec profile_store.cpp) ; extractVersionFromMemory()
    # appelée dans pattern_learning_engine.cpp mais jamais définie ; includes
    # <set>/<map>/<limits> manquants.
    pattern_learning/feature_extractor.h
    pattern_learning/feature_extractor.cpp
    pattern_learning/game_profile_database.h
    pattern_learning/game_profile_database.cpp
    pattern_learning/pattern_learning_engine.h
    pattern_learning/pattern_learning_engine.cpp
)

target_include_directories(killcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(killcore PUBLIC
    killcore_logging
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    nlohmann_json::nlohmann_json
    lz4_static
    # Windows libraries
    psapi
    dbghelp
    advapi32
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

# ---------------------------------------------------------------------------
# Roadmap section J — Speedhack : DLL injectee dans le processus cible, meme
# raisonnement que KillEnginePageGuardHandler ci-dessus (SANS lien vers
# killcore/Qt). Voir core/debug/speedhack_handler/speedhack_handler.cpp.
# Lie winmm (timeGetTime) : bibliotheque systeme minimale, pas une dependance
# applicative — n'introduit aucun risque de chargement dans la cible.
# Lie aussi minhook (PHASE 14B, 29/08/2026) : le patch IAT seul ne couvre pas
# les appels resolus dynamiquement (P/Invoke JIT .NET notamment, voir
# commentaire de tete de speedhack_handler.cpp) — meme moteur de hook inline
# que KillEngineApiHookHandler ci-dessous.
# ---------------------------------------------------------------------------
add_library(KillEngineSpeedhackHandler SHARED
    debug/speedhack_ipc.h
    debug/speedhack_clock.h
    debug/speedhack_handler/speedhack_handler.cpp
)
target_include_directories(KillEngineSpeedhackHandler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(KillEngineSpeedhackHandler PROPERTIES OUTPUT_NAME "KillEngineSpeedhackHandler")
if(WIN32)
    target_compile_definitions(KillEngineSpeedhackHandler PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(KillEngineSpeedhackHandler PRIVATE winmm minhook)
endif()

# ---------------------------------------------------------------------------
# Roadmap section B - interception de fonctions : DLL injectee dans le
# processus cible qui pose un inline hook MinHook sur module!fonction choisi
# par l utilisateur. Meme raisonnement que les trois handlers ci-dessus (SANS
# lien vers killcore/Qt). Lie minhook (add_subdirectory a la racine) :
# c est toute la valeur ajoutee - le trampoline est gere par la lib.
# ---------------------------------------------------------------------------
add_library(KillEngineApiHookHandler SHARED
    inject/api_hook_ipc.h
    inject/api_hook_handler/api_hook_handler.cpp
)
target_include_directories(KillEngineApiHookHandler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(KillEngineApiHookHandler PROPERTIES OUTPUT_NAME "KillEngineApiHookHandler")
if(WIN32)
    target_compile_definitions(KillEngineApiHookHandler PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(KillEngineApiHookHandler PRIVATE minhook)
endif()
