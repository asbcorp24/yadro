cmake_minimum_required(VERSION 3.20)
project(yadro VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep the old option name for compatibility with existing build commands.
# The fallback no longer uses FetchContent: under MSYS2/UCRT64 a nested
# FetchContent/Ninja build can mix cmd.exe and /bin/sh path semantics.
option(YADRO_FETCH_DEPS "Download pinned header-only dependencies when system packages are unavailable" ON)

find_package(Threads REQUIRED)

set(YADRO_DEPS_DIR "${CMAKE_BINARY_DIR}/_deps/yadro_headers")

function(yadro_download_header url output_file)
    if(EXISTS "${output_file}")
        return()
    endif()

    get_filename_component(output_dir "${output_file}" DIRECTORY)
    file(MAKE_DIRECTORY "${output_dir}")
    set(tmp_file "${output_file}.download")

    message(STATUS "Downloading dependency header: ${url}")
    file(DOWNLOAD
        "${url}"
        "${tmp_file}"
        STATUS download_status
        TLS_VERIFY ON
        SHOW_PROGRESS
    )
    list(GET download_status 0 download_code)
    list(GET download_status 1 download_message)
    if(NOT download_code EQUAL 0)
        file(REMOVE "${tmp_file}")
        message(FATAL_ERROR "Failed to download ${url}: ${download_message}")
    endif()
    file(RENAME "${tmp_file}" "${output_file}")
endfunction()

find_package(nlohmann_json 3.11 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    if(NOT YADRO_FETCH_DEPS)
        message(FATAL_ERROR
            "nlohmann_json not found. On MSYS2/UCRT64 install it with: "
            "pacman -S mingw-w64-ucrt-x86_64-nlohmann-json")
    endif()

    set(YADRO_JSON_HEADER "${YADRO_DEPS_DIR}/nlohmann/include/nlohmann/json.hpp")
    yadro_download_header(
        "https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp"
        "${YADRO_JSON_HEADER}"
    )
    add_library(yadro_nlohmann_json INTERFACE)
    target_include_directories(yadro_nlohmann_json INTERFACE
        "${YADRO_DEPS_DIR}/nlohmann/include")
    add_library(nlohmann_json::nlohmann_json ALIAS yadro_nlohmann_json)
endif()

find_package(httplib CONFIG QUIET)
if(NOT httplib_FOUND)
    if(NOT YADRO_FETCH_DEPS)
        message(FATAL_ERROR
            "cpp-httplib not found and dependency downloads are disabled. "
            "Enable YADRO_FETCH_DEPS or provide httplibConfig.cmake.")
    endif()

    set(YADRO_HTTPLIB_HEADER "${YADRO_DEPS_DIR}/httplib/include/httplib.h")
    yadro_download_header(
        "https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.15.3/httplib.h"
        "${YADRO_HTTPLIB_HEADER}"
    )
    add_library(yadro_httplib INTERFACE)
    target_include_directories(yadro_httplib INTERFACE
        "${YADRO_DEPS_DIR}/httplib/include")
    add_library(httplib::httplib ALIAS yadro_httplib)
endif()

add_executable(yadro
    src/main.cpp
    src/treadmill.cpp
    src/protocols.cpp
    src/web_server.cpp
)

target_include_directories(yadro PRIVATE src)
target_link_libraries(yadro PRIVATE Threads::Threads nlohmann_json::nlohmann_json httplib::httplib)

if(WIN32)
    target_link_libraries(yadro PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_options(yadro PRIVATE /W4 /permissive-)
else()
    target_compile_options(yadro PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS yadro RUNTIME DESTINATION bin)
install(DIRECTORY data DESTINATION .)
