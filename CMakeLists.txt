cmake_minimum_required(VERSION 3.16)

project(chess_z C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(xiangqi
    src/position.c
    src/movegen.c
    src/engine.c
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

add_executable(xiangqi_cli examples/cli.c)
target_link_libraries(xiangqi_cli PRIVATE xiangqi)

add_executable(xiangqi_generate_positions examples/generate_positions.c)
target_link_libraries(xiangqi_generate_positions PRIVATE xiangqi)
xq_enable_warnings(xiangqi_generate_positions)

add_executable(xiangqi_search_positions examples/search_positions.c)
target_link_libraries(xiangqi_search_positions PRIVATE xiangqi)
xq_enable_warnings(xiangqi_search_positions)

enable_testing()

add_executable(xiangqi_core_tests tests/test_core.c)
target_link_libraries(xiangqi_core_tests PRIVATE xiangqi)
add_test(NAME xiangqi_core_tests COMMAND xiangqi_core_tests)
