cmake_minimum_required(VERSION 3.20)
project(mfg-provider LANGUAGES CXX)

# MFG Provider: custom dlssg provider for OptiScaler (phases 0-1).
# Output name matters: custom/<name>/dlssg accepts any dll, but the standard
# name nvngx_dlssg.dll keeps the folder unambiguous (misc/CustomProviders.h).
add_library(mfg-provider SHARED
    src/dllmain.cpp
    src/ngx_exports.cpp
    src/plugin.cpp
    src/backends.cpp
    src/lsfg_backend.cpp
    src/lsfg/shader_bank.cpp
    src/lsfg/pipeline.cpp
    src/lsfg/dll_extract.cpp
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

# d3d12.h / dxgi.h come from the Windows SDK; phase 1 links d3d12.lib for
# the D3D12SerializeRootSignature used by the LSFG shader bank, and calls
# D3DReflect through d3dcompiler_47.dll loaded dynamically at runtime (the
# probe tool links it directly instead).
target_link_libraries(mfg-provider PRIVATE d3d12 dxgi)
# Developer tools (lsfg_probe etc). Off by default; CI builds only the dll.
option(MFG_BUILD_TOOLS "Build developer tools" OFF)
if(MFG_BUILD_TOOLS)
    add_executable(lsfg_probe tools/lsfg_probe.cpp src/lsfg/dll_extract.cpp)
    target_include_directories(lsfg_probe PRIVATE src)
    target_compile_features(lsfg_probe PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(lsfg_probe PRIVATE /W4 /permissive- /EHsc)
        target_compile_definitions(lsfg_probe PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(lsfg_probe PRIVATE d3dcompiler dxgi)
    endif()

    # Offline repro for the phase-1 pipeline: debug layer + clears + one
    # burst graph evaluation, prints debug-layer messages and the device
    # removal reason.
    add_executable(lsfg_pipeline_test tools/lsfg_pipeline_test.cpp
        src/lsfg/dll_extract.cpp src/lsfg/shader_bank.cpp src/lsfg/pipeline.cpp
        src/log.cpp src/mfg_state.cpp)
    target_include_directories(lsfg_pipeline_test PRIVATE src)
    target_compile_features(lsfg_pipeline_test PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(lsfg_pipeline_test PRIVATE /W4 /permissive- /EHsc)
        target_compile_definitions(lsfg_pipeline_test PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(lsfg_pipeline_test PRIVATE d3d12 dxgi d3dcompiler)
    endif()
endif()