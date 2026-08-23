cmake_minimum_required(VERSION 3.21)

# =============================================================================
# KillEngine — Projet principal
# =============================================================================
project(KillEngine
    VERSION 0.1.0
    DESCRIPTION "Analyseur de mémoire Windows assisté par IA"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Configuration générale
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Multiprocessing compilation (MSVC)
if(MSVC)
    add_compile_options(/MP /W3 /utf-8 /permissive-)
    # Disable MSVC warnings that are noisy with Qt
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

# ---------------------------------------------------------------------------
# Trouver Qt 6
# ---------------------------------------------------------------------------
# Hint: set CMAKE_PREFIX_PATH to your Qt installation
# Example: cmake -DCMAKE_PREFIX_PATH=C:/Qt/6.8.1/msvc2022_64 ..
find_package(Qt6 6.8 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    WebEngineWidgets
    WebChannel
    Network
)

# Ensure Qt tools are found
if(NOT DEFINED QT_QMAKE_EXECUTABLE)
    find_program(QT_QMAKE_EXECUTABLE NAMES qmake HINTS ${Qt6_DIR}/../../../bin)
endif()

qt_standard_project_setup()

# ---------------------------------------------------------------------------
# Trouver ou fetcher nlohmann/json
# ---------------------------------------------------------------------------
# We use FetchContent for simplicity — no vcpkg required for header-only libs
include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ---------------------------------------------------------------------------
# Google Test
# ---------------------------------------------------------------------------
option(KILLENGINE_BUILD_TESTS "Build KillEngine unit and integration tests" ON)

if(KILLENGINE_BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE
    )
    # For Windows: prevent overriding parent project's compiler/linker settings
    set(gtest_force_shared_rt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# ---------------------------------------------------------------------------
# LZ4
# ---------------------------------------------------------------------------
FetchContent_Declare(
    lz4
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG v1.10.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR build/cmake
)
FetchContent_MakeAvailable(lz4)

# ---------------------------------------------------------------------------
# Zydis — désassembleur x86/x64 pour signatures et patchs assistés
# ---------------------------------------------------------------------------
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    zydis
    GIT_REPOSITORY https://github.com/zyantific/zydis.git
    GIT_TAG v4.1.1
    GIT_SHALLOW TRUE
    GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(zydis)

# ---------------------------------------------------------------------------
# MinHook — moteur d'inline hook (trampoline robuste) pour le composant injecté
# d'interception de fonctions (core/inject/api_hook_handler). Source locale
# (tools/minhook-master/minhook-master, v1.3.3) plutôt que FetchContent : déjà
# présente sur disque, pas de dépendance réseau au configure. Cible : `minhook`.
# ---------------------------------------------------------------------------
add_subdirectory(tools/minhook-master/minhook-master EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Définitions globales du projet
# ---------------------------------------------------------------------------
add_compile_definitions(
    KILLENGINE_VERSION="${PROJECT_VERSION}"
    KILLENGINE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    KILLENGINE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    KILLENGINE_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------
include_directories(${CMAKE_SOURCE_DIR}/core)
include_directories(${CMAKE_SOURCE_DIR}/ai)

# ---------------------------------------------------------------------------
# UI assets — built by Vite, consumed as Qt resource
# ---------------------------------------------------------------------------
# The UI is built separately via npm/vite before the C++ build.
# See: ui/ directory and scripts/build_ui.ps1
set(KILLENGINE_UI_DIST ${CMAKE_SOURCE_DIR}/ui/dist)
set(KILLENGINE_UI_RESOURCE ${CMAKE_SOURCE_DIR}/apps/desktop/resources/ke_ui.qrc)

# ---------------------------------------------------------------------------
# Message d'information
# ---------------------------------------------------------------------------
message(STATUS "============================================")
message(STATUS "  KillEngine ${PROJECT_VERSION}")
message(STATUS "============================================")
message(STATUS "Qt6 found: ${Qt6_DIR}")
message(STATUS "Tests enabled: ${KILLENGINE_BUILD_TESTS}")
message(STATUS "UI dist path: ${KILLENGINE_UI_DIST}")
message(STATUS "============================================")

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
add_subdirectory(core)
add_subdirectory(ai)
add_subdirectory(apps/desktop)

if(KILLENGINE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
