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

if(MSVC)
    target_compile_options(xiangqi PRIVATE /W4)
else()
    target_compile_options(xiangqi PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(xiangqi_cli examples/cli.c)
target_link_libraries(xiangqi_cli PRIVATE xiangqi)

enable_testing()

add_executable(xiangqi_core_tests tests/test_core.c)
target_link_libraries(xiangqi_core_tests PRIVATE xiangqi)
add_test(NAME xiangqi_core_tests COMMAND xiangqi_core_tests)
