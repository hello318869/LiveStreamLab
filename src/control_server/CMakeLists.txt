cmake_minimum_required(VERSION 3.24)
project(LiveStreamLabControlServer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The control server targets Linux/WSL. Configure this directory from Linux.")
endif()

# Can be configured directly or included by the top-level server build option.
option(BUILD_TESTING "Build dependency tests" OFF)
option(LSL_BUILD_TESTS "Build project tests" OFF)
option(LSL_SERVER_FETCH_DROGON "Fetch the pinned Drogon source using CMake" OFF)
if(BUILD_TESTING)
    message(FATAL_ERROR "Dependency tests are not supported; leave BUILD_TESTING=OFF and use LSL_BUILD_TESTS for project tests.")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium=1.0.18)
pkg_check_modules(MARIADB REQUIRED IMPORTED_TARGET libmariadb=3.3.17)
pkg_check_modules(LSL_JSONCPP REQUIRED jsoncpp=1.9.5)

if(LSL_SERVER_FETCH_DROGON)
    include(FetchContent)
    set(BUILD_CTL OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_ORM ON CACHE BOOL "" FORCE)
    set(BUILD_MYSQL ON CACHE BOOL "" FORCE)
    set(BUILD_POSTGRESQL OFF CACHE BOOL "" FORCE)
    set(BUILD_SQLITE OFF CACHE BOOL "" FORCE)
    set(BUILD_REDIS OFF CACHE BOOL "" FORCE)
    set(BUILD_BROTLI OFF CACHE BOOL "" FORCE)
    set(BUILD_YAML_CONFIG OFF CACHE BOOL "" FORCE)
    set(BUILD_DOC OFF CACHE BOOL "" FORCE)
    set(USE_SUBMODULE ON CACHE BOOL "" FORCE)
    FetchContent_Declare(drogon
        GIT_REPOSITORY https://github.com/drogonframework/drogon.git
        GIT_TAG a22956b82b6b221ceeff83913c3014ce0d048555
        GIT_SUBMODULES trantor
        GIT_SUBMODULES_RECURSE FALSE)
    FetchContent_MakeAvailable(drogon)
    set(lsl_drogon_target drogon)
else()
    find_package(Drogon 1.9.11 EXACT CONFIG REQUIRED)
    set(lsl_drogon_target Drogon::Drogon)
endif()

add_executable(control_server
    ../apps/control_server_main.cpp
    Server.cpp
    common/Support.cpp
    config/ServerConfig.cpp
    controllers/ApiResponse.cpp
    controllers/HealthController.cpp
    controllers/AccountController.cpp
    controllers/LiveController.cpp
    filters/AuthFilter.cpp
    repositories/AccountRepository.cpp
    repositories/LiveRepository.cpp
    services/AccountService.cpp
    services/LiveService.cpp
    srs/SrsClient.cpp)
target_include_directories(control_server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(control_server PRIVATE ${lsl_drogon_target} PkgConfig::SODIUM)
target_compile_options(control_server PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(control_server PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(LSL_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)
    add_executable(srs_client_tests
        ../../tests/unit/srs_client_tests.cpp
        srs/SrsClient.cpp)
    target_include_directories(srs_client_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_link_libraries(srs_client_tests PRIVATE ${lsl_drogon_target} Threads::Threads)
    target_compile_options(srs_client_tests PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(srs_client_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_test(NAME unit/srs_client COMMAND srs_client_tests)
    set_tests_properties(unit/srs_client PROPERTIES TIMEOUT 10)
endif()

install(TARGETS control_server RUNTIME DESTINATION bin)
install(FILES ../../config/server.example.json DESTINATION share/livestreamlab)
install(FILES
    ../../deploy/README.md
    ../../deploy/server.lan.example.json
    ../../deploy/control-server.env.example
    ../../deploy/srs-platform.conf
    ../../deploy/srs-http-hooks-redaction.patch
    DESTINATION share/livestreamlab/deploy)
install(FILES
    ../../deploy/systemd/livestreamlab-control.service
    ../../deploy/systemd/livestreamlab-srs.service
    DESTINATION share/livestreamlab/deploy/systemd)
install(FILES
    ../../deploy/database/README.md
    ../../deploy/database/001_initial_schema.sql
    ../../deploy/database/002_publish_connection.sql
    DESTINATION share/livestreamlab/deploy/database)
install(FILES
    ../../docs/platform-api.md
    ../../docs/platform-client.md
    ../../docs/platform-deployment.md
    ../../docs/platform-recovery.md
    ../../docs/platform-srs-baseline.md
    DESTINATION share/livestreamlab/docs)
