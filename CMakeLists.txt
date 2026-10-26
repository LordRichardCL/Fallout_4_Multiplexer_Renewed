cmake_minimum_required(VERSION 3.21)

project(Fallout4MultiplexerRenewed
    VERSION 0.1.0
    DESCRIPTION "Fallout 4 mod compilation, conflict resolution and load order intelligence platform"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Fallout 4 Multiplexer Renewed is 64-bit only.")
endif()

option(FO4MX_BUILD_GUI   "Build the Qt 6 desktop application" ON)
option(FO4MX_BUILD_CLI   "Build the fo4mx command line tool"  ON)
option(FO4MX_BUILD_TESTS "Build the unit test suite"          ON)

include(cmake/CompilerWarnings.cmake)

find_package(ZLIB REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_subdirectory(src/core)

if(FO4MX_BUILD_CLI)
    add_subdirectory(src/cli)
endif()

if(FO4MX_BUILD_GUI)
    add_subdirectory(src/gui)
endif()

if(FO4MX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
