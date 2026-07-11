cmake_minimum_required(VERSION 3.25)

project(BgpTester VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network Sql)

qt_standard_project_setup()

add_library(bgptester_core STATIC
    src/model/Topology.cpp
    src/engine/SimulationEngine.cpp
    src/plugin/RouterPluginRegistry.cpp
    src/plugin/StandardBgpRouterPlugin.cpp
    src/persistence/EventStore.cpp
)

target_include_directories(bgptester_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(bgptester_core
    PUBLIC
        Qt6::Core
        Qt6::Network
        Qt6::Sql
)

qt_add_executable(BgpTester WIN32
    src/main.cpp
    src/ui/Dialogs.cpp
    src/ui/EventTableModel.cpp
    src/ui/MainWindow.cpp
    src/ui/TopologyScene.cpp
)

target_link_libraries(BgpTester
    PRIVATE
        bgptester_core
        Qt6::Gui
        Qt6::Widgets
)

if(MSVC)
    target_compile_options(bgptester_core PRIVATE /W4 /permissive-)
    target_compile_options(BgpTester PRIVATE /W4 /permissive-)
else()
    target_compile_options(bgptester_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(BgpTester PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(BgpTester PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_custom_command(TARGET BgpTester POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:BgpTester>/plugins/routers"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/topo"
        "$<TARGET_FILE_DIR:BgpTester>/topo"
)

include(CTest)
if(BUILD_TESTING)
    add_library(BgpTesterExampleRouterPlugin MODULE
        examples/router-plugin/ConfigurableExportRouterPlugin.cpp
    )
    target_include_directories(BgpTesterExampleRouterPlugin PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(BgpTesterExampleRouterPlugin PRIVATE Qt6::Core)
    set_target_properties(BgpTesterExampleRouterPlugin PROPERTIES
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-plugins"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-plugins"
    )

    add_executable(BgpTesterTests tests/CoreTests.cpp)
    target_link_libraries(BgpTesterTests PRIVATE bgptester_core)
    add_dependencies(BgpTesterTests BgpTesterExampleRouterPlugin)
    target_compile_definitions(BgpTesterTests PRIVATE
        BGPTESTER_TEST_PLUGIN_PATH="$<TARGET_FILE:BgpTesterExampleRouterPlugin>"
    )
    if(MSVC)
        target_compile_options(BgpTesterTests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(BgpTesterTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME BgpTesterTests COMMAND BgpTesterTests)
endif()
