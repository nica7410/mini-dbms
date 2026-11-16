cmake_minimum_required(VERSION 3.21)

project(minidb_phase3 VERSION 1.0.0 LANGUAGES C CXX)

# Phase 1/2 sources remain C translation units. Only this server is C++.
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MINIDB_BUILD_HTTP "Build the Drogon HTTP server and API tests" ON)
option(MINIDB_FETCH_DROGON "Fetch the pinned Drogon source instead of an installed package" ON)
option(MINIDB_BUILD_BENCHMARKS "Build a separate serialized-DB benchmark server" OFF)
option(MINIDB_ENABLE_ASAN_UBSAN "Instrument project code with AddressSanitizer and UBSan" OFF)
option(MINIDB_ENABLE_TSAN "Instrument project code with ThreadSanitizer" OFF)

if(MINIDB_ENABLE_ASAN_UBSAN AND MINIDB_ENABLE_TSAN)
    message(FATAL_ERROR "ASan/UBSan and TSan require separate build directories.")
endif()
if((MINIDB_ENABLE_ASAN_UBSAN OR MINIDB_ENABLE_TSAN)
   AND NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "These sanitizer options require GCC or Clang.")
endif()

include(CTest)
find_package(Threads REQUIRED)

function(minidb_project_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    endif()

    # Do not change third-party library flags or the original Makefile.
    if(MINIDB_ENABLE_ASAN_UBSAN)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer
            -fno-sanitize-recover=all -O1 -g)
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-sanitize-recover=all)
    elseif(MINIDB_ENABLE_TSAN)
        target_compile_options(${target} PRIVATE
            -fsanitize=thread -fno-omit-frame-pointer -O1 -g)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    endif()
endfunction()

get_filename_component(MINIDB_REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
add_library(minidb_core STATIC
    "${MINIDB_REPOSITORY_ROOT}/src/common.c"
    "${MINIDB_REPOSITORY_ROOT}/src/parser.c"
    "${MINIDB_REPOSITORY_ROOT}/src/storage.c"
    "${MINIDB_REPOSITORY_ROOT}/src/executor.c"
    "${MINIDB_REPOSITORY_ROOT}/src/bptree.c")
target_include_directories(minidb_core PUBLIC "${MINIDB_REPOSITORY_ROOT}/src")
target_compile_options(minidb_core PRIVATE
    "$<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wstrict-prototypes>")
if(MINGW)
    target_compile_definitions(minidb_core PRIVATE __USE_MINGW_ANSI_STDIO=1)
endif()
minidb_project_options(minidb_core)

add_library(minidb_server_engine STATIC engine.cpp)
target_include_directories(minidb_server_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(minidb_server_engine PUBLIC minidb_core Threads::Threads)
minidb_project_options(minidb_server_engine)

add_library(minidb_thread_pool STATIC thread_pool.cpp)
target_include_directories(minidb_thread_pool PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(minidb_thread_pool PUBLIC Threads::Threads)
minidb_project_options(minidb_thread_pool)

if(MINIDB_BUILD_HTTP)
    include(cmake/DrogonDependency.cmake)
    minidb_find_drogon()

    add_executable(minidb_server main.cpp)
    target_link_libraries(minidb_server PRIVATE
        minidb_server_engine minidb_thread_pool Drogon::Drogon)
    minidb_project_options(minidb_server)

    if(MINIDB_BUILD_BENCHMARKS)
        add_library(minidb_serial_engine STATIC engine.cpp)
        target_compile_definitions(minidb_serial_engine PRIVATE MINIDB_BENCH_SERIAL=1)
        target_include_directories(minidb_serial_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(minidb_serial_engine PUBLIC minidb_core Threads::Threads)
        minidb_project_options(minidb_serial_engine)
        add_executable(minidb_server_serial main.cpp)
        target_link_libraries(minidb_server_serial PRIVATE minidb_serial_engine minidb_thread_pool Drogon::Drogon)
        minidb_project_options(minidb_server_serial)

        # Arrival-order (ticket) gate candidate for the fairness comparison.
        add_library(minidb_fair_engine STATIC engine.cpp)
        target_compile_definitions(minidb_fair_engine PRIVATE MINIDB_BENCH_FAIR=1)
        target_include_directories(minidb_fair_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(minidb_fair_engine PUBLIC minidb_core Threads::Threads)
        minidb_project_options(minidb_fair_engine)
        add_executable(minidb_server_fair main.cpp)
        target_link_libraries(minidb_server_fair PRIVATE minidb_fair_engine minidb_thread_pool Drogon::Drogon)
        minidb_project_options(minidb_server_fair)
    endif()

    include(GNUInstallDirs)
    install(TARGETS minidb_server RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

if(BUILD_TESTING)
    add_executable(minidb_gate_test tests/gate_test.cpp)
    target_link_libraries(minidb_gate_test PRIVATE minidb_server_engine)
    minidb_project_options(minidb_gate_test)
    add_test(NAME phase3_gate COMMAND minidb_gate_test)
    set_tests_properties(phase3_gate PROPERTIES TIMEOUT 30)

    add_executable(minidb_engine_test tests/engine_test.cpp)
    target_link_libraries(minidb_engine_test PRIVATE minidb_server_engine)
    minidb_project_options(minidb_engine_test)
    add_test(NAME phase3_engine COMMAND minidb_engine_test)
    set_tests_properties(phase3_engine PROPERTIES TIMEOUT 90)

    add_executable(minidb_pool_test tests/pool_test.cpp)
    target_link_libraries(minidb_pool_test PRIVATE minidb_thread_pool)
    minidb_project_options(minidb_pool_test)
    add_test(NAME phase3_pool COMMAND minidb_pool_test)
    set_tests_properties(phase3_pool PROPERTIES TIMEOUT 30)

    # Link wrapping and inherited POSIX pipe descriptors are test-only facilities.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_library(minidb_test_engine STATIC engine.cpp)
        target_compile_definitions(minidb_test_engine PRIVATE MINIDB_TEST_ACCESS=1)
        target_include_directories(minidb_test_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(minidb_test_engine PUBLIC minidb_core Threads::Threads)
        minidb_project_options(minidb_test_engine)
        add_executable(minidb_engine_gate_test tests/engine_gate_test.cpp)
        target_link_libraries(minidb_engine_gate_test PRIVATE minidb_test_engine)
        target_link_options(minidb_engine_gate_test PRIVATE "-Wl,--wrap=executor_run")
        minidb_project_options(minidb_engine_gate_test)
        add_test(NAME phase3_engine_gate COMMAND minidb_engine_gate_test)
        set_tests_properties(phase3_engine_gate PROPERTIES TIMEOUT 30)
    else()
        message(STATUS "Skipping POSIX/link-wrapper integration tests on this platform")
    endif()

    if(MINIDB_BUILD_HTTP)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME phase3_api
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/api_test.py"
                "$<TARGET_FILE:minidb_server>")
        set_tests_properties(phase3_api PROPERTIES TIMEOUT 120)
        if(TARGET minidb_test_engine)
            add_executable(minidb_server_test main.cpp tests/api_executor_control.cpp)
            target_link_libraries(minidb_server_test PRIVATE
                minidb_server_engine minidb_thread_pool Drogon::Drogon)
            target_link_options(minidb_server_test PRIVATE "-Wl,--wrap=executor_run")
            minidb_project_options(minidb_server_test)
            add_test(NAME phase3_control_api
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/control_api_test.py"
                    "$<TARGET_FILE:minidb_server_test>")
            set_tests_properties(phase3_control_api PROPERTIES TIMEOUT 60)
        endif()
    endif()
endif()
