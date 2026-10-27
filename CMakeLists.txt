cmake_minimum_required(VERSION 3.28)

project(Avionix VERSION 0.0.0 LANGUAGES C CXX)

include(CTest)

option(AVIONIX_BUILD_EXAMPLES "Build the Avionix examples" ON)
option(AVIONIX_BUILD_BENCHMARKS "Build the Avionix benchmarks" OFF)
option(AVIONIX_WARNINGS_AS_ERRORS "Treat warnings in Avionix sources as errors" ON)
option(AVIONIX_BUILD_VIEWPORT "Build the optional 3D viewport library (GLFW + OpenGL 3.3)" ON)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

set(avionix_modules
    core/entity/geometry.cppm
    core/entity/color.cppm
    core/entity/style.cppm
    core/entity/unicode.cppm
    core/entity/cell.cppm
    core/entity/clipboard.cppm
    core/entity/event.cppm
    core/entity/constraint.cppm
    core/entity/error.cppm
    core/entity/markdown.cppm
    core/entity/latex.cppm
    core/entity/image.cppm
    core/entity/terminal_graphics.cppm
    core/object/terminal_capabilities.cppm
    core/object/image_file.cppm
    core/object/clipboard.cppm
    core/object/terminal.cppm
    core/object/ansi_encoder.cppm
    core/object/buffer.cppm
    core/object/renderer.cppm
    core/object/input_decoder.cppm
    core/object/event_queue.cppm
    core/object/input_reader.cppm
    core/task/dispatch.cppm
    core/task/render.cppm
    core/task/resize.cppm
    core/task/run.cppm
    core/interface/widget.cppm
    core/interface/layout.cppm
    core/interface/controls.cppm
    core/interface/markdown.cppm
    core/interface/image.cppm
    core/interface/application.cppm
    lib/avionix.cppm
)

add_library(avionix)
add_library(Avionix::avionix ALIAS avionix)
if(WIN32)
    target_link_libraries(avionix PUBLIC user32)
endif()
target_compile_features(avionix PUBLIC cxx_std_26)
# stb_image (PNG and JPEG decoding), third-party C with its own flags.
target_sources(avionix PRIVATE libs/stb/stb_image_impl.c)
set_source_files_properties(libs/stb/stb_image_impl.c PROPERTIES COMPILE_OPTIONS "-w")
target_include_directories(avionix SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/libs/stb)
target_sources(avionix
    PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES ${avionix_modules}
)

set(avionix_warnings
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Woverloaded-virtual
    -Wimplicit-fallthrough
)

target_compile_options(avionix PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:${avionix_warnings}>
    $<$<AND:$<BOOL:${AVIONIX_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:Clang,GNU>>:-Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<AND:$<BOOL:${AVIONIX_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:MSVC>>:/WX>
)

# Capability detection reads a fixed set of environment variables once at
# startup. The Windows CRT deprecates getenv even for this read-only use.
target_compile_definitions(avionix PRIVATE
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
)

# Clang records this ABI option in BMIs. Importers must use the same value.
target_compile_options(avionix PUBLIC
    $<$<CXX_COMPILER_ID:Clang>:-fno-sized-deallocation>
)

# Optional 3D viewport: a separate library so terminal-only consumers do not
# link GLFW or OpenGL. GLFW 3.4 is vendored in libs/glfw (zlib license).
if(AVIONIX_BUILD_VIEWPORT)
    if(APPLE)
        enable_language(C OBJC)
    else()
        enable_language(C)
    endif()

    set(glfw_dir ${CMAKE_CURRENT_SOURCE_DIR}/libs/glfw/src)
    set(glfw_sources
        context.c init.c input.c monitor.c platform.c vulkan.c window.c
        egl_context.c osmesa_context.c null_init.c null_monitor.c null_window.c
        null_joystick.c)
    if(WIN32)
        list(APPEND glfw_sources win32_init.c win32_joystick.c win32_module.c
            win32_monitor.c win32_time.c win32_thread.c win32_window.c wgl_context.c)
        set(glfw_platform _GLFW_WIN32)
    elseif(APPLE)
        list(APPEND glfw_sources cocoa_init.m cocoa_joystick.m cocoa_monitor.m
            cocoa_window.m nsgl_context.m cocoa_time.c posix_module.c posix_thread.c)
        set(glfw_platform _GLFW_COCOA)
    else()
        # X11 only; Wayland sessions run it through XWayland.
        list(APPEND glfw_sources posix_module.c posix_time.c posix_thread.c
            posix_poll.c x11_init.c x11_monitor.c x11_window.c xkb_unicode.c
            glx_context.c linux_joystick.c)
        set(glfw_platform _GLFW_X11 _DEFAULT_SOURCE)
    endif()
    list(TRANSFORM glfw_sources PREPEND ${glfw_dir}/)
    add_library(avionix_glfw STATIC ${glfw_sources})
    target_include_directories(avionix_glfw SYSTEM PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/glfw/include>)
    target_compile_definitions(avionix_glfw PRIVATE ${glfw_platform}
        $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>)
    set_target_properties(avionix_glfw PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
    if(WIN32)
        target_link_libraries(avionix_glfw PUBLIC gdi32 user32 shell32)
    elseif(APPLE)
        target_link_libraries(avionix_glfw PUBLIC "-framework Cocoa" "-framework IOKit"
            "-framework CoreFoundation" "-framework QuartzCore")
    else()
        target_link_libraries(avionix_glfw PUBLIC ${CMAKE_DL_LIBS} pthread m)
    endif()

    add_library(avionix_viewport)
    add_library(Avionix::viewport ALIAS avionix_viewport)
    target_compile_features(avionix_viewport PUBLIC cxx_std_26)
    target_sources(avionix_viewport
        PUBLIC
            FILE_SET CXX_MODULES
            BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
            FILES
                core/entity/spatial.cppm
                core/entity/scene.cppm
                core/entity/camera.cppm
                core/entity/picking.cppm
                core/entity/manipulator.cppm
                core/entity/viewport.cppm
                core/object/scene_store.cppm
                core/object/gl.cppm
                core/object/native_window.cppm
                core/object/scene_renderer.cppm
                core/object/software_renderer.cppm
                core/interface/viewport.cppm
                core/interface/viewport_panel.cppm
                lib/viewport.cppm
    )
    target_link_libraries(avionix_viewport PUBLIC avionix PRIVATE avionix_glfw)
    target_compile_options(avionix_viewport PRIVATE
        $<$<CXX_COMPILER_ID:Clang,GNU>:${avionix_warnings}>
        $<$<AND:$<BOOL:${AVIONIX_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:Clang,GNU>>:-Werror>
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<AND:$<BOOL:${AVIONIX_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:MSVC>>:/WX>
    )
endif()

if(BUILD_TESTING)
    add_executable(avionix-tests
        tests/main.cpp
        tests/entity_tests.cpp
        tests/unicode_tests.cpp
        tests/buffer_tests.cpp
        tests/renderer_tests.cpp
        tests/input_tests.cpp
        tests/attachment_tests.cpp
        tests/event_tests.cpp
        tests/layout_tests.cpp
        tests/widget_tests.cpp
        tests/feature_tests.cpp
        tests/markdown_tests.cpp
        tests/terminal_tests.cpp
        tests/graphics_tests.cpp
    )
    target_compile_features(avionix-tests PRIVATE cxx_std_26)
    target_sources(avionix-tests PRIVATE FILE_SET CXX_MODULES FILES tests/check.cppm)
    target_link_libraries(avionix-tests PRIVATE avionix)
    if(AVIONIX_BUILD_VIEWPORT)
        target_sources(avionix-tests PRIVATE tests/viewport_tests.cpp)
        target_link_libraries(avionix-tests PRIVATE avionix_viewport)
    endif()
    add_test(NAME avionix-tests COMMAND avionix-tests)

    add_executable(avionix-pty-smoke tests/pty_smoke.cpp)
    target_compile_features(avionix-pty-smoke PRIVATE cxx_std_26)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(avionix-pty-smoke PRIVATE util)
    endif()
endif()

if(AVIONIX_BUILD_EXAMPLES)
    foreach(example IN ITEMS hello counter dashboard markdown attachments background)
        add_executable(avionix-${example} examples/${example}.cpp)
        target_compile_features(avionix-${example} PRIVATE cxx_std_26)
        target_link_libraries(avionix-${example} PRIVATE avionix)
        set_target_properties(avionix-${example} PROPERTIES OUTPUT_NAME ${example})
    endforeach()
    if(AVIONIX_BUILD_VIEWPORT)
        add_executable(avionix-both examples/both.cpp)
        target_compile_features(avionix-both PRIVATE cxx_std_26)
        target_link_libraries(avionix-both PRIVATE avionix_viewport)
        set_target_properties(avionix-both PROPERTIES OUTPUT_NAME both)
        add_executable(avionix-viewport examples/viewport.cpp)
        target_compile_features(avionix-viewport PRIVATE cxx_std_26)
        target_link_libraries(avionix-viewport PRIVATE avionix_viewport)
        set_target_properties(avionix-viewport PROPERTIES OUTPUT_NAME viewport)
    endif()
endif()

if(AVIONIX_BUILD_BENCHMARKS)
    add_executable(avionix-benchmarks benchmarks/main.cpp)
    target_compile_features(avionix-benchmarks PRIVATE cxx_std_26)
    target_link_libraries(avionix-benchmarks PRIVATE avionix)
endif()

include(GNUInstallDirs)
set(avionix_install_targets avionix)
if(AVIONIX_BUILD_VIEWPORT)
    list(APPEND avionix_install_targets avionix_viewport avionix_glfw)
endif()
install(TARGETS ${avionix_install_targets}
    EXPORT AvionixTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_DATADIR}/avionix/modules
)
install(EXPORT AvionixTargets
    FILE AvionixTargets.cmake
    NAMESPACE Avionix::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Avionix
)
