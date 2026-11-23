cmake_minimum_required(VERSION 3.20)
project(mfg-provider LANGUAGES CXX)

# Phase 0 skeleton: custom dlssg provider for OptiScaler.
# Output name matters: custom/<name>/dlssg accepts any dll, but the standard
# name nvngx_dlssg.dll keeps the folder unambiguous (misc/CustomProviders.h).
add_library(mfg-provider SHARED
    src/dllmain.cpp
    src/ngx_exports.cpp
    src/plugin.cpp
    src/backends.cpp
    src/mfg_state.cpp
    src/log.cpp
)

set_target_properties(mfg-provider PROPERTIES OUTPUT_NAME "nvngx_dlssg")

target_include_directories(mfg-provider PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include src)

target_compile_features(mfg-provider PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(mfg-provider PRIVATE /W4 /permissive- /EHsc)
    target_compile_definitions(mfg-provider PRIVATE _CRT_SECURE_NO_WARNINGS)
    # Static CRT: the dll must not depend on the VC++ Redistributable
    # being present in a game folder.
    set_property(TARGET mfg-provider PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# d3d12.h / dxgi.h come from the Windows SDK; no d3d12.lib needed in phase 0
# (we only store and log pointers, never call into D3D12).
target_link_libraries(mfg-provider PRIVATE dxgi)
