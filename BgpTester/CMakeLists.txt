cmake_minimum_required(VERSION 3.25)

project(BgpTester VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BGPTESTER_BUILD_GUI "Build the optional Qt 6 desktop application" OFF)

include(FetchContent)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

find_package(Threads REQUIRED)
find_package(SQLite3 QUIET)
if(NOT SQLite3_FOUND)
    FetchContent_Declare(
        sqlite_amalgamation
        URL https://www.sqlite.org/2024/sqlite-amalgamation-3450300.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(sqlite_amalgamation)

    add_library(bgptester_sqlite3 STATIC
        "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c"
    )
    target_include_directories(bgptester_sqlite3 PUBLIC
        "${sqlite_amalgamation_SOURCE_DIR}"
    )
    target_compile_definitions(bgptester_sqlite3 PRIVATE SQLITE_THREADSAFE=1)
    target_link_libraries(bgptester_sqlite3 PUBLIC
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )
    add_library(SQLite::SQLite3 ALIAS bgptester_sqlite3)
endif()

function(bgptester_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

function(bgptester_set_runtime_output target)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin"
    )
endfunction()

add_library(bgptester_core STATIC
    src/core/Types.hpp
    src/core/Topology.cpp
    src/core/RouterPolicy.cpp
    src/core/SimulationEngine.cpp
    src/core/EventStore.cpp
    src/core/HeadlessSession.cpp
)
target_include_directories(bgptester_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(bgptester_core
    PUBLIC
        nlohmann_json::nlohmann_json
    PRIVATE
        SQLite::SQLite3
        Threads::Threads
)
bgptester_enable_warnings(bgptester_core)

add_executable(BgpTesterCli
    src/cli/portable_main.cpp
)
target_link_libraries(BgpTesterCli PRIVATE bgptester_core)
bgptester_enable_warnings(BgpTesterCli)
bgptester_set_runtime_output(BgpTesterCli)
add_custom_command(TARGET BgpTesterCli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/topo"
        "$<TARGET_FILE_DIR:BgpTesterCli>/topo"
)

if(BGPTESTER_BUILD_GUI)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Sql)

    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)
    set(CMAKE_AUTORCC ON)
    qt_standard_project_setup()

    # The optional Qt application keeps its legacy source-plugin discovery.
    # Adding a .hpp/.cpp pair under src/router_plugins is enough;
    # CONFIGURE_DEPENDS makes the next normal build rerun CMake when files are
    # added or removed. The portable core uses RouterPolicyRegistry instead.
    file(GLOB_RECURSE BGPTESTER_QT_ROUTER_PLUGIN_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/router_plugins/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/router_plugins/*.hpp"
    )

    add_library(bgptester_qt_router_plugins OBJECT
        ${BGPTESTER_QT_ROUTER_PLUGIN_SOURCES}
    )
    target_include_directories(bgptester_qt_router_plugins PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(bgptester_qt_router_plugins PRIVATE
        Qt6::Core
        Qt6::Sql
    )
    bgptester_enable_warnings(bgptester_qt_router_plugins)

    add_library(bgptester_qt_core STATIC
        src/model/DeterministicRandom.hpp
        src/model/Topology.cpp
        src/engine/SimulationEngine.cpp
        src/plugin/RouterPluginRegistry.cpp
        src/persistence/EventStore.cpp
        src/persistence/SimulationEventCodec.cpp
    )
    target_include_directories(bgptester_qt_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(bgptester_qt_core
        PUBLIC
            Qt6::Core
            Qt6::Sql
    )
    bgptester_enable_warnings(bgptester_qt_core)

    qt_add_executable(BgpTester WIN32
        src/main.cpp
        src/ui/Dialogs.cpp
        src/ui/EventTableModel.cpp
        src/ui/MainWindow.cpp
        src/ui/RibTableModels.cpp
        src/ui/TopologyScene.cpp
    )
    target_sources(BgpTester PRIVATE
        $<TARGET_OBJECTS:bgptester_qt_router_plugins>
    )
    target_link_libraries(BgpTester
        PRIVATE
            bgptester_qt_core
            Qt6::Gui
            Qt6::Widgets
    )
    bgptester_enable_warnings(BgpTester)
    bgptester_set_runtime_output(BgpTester)

    add_custom_command(TARGET BgpTester POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/topo"
            "$<TARGET_FILE_DIR:BgpTester>/topo"
    )
endif()

include(CTest)
if(BUILD_TESTING)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/cli_test")
    add_test(NAME BgpTesterCliTests
        COMMAND ${CMAKE_COMMAND}
            "-DCLI_EXECUTABLE=$<TARGET_FILE:BgpTesterCli>"
            "-DTOPOLOGY=${CMAKE_CURRENT_SOURCE_DIR}/topo/sample_topology.json"
            "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/topo/sample_headless_commands.jsonl"
            "-DWORK_DIR=${CMAKE_BINARY_DIR}/cli_test"
            "-DSCENARIO=runtime"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/VerifyCliScenario.cmake"
    )
    set_tests_properties(BgpTesterCliTests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/cli_test"
        TIMEOUT 60
    )

    add_test(NAME BgpTesterCliEditTests
        COMMAND ${CMAKE_COMMAND}
            "-DCLI_EXECUTABLE=$<TARGET_FILE:BgpTesterCli>"
            "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/topo/sample_headless_edit_commands.jsonl"
            "-DWORK_DIR=${CMAKE_BINARY_DIR}/cli_test"
            "-DSCENARIO=edit"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/VerifyCliScenario.cmake"
    )
    set_tests_properties(BgpTesterCliEditTests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/cli_test"
        TIMEOUT 30
    )

    if(BGPTESTER_BUILD_GUI)
        find_package(Qt6 6.5 REQUIRED COMPONENTS Test)

        add_executable(BgpTesterTests
            tests/CoreTests.cpp
            src/headless/HeadlessSession.cpp
            src/headless/HeadlessSession.hpp
        )
        target_sources(BgpTesterTests PRIVATE
            $<TARGET_OBJECTS:bgptester_qt_router_plugins>
        )
        target_link_libraries(BgpTesterTests PRIVATE bgptester_qt_core)
        bgptester_enable_warnings(BgpTesterTests)
        add_test(NAME BgpTesterTests COMMAND BgpTesterTests)

        add_test(NAME BgpTesterDeterminismTests
            COMMAND ${CMAKE_COMMAND}
                "-DTEST_EXECUTABLE=$<TARGET_FILE:BgpTesterTests>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/VerifyDeterminism.cmake"
        )
        set_tests_properties(BgpTesterDeterminismTests PROPERTIES TIMEOUT 120)

        add_executable(BgpTesterUiTests
            tests/UiTests.cpp
            src/ui/Dialogs.cpp
            src/ui/EventTableModel.cpp
            src/ui/MainWindow.cpp
            src/ui/RibTableModels.cpp
            src/ui/TopologyScene.cpp
        )
        target_sources(BgpTesterUiTests PRIVATE
            $<TARGET_OBJECTS:bgptester_qt_router_plugins>
        )
        target_link_libraries(BgpTesterUiTests
            PRIVATE
                bgptester_qt_core
                Qt6::Gui
                Qt6::Widgets
                Qt6::Test
        )
        bgptester_enable_warnings(BgpTesterUiTests)
        add_test(NAME BgpTesterUiTests COMMAND BgpTesterUiTests)
        set_tests_properties(BgpTesterUiTests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        )
    endif()
endif()
