cmake_minimum_required(VERSION 3.25)
project(AdminExecMCP VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# vcpkg integration (set VCPKG_ROOT or pass -DCMAKE_TOOLCHAIN_FILE)
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)

add_executable(AdminExecMCP
    src/main.cpp
    src/service.cpp
    src/tcp_server.cpp
    src/command_executor.cpp
    src/telegram_approver.cpp
    src/config.cpp
)

target_include_directories(AdminExecMCP PRIVATE src)

target_link_libraries(AdminExecMCP PRIVATE
    nlohmann_json::nlohmann_json
    CURL::libcurl
    ws2_32        # Winsock2
    advapi32      # Windows service APIs
)

# Require Windows
if(NOT WIN32)
    message(FATAL_ERROR "AdminExecMCP only supports Windows.")
endif()

target_compile_definitions(AdminExecMCP PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

install(TARGETS AdminExecMCP DESTINATION bin)
install(FILES "${CMAKE_SOURCE_DIR}/config.example.json" DESTINATION bin RENAME config.json)
