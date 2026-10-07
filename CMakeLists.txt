cmake_minimum_required(VERSION 3.28)

project(Avionix VERSION 0.0.0 LANGUAGES CXX)

include(CTest)

option(AVIONIX_BUILD_EXAMPLES "Build the Avionix examples" ON)
option(AVIONIX_BUILD_BENCHMARKS "Build the Avionix benchmarks" OFF)
option(AVIONIX_WARNINGS_AS_ERRORS "Treat warnings in Avionix sources as errors" ON)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

set(avionix_modules
    core/entity/geometry.cppm
    core/entity/color.cppm
    core/entity/style.cppm
    core/entity/unicode.cppm
    core/entity/cell.cppm
    core/entity/event.cppm
    core/entity/constraint.cppm
    core/entity/error.cppm
    core/entity/markdown.cppm
    core/object/terminal_capabilities.cppm
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
    core/interface/application.cppm
    lib/avionix.cppm
)

add_library(avionix)
add_library(Avionix::avionix ALIAS avionix)
target_compile_features(avionix PUBLIC cxx_std_26)
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

if(BUILD_TESTING)
    add_executable(avionix-tests
        tests/main.cpp
        tests/entity_tests.cpp
        tests/unicode_tests.cpp
        tests/buffer_tests.cpp
        tests/renderer_tests.cpp
        tests/input_tests.cpp
        tests/event_tests.cpp
        tests/layout_tests.cpp
        tests/widget_tests.cpp
        tests/feature_tests.cpp
        tests/markdown_tests.cpp
        tests/terminal_tests.cpp
    )
    target_compile_features(avionix-tests PRIVATE cxx_std_26)
    target_sources(avionix-tests PRIVATE FILE_SET CXX_MODULES FILES tests/check.cppm)
    target_link_libraries(avionix-tests PRIVATE avionix)
    add_test(NAME avionix-tests COMMAND avionix-tests)

    add_executable(avionix-pty-smoke tests/pty_smoke.cpp)
    target_compile_features(avionix-pty-smoke PRIVATE cxx_std_26)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(avionix-pty-smoke PRIVATE util)
    endif()
endif()

if(AVIONIX_BUILD_EXAMPLES)
    foreach(example IN ITEMS hello counter dashboard markdown)
        add_executable(avionix-${example} examples/${example}.cpp)
        target_compile_features(avionix-${example} PRIVATE cxx_std_26)
        target_link_libraries(avionix-${example} PRIVATE avionix)
        set_target_properties(avionix-${example} PROPERTIES OUTPUT_NAME ${example})
    endforeach()
endif()

if(AVIONIX_BUILD_BENCHMARKS)
    add_executable(avionix-benchmarks benchmarks/main.cpp)
    target_compile_features(avionix-benchmarks PRIVATE cxx_std_26)
    target_link_libraries(avionix-benchmarks PRIVATE avionix)
endif()

include(GNUInstallDirs)
install(TARGETS avionix
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
