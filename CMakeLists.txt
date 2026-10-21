cmake_minimum_required(VERSION 3.16)

project(chess_z C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(XIANGQI_SOURCES
    src/position.c
    src/history.c
    src/movegen.c
    src/engine.c
    src/parallel_search.c
    src/search_sampling.c
)

add_library(xiangqi ${XIANGQI_SOURCES})

target_include_directories(xiangqi
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

function(xq_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

xq_enable_warnings(xiangqi)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(xiangqi PUBLIC Threads::Threads)
endif()

add_executable(xiangqi_cli examples/cli.c examples/search_progress.c examples/search_sample_report.c)
target_link_libraries(xiangqi_cli PRIVATE xiangqi)
xq_enable_warnings(xiangqi_cli)

add_executable(xiangqi_explain examples/explain.c)
target_link_libraries(xiangqi_explain PRIVATE xiangqi)
xq_enable_warnings(xiangqi_explain)

find_package(raylib QUIET)
if(raylib_FOUND)
    add_executable(xiangqi_gui examples/gui.c)
    target_link_libraries(xiangqi_gui PRIVATE xiangqi raylib)
    xq_enable_warnings(xiangqi_gui)
else()
    message(STATUS "raylib not found: skipping xiangqi_gui")
endif()

add_executable(xiangqi_generate_positions stats/generate_positions.c)
target_link_libraries(xiangqi_generate_positions PRIVATE xiangqi)
xq_enable_warnings(xiangqi_generate_positions)

add_executable(xiangqi_search_positions stats/search_positions.c)
target_link_libraries(xiangqi_search_positions PRIVATE xiangqi)
xq_enable_warnings(xiangqi_search_positions)

include(CTest)
if(BUILD_TESTING)
    find_program(XQ_VIEWER_NODE NAMES node nodejs)
    if(XQ_VIEWER_NODE)
        add_test(NAME search_sample_viewer_model COMMAND ${XQ_VIEWER_NODE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/search_sample_viewer_tests.js)
        add_test(NAME search_sample_viewer_ui COMMAND ${XQ_VIEWER_NODE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/search_sample_viewer_ui_tests.js)
        set_tests_properties(search_sample_viewer_model search_sample_viewer_ui PROPERTIES TIMEOUT 30)
    endif()
endif()
if(BUILD_TESTING AND NOT WIN32)
    add_executable(xiangqi_sampling_tests tests/search_sampling_tests.c)
    target_include_directories(xiangqi_sampling_tests PRIVATE src)
    target_link_libraries(xiangqi_sampling_tests PRIVATE xiangqi)
    xq_enable_warnings(xiangqi_sampling_tests)
    add_test(NAME search_sampling COMMAND xiangqi_sampling_tests)
    set_tests_properties(search_sampling PROPERTIES TIMEOUT 30)

    add_executable(xiangqi_sampling_report_tests tests/sampling_report_tests.c
        examples/search_progress.c examples/search_sample_report.c)
    target_include_directories(xiangqi_sampling_report_tests PRIVATE src examples)
    target_link_libraries(xiangqi_sampling_report_tests PRIVATE xiangqi)
    xq_enable_warnings(xiangqi_sampling_report_tests)
    add_test(NAME sampling_reports COMMAND xiangqi_sampling_report_tests
        ${CMAKE_CURRENT_BINARY_DIR}/sampling_test_reports)
    set_tests_properties(sampling_reports PROPERTIES TIMEOUT 15)
    add_executable(xiangqi_sampling_allocation_tests tests/sampling_allocation_tests.c
        src/search_sampling.c examples/search_progress.c examples/search_sample_report.c)
    target_include_directories(xiangqi_sampling_allocation_tests PRIVATE src examples)
    target_compile_definitions(xiangqi_sampling_allocation_tests PRIVATE
        calloc=xq_test_calloc realloc=xq_test_realloc)
    target_link_libraries(xiangqi_sampling_allocation_tests PRIVATE xiangqi)
    xq_enable_warnings(xiangqi_sampling_allocation_tests)
    add_test(NAME sampling_allocation_failures COMMAND xiangqi_sampling_allocation_tests
        ${CMAKE_CURRENT_BINARY_DIR}/sampling_allocation_reports)
    set_tests_properties(sampling_allocation_failures PROPERTIES TIMEOUT 15)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME sampling_report_format COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sampling_reports.py
            $<TARGET_FILE:xiangqi_sampling_report_tests>)
        set_tests_properties(sampling_report_format PROPERTIES TIMEOUT 20)
    endif()
endif()
