cmake_minimum_required(VERSION 3.16)
project(wlclip C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(WLCLIP_STATIC "Link C++ runtime statically where possible" OFF)
option(WLCLIP_USE_SYSTEM_SPDLOG "Use system-installed spdlog instead of submodule" OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_compile_options(-Wall -Wextra -Wpedantic)

# ---- 3rd-party: spdlog --------------------------------------------------
if(WLCLIP_USE_SYSTEM_SPDLOG)
    find_package(spdlog REQUIRED)
else()
    set(WLCLIP_SPDLOG_DIR ${CMAKE_SOURCE_DIR}/3rdparty/spdlog)
    if(NOT EXISTS ${WLCLIP_SPDLOG_DIR}/CMakeLists.txt)
        message(FATAL_ERROR
            "3rdparty/spdlog is empty. Initialise the submodule:\n"
            "    git submodule update --init --recursive\n"
            "Or pass -DWLCLIP_USE_SYSTEM_SPDLOG=ON to use a system install.")
    endif()
    set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(${WLCLIP_SPDLOG_DIR} ${CMAKE_BINARY_DIR}/_spdlog EXCLUDE_FROM_ALL)
endif()

# ---- Wayland scanner + protocol code ------------------------------------
include(cmake/WaylandProtocols.cmake)

# ---- Sources ------------------------------------------------------------
add_subdirectory(src)
