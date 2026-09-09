# SPDX-FileCopyrightText: 2026 maninblack
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)
project(brake_health_service LANGUAGES CXX)

add_library(brake_health_v1
    src/v1/messages.cpp
    src/v1/model.cpp
    src/v1/sha256.cpp
    src/v1/spool.cpp
    src/v1/window.cpp
)
target_include_directories(brake_health_v1 PUBLIC include)
target_compile_features(brake_health_v1 PUBLIC cxx_std_17)
set_target_properties(brake_health_v1 PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_STANDARD_REQUIRED ON
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(brake_health_v1 PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

find_package(Threads REQUIRED)
add_library(brake_health_runtime
    src/runtime/application.cpp
    src/runtime/json.cpp
    src/runtime/runtime.cpp
    src/runtime/posix.cpp
)
target_include_directories(brake_health_runtime PUBLIC include)
target_link_libraries(brake_health_runtime PUBLIC brake_health_v1 Threads::Threads)
target_compile_features(brake_health_runtime PUBLIC cxx_std_17)
set_target_properties(brake_health_runtime PROPERTIES CXX_EXTENSIONS OFF CXX_STANDARD_REQUIRED ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(brake_health_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(brake-health-bootstrap src/runtime/bootstrap_main.cpp)
target_link_libraries(brake-health-bootstrap PRIVATE brake_health_runtime)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(brake-health-bootstrap PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

option(BHS_BUILD_KUKSA_RUNTIME "Build actual KUKSA product executable (requires pinned dependencies)" OFF)
if(BHS_BUILD_KUKSA_RUNTIME)
    include(cmake/KuksaRuntime.cmake)
else()
    message(STATUS "Host/domain targets only: brake-health-service product executable is NOT built")
endif()

add_library(brake_health_v2
    src/v2/messages.cpp
    src/v2/model.cpp
    src/v2/state_store.cpp
)
target_include_directories(brake_health_v2 PUBLIC include)
target_link_libraries(brake_health_v2 PUBLIC brake_health_v1)
target_compile_features(brake_health_v2 PUBLIC cxx_std_17)
set_target_properties(brake_health_v2 PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_STANDARD_REQUIRED ON
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(brake_health_v2 PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(brake_health_v1_tests tests/cpp/v1/v1_tests.cpp)
    target_link_libraries(brake_health_v1_tests PRIVATE brake_health_v1)
    target_compile_features(brake_health_v1_tests PRIVATE cxx_std_17)
    set_target_properties(brake_health_v1_tests PROPERTIES
        CXX_EXTENSIONS OFF
        CXX_STANDARD_REQUIRED ON
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(brake_health_v1_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME brake_health_v1_contract COMMAND brake_health_v1_tests)

    add_executable(brake_health_v2_tests tests/cpp/v2/v2_tests.cpp)
    target_link_libraries(brake_health_v2_tests PRIVATE brake_health_v2)
    target_compile_features(brake_health_v2_tests PRIVATE cxx_std_17)
    set_target_properties(brake_health_v2_tests PROPERTIES
        CXX_EXTENSIONS OFF
        CXX_STANDARD_REQUIRED ON
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(brake_health_v2_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME brake_health_v2_contract COMMAND brake_health_v2_tests)

    add_executable(brake_health_runtime_tests tests/cpp/runtime/runtime_tests.cpp)
    target_link_libraries(brake_health_runtime_tests PRIVATE brake_health_runtime)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(brake_health_runtime_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME brake_health_runtime_contract COMMAND brake_health_runtime_tests)
    add_executable(brake_health_application_tests tests/cpp/runtime/application_tests.cpp)
    target_link_libraries(brake_health_application_tests PRIVATE brake_health_runtime)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(brake_health_application_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME brake_health_application_contract COMMAND brake_health_application_tests)
endif()
