cmake_minimum_required(VERSION 3.20)
project(Stardom3Widescreen LANGUAGES CXX)

add_library(d3d9 SHARED
    src/d3d9_proxy.cpp
    src/d3d9_ui_layout.cpp
    src/d3d9_render_hooks.cpp
    src/d3d9_video.cpp
    src/d3d9_proxy_internal.h
    src/d3d9_proxy.def
)
target_compile_features(d3d9 PRIVATE cxx_std_17)
target_compile_definitions(d3d9 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(d3d9 PRIVATE user32 dxguid)
if(MSVC)
    target_link_options(d3d9 PRIVATE "/MAP:${CMAKE_CURRENT_BINARY_DIR}/d3d9.map")
endif()
set_property(TARGET d3d9 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(d3d9 PROPERTIES
    OUTPUT_NAME d3d9
    PREFIX ""
    SUFFIX ".dll"
)

add_executable(proxy_smoke tests/proxy_smoke.cpp)
target_compile_features(proxy_smoke PRIVATE cxx_std_17)
target_compile_definitions(proxy_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(proxy_smoke PRIVATE d3d9 user32)
set_property(TARGET proxy_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
