cmake_minimum_required(VERSION 3.20)
project(yadro VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(YADRO_FETCH_DEPS "Fetch header-only dependencies when system packages are unavailable" ON)

include(FetchContent)
find_package(Threads REQUIRED)

find_package(nlohmann_json 3.11 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    if(NOT YADRO_FETCH_DEPS)
        message(FATAL_ERROR "nlohmann_json not found. Install it or enable YADRO_FETCH_DEPS")
    endif()
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

find_package(httplib CONFIG QUIET)
if(NOT httplib_FOUND)
    if(NOT YADRO_FETCH_DEPS)
        message(FATAL_ERROR "cpp-httplib not found. Install it or enable YADRO_FETCH_DEPS")
    endif()
    FetchContent_Declare(
        httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG v0.15.3
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(httplib)
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
