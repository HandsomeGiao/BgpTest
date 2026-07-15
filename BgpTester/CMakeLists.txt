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

# Source plugins are discovered automatically. Adding a .hpp/.cpp pair under
# src/router_plugins is enough; CONFIGURE_DEPENDS makes the next normal build
# rerun CMake when files are added or removed.
file(GLOB_RECURSE BGPTESTER_ROUTER_PLUGIN_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/router_plugins/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/router_plugins/*.hpp"
)

add_library(bgptester_router_plugins OBJECT
    ${BGPTESTER_ROUTER_PLUGIN_SOURCES}
)
target_include_directories(bgptester_router_plugins PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(bgptester_router_plugins PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Sql
)

add_library(bgptester_core STATIC
    src/model/Topology.cpp
    src/engine/SimulationEngine.cpp
    src/plugin/RouterPluginRegistry.cpp
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
    src/ui/RibTableModels.cpp
    src/ui/TopologyScene.cpp
)
target_sources(BgpTester PRIVATE
    $<TARGET_OBJECTS:bgptester_router_plugins>
)

target_link_libraries(BgpTester
    PRIVATE
        bgptester_core
        Qt6::Gui
        Qt6::Widgets
)

if(MSVC)
    target_compile_options(bgptester_router_plugins PRIVATE /W4 /permissive-)
    target_compile_options(bgptester_core PRIVATE /W4 /permissive-)
    target_compile_options(BgpTester PRIVATE /W4 /permissive-)
else()
    target_compile_options(bgptester_router_plugins PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_options(bgptester_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(BgpTester PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(BgpTester PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_custom_command(TARGET BgpTester POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/topo"
        "$<TARGET_FILE_DIR:BgpTester>/topo"
)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)

    add_executable(BgpTesterTests tests/CoreTests.cpp)
    target_sources(BgpTesterTests PRIVATE
        $<TARGET_OBJECTS:bgptester_router_plugins>
    )
    target_link_libraries(BgpTesterTests PRIVATE bgptester_core)
    if(MSVC)
        target_compile_options(BgpTesterTests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(BgpTesterTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME BgpTesterTests COMMAND BgpTesterTests)

    add_executable(BgpTesterUiTests
        tests/UiTests.cpp
        src/ui/Dialogs.cpp
        src/ui/EventTableModel.cpp
        src/ui/MainWindow.cpp
        src/ui/RibTableModels.cpp
        src/ui/TopologyScene.cpp
    )
    target_sources(BgpTesterUiTests PRIVATE
        $<TARGET_OBJECTS:bgptester_router_plugins>
    )
    target_link_libraries(BgpTesterUiTests
        PRIVATE
            bgptester_core
            Qt6::Gui
            Qt6::Widgets
            Qt6::Test
    )
    if(MSVC)
        target_compile_options(BgpTesterUiTests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(BgpTesterUiTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME BgpTesterUiTests COMMAND BgpTesterUiTests)
    set_tests_properties(BgpTesterUiTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
