# Runtime integrity probe: injectable diagnostic DLL for authorized lab targets.
# It observes selected debug-state queries and records telemetry without altering
# the API result returned to the target process.
add_library(KillEngineRuntimeIntegrityProbe SHARED
    runtime_integrity_probe.cpp
)

target_link_libraries(KillEngineRuntimeIntegrityProbe PRIVATE minhook)

if(WIN32)
    target_compile_definitions(KillEngineRuntimeIntegrityProbe PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
endif()

set_target_properties(KillEngineRuntimeIntegrityProbe PROPERTIES
    OUTPUT_NAME "KillEngineRuntimeIntegrityProbe"
)
