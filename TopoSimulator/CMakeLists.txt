cmake_minimum_required(VERSION 3.22)

project(TopoSimulator VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_library(toposim
    src/BgpRouter.cpp
    src/BgpTypes.cpp
    src/BmpCollector.cpp
    src/ThreadPool.cpp
    src/TopoManager.cpp
)

target_include_directories(toposim
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(toposim
    PUBLIC
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)

if(MSVC)
    target_compile_options(toposim PRIVATE /W4 /permissive-)
else()
    target_compile_options(toposim PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(TopoSimulator src/main.cpp)
target_link_libraries(TopoSimulator PRIVATE toposim)

add_custom_command(TARGET TopoSimulator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/topo
        $<TARGET_FILE_DIR:TopoSimulator>/topo
)
