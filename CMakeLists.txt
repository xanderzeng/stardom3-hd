cmake_minimum_required(VERSION 3.20)
project(Stardom3Widescreen LANGUAGES CXX)

enable_testing()
add_executable(font_scale_patch_tests tests/font_scale_patch_tests.cpp)
target_compile_features(font_scale_patch_tests PRIVATE cxx_std_17)
target_compile_definitions(font_scale_patch_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_property(TARGET font_scale_patch_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_test(NAME font_scale_patch COMMAND font_scale_patch_tests)
add_executable(engine_display_mode_tests tests/engine_display_mode_tests.cpp)
target_compile_features(engine_display_mode_tests PRIVATE cxx_std_17)
target_include_directories(engine_display_mode_tests PRIVATE src)
set_property(TARGET engine_display_mode_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_test(NAME engine_display_mode COMMAND engine_display_mode_tests)

add_executable(gui_move_bounds_tests tests/gui_move_bounds_tests.cpp)
target_compile_features(gui_move_bounds_tests PRIVATE cxx_std_17)
target_include_directories(gui_move_bounds_tests PRIVATE src)
set_property(TARGET gui_move_bounds_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_test(NAME gui_move_bounds COMMAND gui_move_bounds_tests)

add_executable(training_animation_tests tests/training_animation_tests.cpp)
target_compile_features(training_animation_tests PRIVATE cxx_std_17)
target_include_directories(training_animation_tests PRIVATE src)
target_compile_definitions(training_animation_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_property(TARGET training_animation_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_test(NAME training_animation COMMAND training_animation_tests)

add_library(d3d9 SHARED
    src/d3d9_proxy.cpp
    src/display_config.cpp
    src/proxy_config.cpp
    src/proxy_config.h
    src/ui_geometry.cpp
    src/d3d9_ui_layout.cpp
    src/ui_layout_registry.cpp
    src/ui_layout_registry.h
    src/ui_page_geometry.cpp
    src/ui_page_geometry.h
    src/gui_object.cpp
    src/gui_object.h
    src/d3d9_render_hooks.cpp
    src/render_hook_state.cpp
    src/render_hook_state.h
    src/render_diagnostics.cpp
    src/render_diagnostics.h
    src/d3d9_video.cpp
    src/d3d9_font.cpp
    src/font_scale_patch.cpp
    src/font_outline.cpp
    src/d3d9_proxy_internal.h
    src/ui_layout_state.h
    src/ui_layout_state.cpp
    src/d3d9_proxy.def
)
target_include_directories(d3d9 PRIVATE src)
target_compile_features(d3d9 PRIVATE cxx_std_17)
target_compile_definitions(d3d9 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(d3d9 PRIVATE user32 gdi32 dxguid)
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

add_executable(resolution_geometry_tests
    tests/resolution_geometry_tests.cpp
    src/display_config.cpp
    src/ui_geometry.cpp
)
target_compile_features(resolution_geometry_tests PRIVATE cxx_std_17)
target_include_directories(resolution_geometry_tests PRIVATE src)
set_property(TARGET resolution_geometry_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(proxy_config_tests
    tests/proxy_config_tests.cpp
    src/display_config.cpp
    src/proxy_config.cpp
)
target_compile_features(proxy_config_tests PRIVATE cxx_std_17)
target_include_directories(proxy_config_tests PRIVATE src)
target_compile_definitions(proxy_config_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set_property(TARGET proxy_config_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(font_replacement_tests tests/font_replacement_tests.cpp)
target_compile_features(font_replacement_tests PRIVATE cxx_std_17)
target_compile_definitions(font_replacement_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(font_replacement_tests PRIVATE gdi32)
set_property(TARGET font_replacement_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(ui_layout_registry_tests
    tests/ui_layout_registry_tests.cpp
    src/ui_layout_registry.cpp
    src/ui_layout_state.cpp
)
target_compile_features(ui_layout_registry_tests PRIVATE cxx_std_17)
target_include_directories(ui_layout_registry_tests PRIVATE src)
target_compile_definitions(ui_layout_registry_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_property(TARGET ui_layout_registry_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(ui_page_geometry_tests
    tests/ui_page_geometry_tests.cpp
    src/ui_page_geometry.cpp
    src/ui_geometry.cpp
    src/gui_object.cpp
)
target_compile_features(ui_page_geometry_tests PRIVATE cxx_std_17)
target_include_directories(ui_page_geometry_tests PRIVATE src)
target_compile_definitions(ui_page_geometry_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_property(TARGET ui_page_geometry_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(gui_field_tests tests/gui_field_tests.cpp)
target_compile_features(gui_field_tests PRIVATE cxx_std_17)
target_include_directories(gui_field_tests PRIVATE src)
set_property(TARGET gui_field_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_test(NAME gui_field COMMAND gui_field_tests)

add_executable(font_outline_render_tests tests/font_outline_render_tests.cpp)
target_compile_features(font_outline_render_tests PRIVATE cxx_std_17)
target_include_directories(font_outline_render_tests PRIVATE src)
target_compile_definitions(font_outline_render_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(font_outline_render_tests PRIVATE user32)
set_property(TARGET font_outline_render_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_test(NAME font_outline_render COMMAND font_outline_render_tests)

add_test(NAME resolution_geometry COMMAND resolution_geometry_tests)
add_test(NAME proxy_config COMMAND proxy_config_tests)
add_test(NAME ui_layout_registry COMMAND ui_layout_registry_tests)
add_test(NAME ui_page_geometry COMMAND ui_page_geometry_tests)
add_test(NAME font_replacement_default
    COMMAND font_replacement_tests $<TARGET_FILE:d3d9>
        SimHei 1.00 20)
add_test(NAME font_replacement_scaled
    COMMAND font_replacement_tests $<TARGET_FILE:d3d9> Arial 1.25 25)
add_test(NAME proxy_default
    COMMAND proxy_smoke $<TARGET_FILE:d3d9> 1920 1080 1920 1080)
add_test(NAME proxy_16_9
    COMMAND proxy_smoke $<TARGET_FILE:d3d9> 1280 720 1280 720)
add_test(NAME proxy_4_3
    COMMAND proxy_smoke $<TARGET_FILE:d3d9> 1024 768 1024 768)
add_test(NAME proxy_borderless
    COMMAND proxy_smoke $<TARGET_FILE:d3d9> 1920 1080 1920 1080 1)
add_test(NAME proxy_invalid_resolution
    COMMAND proxy_smoke $<TARGET_FILE:d3d9> 1366 768 -1 -1)
add_test(NAME proxy_negative_resolution
    COMMAND proxy_smoke $<TARGET_FILE:d3d9> -1920 1080 -1 -1)
add_test(NAME proxy_partial_resolution
    COMMAND proxy_smoke $<TARGET_FILE:d3d9> 1920 - -1 -1)
add_test(NAME proxy_malformed_resolution
    COMMAND proxy_smoke $<TARGET_FILE:d3d9> invalid 1080 -1 -1)
add_test(NAME proxy_lifecycle_native
    COMMAND proxy_smoke $<TARGET_FILE:d3d9> 1920 1080 1920 1080 0 1)
add_test(NAME proxy_lifecycle_compatibility
    COMMAND proxy_smoke $<TARGET_FILE:d3d9> 1280 720 1280 720 1 0)
set_tests_properties(proxy_lifecycle_native proxy_lifecycle_compatibility
    PROPERTIES SKIP_RETURN_CODE 77)
