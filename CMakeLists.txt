cmake_minimum_required(VERSION 3.20)
project(yadro VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep the old option name for compatibility with existing build commands.
# The fallback no longer uses FetchContent: under MSYS2/UCRT64 a nested
# FetchContent/Ninja build can mix cmd.exe and /bin/sh path semantics.
option(YADRO_FETCH_DEPS "Download pinned header-only dependencies when system packages are unavailable" ON)
option(YADRO_VENDOR_WEB_DEPS "Download pinned web dependencies into data/static/vendor for offline runtime" ON)
option(YADRO_WITH_CEF "Build REMOTION fullscreen Chromium shell using a local CEF binary distribution" OFF)
set(CEF_ROOT "" CACHE PATH "Path to an unpacked CEF binary distribution")

find_package(Threads REQUIRED)

set(YADRO_DEPS_DIR "${CMAKE_BINARY_DIR}/_deps/yadro_headers")

function(yadro_download_file url output_file)
    if(EXISTS "${output_file}")
        return()
    endif()

    get_filename_component(output_dir "${output_file}" DIRECTORY)
    file(MAKE_DIRECTORY "${output_dir}")
    set(tmp_file "${output_file}.download")

    message(STATUS "Downloading dependency: ${url}")
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
    yadro_download_file(
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
    yadro_download_file(
        "https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.15.3/httplib.h"
        "${YADRO_HTTPLIB_HEADER}"
    )
    add_library(yadro_httplib INTERFACE)
    target_include_directories(yadro_httplib INTERFACE
        "${YADRO_DEPS_DIR}/httplib/include")
    add_library(httplib::httplib ALIAS yadro_httplib)
endif()

# Bootstrap is copied into the static tree so the control panel has no CDN or
# Internet dependency at runtime. The version is pinned for reproducibility.
if(YADRO_VENDOR_WEB_DEPS)
    set(YADRO_BOOTSTRAP_DIR "${CMAKE_SOURCE_DIR}/data/static/vendor/bootstrap")
    yadro_download_file(
        "https://raw.githubusercontent.com/twbs/bootstrap/v5.3.3/dist/css/bootstrap.min.css"
        "${YADRO_BOOTSTRAP_DIR}/bootstrap.min.css"
    )
    yadro_download_file(
        "https://raw.githubusercontent.com/twbs/bootstrap/v5.3.3/dist/js/bootstrap.bundle.min.js"
        "${YADRO_BOOTSTRAP_DIR}/bootstrap.bundle.min.js"
    )
endif()

set(YADRO_CORE_SOURCES
    src/treadmill.cpp
    src/protocols.cpp
    src/web_server.cpp
)

add_executable(yadro
    src/main.cpp
    ${YADRO_CORE_SOURCES}
)

target_include_directories(yadro PRIVATE src)
target_link_libraries(yadro PRIVATE Threads::Threads nlohmann_json::nlohmann_json httplib::httplib)

if(WIN32)
    target_link_libraries(yadro PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_options(yadro PRIVATE /EHsc /W4 /permissive-)
else()
    target_compile_options(yadro PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(YADRO_WITH_CEF)
    if(CMAKE_VERSION VERSION_LESS 3.21)
        message(FATAL_ERROR "CEF integration requires CMake 3.21 or newer")
    endif()
    if(NOT CEF_ROOT OR NOT EXISTS "${CEF_ROOT}/cmake/FindCEF.cmake")
        message(FATAL_ERROR
            "YADRO_WITH_CEF=ON requires CEF_ROOT to point to an unpacked CEF binary distribution. "
            "Example: -DCEF_ROOT=C:/dev/cef_binary_..._windows64")
    endif()
    if(WIN32 AND NOT MSVC)
        message(FATAL_ERROR
            "The REMOTION CEF shell on Windows must be built with the Visual Studio 2022/MSVC toolchain. "
            "Keep using MSYS2/UCRT64 for the core-only yadro target, or configure a separate MSVC build for CEF.")
    endif()

    list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
    find_package(CEF REQUIRED)
    add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)

    add_executable(remotion
        src/remotion_shell.cpp
        ${YADRO_CORE_SOURCES}
    )
    target_include_directories(remotion PRIVATE src "${CEF_ROOT}")
    target_link_libraries(remotion PRIVATE
        Threads::Threads
        nlohmann_json::nlohmann_json
        httplib::httplib
        libcef_lib
        libcef_dll_wrapper
        ${CEF_STANDARD_LIBS}
    )
    add_dependencies(remotion libcef_dll_wrapper)
    SET_EXECUTABLE_TARGET_PROPERTIES(remotion)

    if(WIN32)
        target_link_libraries(remotion PRIVATE ws2_32)
    endif()
    if(MSVC)
        # CEF's default target properties disable exception unwinding, while
        # cpp-httplib uses C++ exceptions internally. Re-enable /EHsc on the
        # application target after applying the CEF properties; otherwise CEF's
        # /WX promotes httplib warning C4530 to error C2220.
        target_compile_options(remotion PRIVATE /EHsc /W4 /permissive-)
    else()
        target_compile_options(remotion PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # Copy the Chromium runtime beside remotion after build. The CEF binary
    # distribution provides these lists/macros through FindCEF.cmake.
    COPY_FILES(remotion "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}")
    COPY_FILES(remotion "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${CEF_TARGET_OUT_DIR}")

    install(TARGETS remotion RUNTIME DESTINATION .)
endif()

install(TARGETS yadro RUNTIME DESTINATION bin)
install(DIRECTORY data DESTINATION .)
