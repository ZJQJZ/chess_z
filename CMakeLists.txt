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
        add_test(NAME search_sample_report_reader COMMAND ${XQ_VIEWER_NODE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/search_sample_report_reader_tests.js)
        add_test(NAME search_sample_viewer_ui COMMAND ${XQ_VIEWER_NODE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/search_sample_viewer_ui_tests.js)
        add_test(NAME search_sample_timeline COMMAND ${XQ_VIEWER_NODE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/search_sample_timeline_tests.js)
        add_test(NAME search_sample_timeline_ui COMMAND ${XQ_VIEWER_NODE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/search_sample_timeline_ui_tests.js)
        set_tests_properties(search_sample_viewer_model search_sample_report_reader search_sample_viewer_ui
            search_sample_timeline search_sample_timeline_ui PROPERTIES TIMEOUT 30)
    endif()
endif()
if(BUILD_TESTING AND NOT WIN32)
    # White-box tests include generated copies with test-only enum values.
    set(similarity_source_path "${CMAKE_CURRENT_SOURCE_DIR}/src/parallel_search.c")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${similarity_source_path}")
    file(READ "${similarity_source_path}" similarity_source)
    foreach(similarity_constant IN ITEMS
            PARALLEL_TT_BUCKET_COUNT PARALLEL_MAX_SIMILAR_POSITION_QUERIES)
        string(REGEX MATCHALL "\n[ \t]*${similarity_constant}[ \t]*=[^,\r\n]*,"
            similarity_declarations "${similarity_source}")
        list(LENGTH similarity_declarations similarity_declaration_count)
        if(NOT similarity_declaration_count EQUAL 1)
            message(FATAL_ERROR "Expected one enum initializer for ${similarity_constant}")
        endif()
    endforeach()
    string(REGEX REPLACE "(\n[ \t]*PARALLEL_TT_BUCKET_COUNT[ \t]*=)[^,\r\n]*,"
        "\\1 64," similarity_source "${similarity_source}")
    # Keep the support library static even when the main library is shared.
    set(XQ_SIMILARITY_SUPPORT_SOURCES ${XIANGQI_SOURCES})
    list(REMOVE_ITEM XQ_SIMILARITY_SUPPORT_SOURCES src/parallel_search.c)
    add_library(xiangqi_similarity_test_support STATIC ${XQ_SIMILARITY_SUPPORT_SOURCES})
    target_include_directories(xiangqi_similarity_test_support PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(xiangqi_similarity_test_support PUBLIC Threads::Threads)
    xq_enable_warnings(xiangqi_similarity_test_support)
    foreach(query_limit IN ITEMS 2 0)
        set(similarity_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/parallel_similarity_${query_limit}")
        file(MAKE_DIRECTORY "${similarity_generated_dir}")
        string(REGEX REPLACE "(\n[ \t]*PARALLEL_MAX_SIMILAR_POSITION_QUERIES[ \t]*=)[^,\r\n]*,"
            "\\1 ${query_limit}," similarity_test_source "${similarity_source}")
        file(WRITE "${similarity_generated_dir}/parallel_search_under_test.c" "${similarity_test_source}")
        set(similarity_target xiangqi_parallel_similarity_tests_${query_limit})
        add_executable(${similarity_target} tests/parallel_similarity_tests.c)
        target_include_directories(${similarity_target} PRIVATE
            ${similarity_generated_dir} ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_definitions(${similarity_target} PRIVATE
            XQ_TEST_SIMILARITY_QUERY_LIMIT=${query_limit})
        target_link_libraries(${similarity_target} PRIVATE xiangqi_similarity_test_support)
        xq_enable_warnings(${similarity_target})
        if(query_limit EQUAL 0)
            set(similarity_test_name parallel_similarity_disabled)
        else()
            set(similarity_test_name parallel_similarity)
        endif()
        add_test(NAME ${similarity_test_name} COMMAND ${similarity_target})
        set_tests_properties(${similarity_test_name} PROPERTIES TIMEOUT 30)
    endforeach()

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
    add_executable(xiangqi_timeline_report_tests tests/timeline_report_tests.c)
    target_include_directories(xiangqi_timeline_report_tests PRIVATE examples)
    target_link_libraries(xiangqi_timeline_report_tests PRIVATE xiangqi)
    xq_enable_warnings(xiangqi_timeline_report_tests)
    foreach(timeline_variant IN ITEMS normal overflow)
        if(timeline_variant STREQUAL "overflow")
            set(timeline_collector_target xiangqi_timeline_collector_overflow_tests)
            set(timeline_collector_test timeline_collector_overflow)
            set(timeline_collector_arguments overflow)
        else()
            set(timeline_collector_target xiangqi_timeline_collector_tests)
            set(timeline_collector_test timeline_collector)
            set(timeline_collector_arguments)
        endif()
        set(timeline_collector_reports "${CMAKE_CURRENT_BINARY_DIR}/${timeline_collector_test}_reports")
        add_executable(${timeline_collector_target} tests/timeline_collector_tests.c
            examples/search_progress.c examples/search_sample_report.c)
        target_include_directories(${timeline_collector_target} PRIVATE examples)
        target_link_libraries(${timeline_collector_target} PRIVATE xiangqi)
        xq_enable_warnings(${timeline_collector_target})
        if(timeline_variant STREQUAL "overflow")
            target_compile_definitions(${timeline_collector_target} PRIVATE
                CLI_SAMPLE_QUEUE_CAPACITY=2 CLI_SAMPLE_BATCH_CAPACITY=1)
        endif()
        add_test(NAME ${timeline_collector_test} COMMAND ${timeline_collector_target}
            ${timeline_collector_reports} ${timeline_collector_arguments})
        set_tests_properties(${timeline_collector_test} PROPERTIES TIMEOUT 45
            FIXTURES_SETUP ${timeline_collector_test}_data)
        if(Python3_Interpreter_FOUND)
            add_test(NAME ${timeline_collector_test}_format COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_timeline_collector.py
                ${timeline_collector_reports})
            set_tests_properties(${timeline_collector_test}_format PROPERTIES TIMEOUT 20
                FIXTURES_REQUIRED ${timeline_collector_test}_data)
        endif()
    endforeach()
    if(Python3_Interpreter_FOUND)
        add_test(NAME sampling_report_format COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sampling_reports.py
            $<TARGET_FILE:xiangqi_sampling_report_tests>)
        set_tests_properties(sampling_report_format PROPERTIES TIMEOUT 20)
        add_test(NAME timeline_report_format COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_timeline_reports.py
            $<TARGET_FILE:xiangqi_timeline_report_tests>)
        set_tests_properties(timeline_report_format PROPERTIES TIMEOUT 30)
    endif()
endif()
