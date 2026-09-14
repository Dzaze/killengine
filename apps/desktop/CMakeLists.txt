# =============================================================================
# KillEngine Desktop Application
# =============================================================================

qt_add_executable(KillEngine
    main.cpp
    application_controller.cpp
    auto_write_state_access.cpp
    scan_state_access.cpp
    profile_manager.cpp
    settings_diagnostics_manager.cpp
    smart_search_manager.cpp
    scanning_core_manager.cpp
    write_freeze_core_manager.cpp
    clr_inspector_bridge.cpp
    code_patch_manager.cpp
    debug_feature_manager.cpp
    display_string_investigator.cpp
    external_tool_profiler.cpp
    freeze_hotkey_overlay_manager.cpp
    kernel_driver_manager.cpp
    save_file_investigator.cpp
    automation_pipe_server.cpp
    automation_pipe_manager.cpp
    claude_chat_manager.cpp
    crash_handler.cpp
    investigation_notebook_manager.cpp
    effect_proof_manager.cpp
    smart_watchdog_manager.cpp
    lua_runtime_locator.cpp
    lua_repl_manager.cpp
    memory_heatmap_manager.cpp
    memory_timeline_manager.cpp
    pattern_learning_manager.cpp
)


if(WIN32)
    target_sources(KillEngine PRIVATE killengine.rc)
endif()

# Qt WebEngine needs special handling
qt_add_resources(KillEngine "ke_ui"
    PREFIX "/"
    FILES
        resources/index.html
        resources/icons/killengine_app.png
)

target_link_libraries(KillEngine PRIVATE
    killcore
    killai
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::WebEngineWidgets
    Qt6::WebChannel
    Qt6::Network
)

if(WIN32)
    # processHasActiveRemoteConnections() (application_controller.cpp) est
    # deja gardee par #ifdef _WIN32 / stub sinon : miroir cote link pour que
    # ce lien Windows-only ne casse pas un futur build non-Windows.
    target_link_libraries(KillEngine PRIVATE iphlpapi)
    # OpenSCManager/OpenService/StartService pour relancer le service kernel
    # optionnel depuis Paramètres quand il est installé mais arrêté.
    target_link_libraries(KillEngine PRIVATE advapi32)
    # MiniDumpWriteDump (crash_handler.cpp), deja gardee par #ifdef Q_OS_WIN
    # / stub sinon.
    target_link_libraries(KillEngine PRIVATE dbghelp)
    # IUIAutomation (readUiAutomationTree, application_controller.cpp) --
    # CoCreateInstance(CUIAutomation) vient de uiautomationcore.dll.
    target_link_libraries(KillEngine PRIVATE uiautomationcore)
endif()

# Set output name
set_target_properties(KillEngine PROPERTIES
    OUTPUT_NAME "KillEngine"
    WIN32_EXECUTABLE TRUE
)

# ---------------------------------------------------------------------------
# Post-build: copy Qt DLLs and WebEngine resources
# ---------------------------------------------------------------------------
if(WIN32)
    # windeployqt ensures all Qt DLLs, plugins, and WebEngine resources are copied
    find_program(WINDEPLOYQT_EXECUTABLE NAMES windeployqt HINTS ${Qt6_DIR}/../../../bin)
    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET KillEngine POST_BUILD
            COMMAND ${WINDEPLOYQT_EXECUTABLE}
                --no-translations
                --no-system-d3d-compiler
                "$<TARGET_FILE:KillEngine>"
            COMMENT "Running windeployqt..."
        )
    endif()
endif()

# Install rules
install(TARGETS KillEngine RUNTIME DESTINATION bin)
