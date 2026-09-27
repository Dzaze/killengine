# =============================================================================
# KillEngineDemoTarget — cible de démonstration distribuable (UX-PRODUIT-15)
# =============================================================================
# Reprend la FORME de tests/memory_targets/test_target_main.cpp (Qt Widgets,
# pas Win32 brut) mais aucun de ses modes stress/hook -- distribuable avec le
# paquet portable (voir scripts/package-windows.ps1), contrairement à
# KillEngineTestTarget (tests/, gated par KILLENGINE_BUILD_TESTS, exclu et
# interdit à l'empaquetage).
add_executable(KillEngineDemoTarget
    demo_target_main.cpp
)
target_link_libraries(KillEngineDemoTarget PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)
set_target_properties(KillEngineDemoTarget PROPERTIES
    AUTOMOC ON
    WIN32_EXECUTABLE ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
