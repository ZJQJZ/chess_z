cmake_minimum_required(VERSION 3.16)

project(chess_z C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(xiangqi
    src/position.c
    src/history.c
    src/movegen.c
    src/engine.c
    src/parallel_search.c
)

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

add_executable(xiangqi_cli examples/cli.c)
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

enable_testing()

add_executable(xiangqi_core_tests tests/test_core.c)
target_link_libraries(xiangqi_core_tests PRIVATE xiangqi)
add_test(NAME xiangqi_core_tests COMMAND xiangqi_core_tests)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_explain.c")
    add_executable(xiangqi_explain_tests tests/test_explain.c)
    target_link_libraries(xiangqi_explain_tests PRIVATE xiangqi)
    xq_enable_warnings(xiangqi_explain_tests)
    add_test(NAME xiangqi_explain_tests COMMAND xiangqi_explain_tests)
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    foreach(name explain_cli cli)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${name}.py")
            if(name STREQUAL "explain_cli")
                set(cli_target xiangqi_explain)
            else()
                set(cli_target xiangqi_cli)
            endif()
            add_test(NAME xiangqi_${name}_tests
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${name}.py
                        $<TARGET_FILE:${cli_target}>)
        endif()
    endforeach()
endif()

if(NOT WIN32)
    # Compile the search with private scheduling/failure hooks for deterministic tests.
    add_executable(xiangqi_parallel_tests tests/test_parallel.c src/parallel_search.c)
    target_compile_definitions(xiangqi_parallel_tests PRIVATE XQ_PARALLEL_TESTING)
    target_link_libraries(xiangqi_parallel_tests PRIVATE xiangqi)
    xq_enable_warnings(xiangqi_parallel_tests)
    if(MSVC)
        target_compile_options(xiangqi_parallel_tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(xiangqi_parallel_tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME xiangqi_parallel_tests COMMAND xiangqi_parallel_tests)
    set_tests_properties(xiangqi_parallel_tests PROPERTIES TIMEOUT 120)
    if(Python3_Interpreter_FOUND)
        add_test(NAME xiangqi_parallel_cli_tests
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_parallel_cli.py
                    $<TARGET_FILE:xiangqi_cli>)
    endif()
endif()
