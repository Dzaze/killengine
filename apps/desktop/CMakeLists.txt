# =============================================================================
# KillEngine Desktop Application
# =============================================================================

qt_add_executable(KillEngine
    main.cpp
    application_controller.cpp
    automation_pipe_server.cpp
    crash_handler.cpp
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
    # MiniDumpWriteDump (crash_handler.cpp), deja gardee par #ifdef Q_OS_WIN
    # / stub sinon.
    target_link_libraries(KillEngine PRIVATE dbghelp)
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
