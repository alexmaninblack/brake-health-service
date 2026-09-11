# SPDX-FileCopyrightText: 2026 maninblack
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)
project(brake_health_service LANGUAGES CXX)
set(BHS_FUNCTIONAL_PROFILE "v1" CACHE STRING "Explicit immutable product content profile: v1, v2 or v3")
set_property(CACHE BHS_FUNCTIONAL_PROFILE PROPERTY STRINGS v1 v2 v3)
if(NOT BHS_FUNCTIONAL_PROFILE MATCHES "^v[123]$")
    message(FATAL_ERROR "BHS_FUNCTIONAL_PROFILE must be v1, v2 or v3; it is never inferred from a release")
endif()

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
    src/runtime/advisory_runtime.cpp
    src/runtime/application.cpp
    src/runtime/token_session.cpp
    src/runtime/derived_delivery.cpp
    src/runtime/json.cpp
    src/runtime/model_capture.cpp
    src/runtime/product.cpp
    src/runtime/runtime.cpp
    src/runtime/posix.cpp
    src/v3/advisory.cpp
    src/v3/messages.cpp
)
target_include_directories(brake_health_runtime PUBLIC include)
target_link_libraries(brake_health_runtime PUBLIC brake_health_v1 Threads::Threads)
target_compile_features(brake_health_runtime PUBLIC cxx_std_17)
target_compile_definitions(brake_health_runtime PUBLIC BHS_FUNCTIONAL_PROFILE="${BHS_FUNCTIONAL_PROFILE}")
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
target_link_libraries(brake_health_runtime PUBLIC brake_health_v2)
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
    add_executable(brake_demo_mock_tests tests/demo_mock_tests.cpp)
    target_link_libraries(brake_demo_mock_tests PRIVATE brake_health_runtime)
    target_compile_options(brake_demo_mock_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME brake_demo_mock_isolation COMMAND brake_demo_mock_tests)
    add_executable(native_inputs_tests tests/native_inputs_tests.cpp)
    target_link_libraries(native_inputs_tests PRIVATE brake_health_runtime)
    target_compile_options(native_inputs_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_service_inputs COMMAND native_inputs_tests)
    add_executable(brake_token_session_tests tests/token_session_tests.cpp)
    target_link_libraries(brake_token_session_tests PRIVATE brake_health_runtime)
    target_compile_options(brake_token_session_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME brake_private_token_session COMMAND brake_token_session_tests)
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
    add_executable(brake_health_application_tests tests/cpp/runtime/application_tests.cpp tests/cpp/runtime/product_tests.cpp)
    target_link_libraries(brake_health_application_tests PRIVATE brake_health_runtime)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(brake_health_application_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME brake_health_application_contract COMMAND brake_health_application_tests)
endif()
