cmake_minimum_required(VERSION 3.16)

project(read_training
    VERSION 0.1.0
    DESCRIPTION "Small C library for read_training core functions"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(read_training
    src/read_training.c
)

target_include_directories(read_training
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

if(MSVC)
    target_compile_options(read_training PRIVATE /W4)
else()
    target_compile_options(read_training PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()

add_executable(rd_tr_result_tests
    tests/t07_result/test_t07_result.c
)

target_link_libraries(rd_tr_result_tests
    PRIVATE read_training
)

target_compile_definitions(rd_tr_result_tests
    PRIVATE
        READ_TRAINING_EXPOSE_TEST_HELPERS
        RESULT_LOG_PATH="${CMAKE_CURRENT_BINARY_DIR}/t07_result_visual_log.txt"
)

add_test(NAME rd_tr_result_tests COMMAND rd_tr_result_tests)
