cmake_minimum_required(VERSION 3.20)
project(Stardom3FontPatch LANGUAGES CXX)

enable_testing()

add_library(d3d9 SHARED
    src/d3d9_proxy.cpp
    src/d3d9_proxy.def
    src/font_config.cpp
    src/font_config.h
    src/font_hook.cpp
    src/font_hook.h
    src/import_hook.cpp
    src/import_hook.h
)
target_compile_features(d3d9 PRIVATE cxx_std_17)
target_compile_definitions(d3d9 PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(d3d9 PRIVATE gdi32)
set_property(TARGET d3d9 PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(d3d9 PROPERTIES
    OUTPUT_NAME d3d9
    PREFIX ""
    SUFFIX ".dll"
)

add_executable(font_config_tests
    tests/font_config_tests.cpp
    src/font_config.cpp
)
target_include_directories(font_config_tests PRIVATE src)
target_compile_features(font_config_tests PRIVATE cxx_std_17)
target_compile_definitions(font_config_tests PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set_property(TARGET font_config_tests PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(font_patch_smoke tests/font_patch_smoke.cpp)
target_compile_features(font_patch_smoke PRIVATE cxx_std_17)
target_compile_definitions(font_patch_smoke PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(font_patch_smoke PRIVATE gdi32)
set_property(TARGET font_patch_smoke PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_test(NAME font_config COMMAND font_config_tests)
add_test(NAME font_patch_default
    COMMAND font_patch_smoke $<TARGET_FILE:d3d9> 1
        SimHei 1.00 SimHei 20)
add_test(NAME font_patch_custom_scale
    COMMAND font_patch_smoke $<TARGET_FILE:d3d9> 1 Arial 1.25 Arial 25)
add_test(NAME font_patch_disabled
    COMMAND font_patch_smoke $<TARGET_FILE:d3d9> 0 Arial 1.25 MingLiU 20)
