cmake_minimum_required(VERSION 3.22)

project(TopoSimulator VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "TopoSimulator only supports Windows.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "TopoSimulator only supports the MSVC compiler.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

add_library(toposim
    src/BmpLogManager.cpp
    src/BgpRouter.cpp
    src/BgpTypes.cpp
    src/RouterFactory.cpp
    src/ThreadPool.cpp
    src/TopoManager.cpp
)

target_include_directories(toposim
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(toposim PRIVATE /W4 /permissive-)
target_link_libraries(toposim PRIVATE unofficial::sqlite3::sqlite3)

add_library(toposim_io
    src/TopologyObserverServer.cpp
    src/TopologyJson.cpp
)

target_link_libraries(toposim_io
    PUBLIC
        toposim
        nlohmann_json::nlohmann_json
)

target_compile_options(toposim_io PRIVATE /W4 /permissive-)

add_executable(TopoSimulator
    src/main.cpp
    src/BmpLogViewer.cpp
)
target_link_libraries(TopoSimulator
    PRIVATE
        toposim_io
        spdlog::spdlog
        imgui::imgui
        d3d11
        dxgi
)
target_link_options(TopoSimulator PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:toposim>")
target_compile_options(TopoSimulator PRIVATE /W4 /permissive-)

add_custom_command(TARGET TopoSimulator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/topo
        $<TARGET_FILE_DIR:TopoSimulator>/topo
)

add_executable(BmpLogViewer
    src/BmpLogViewerMain.cpp
    src/BmpLogViewer.cpp
)
target_link_libraries(BmpLogViewer
    PRIVATE
        toposim
        imgui::imgui
        d3d11
        dxgi
        comdlg32
)
target_compile_options(BmpLogViewer PRIVATE /W4 /permissive-)

add_executable(TopoSimulatorTests tests/core_tests.cpp)
target_link_libraries(TopoSimulatorTests PRIVATE toposim)
target_link_options(TopoSimulatorTests PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:toposim>")
target_compile_options(TopoSimulatorTests PRIVATE /W4 /permissive-)

add_test(NAME TopoSimulatorTests COMMAND TopoSimulatorTests)
