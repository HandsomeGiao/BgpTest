cmake_minimum_required(VERSION 3.25)

project(BgpTesterConfigurableExportPlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core)

set(BGPTESTER_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "BgpTester source directory containing src/plugin/RouterPlugin.hpp")

add_library(BgpTesterConfigurableExportPlugin MODULE
    ConfigurableExportRouterPlugin.cpp
)
target_include_directories(BgpTesterConfigurableExportPlugin PRIVATE
    "${BGPTESTER_SDK_DIR}/src"
)
target_link_libraries(BgpTesterConfigurableExportPlugin PRIVATE Qt6::Core)
set_target_properties(BgpTesterConfigurableExportPlugin PROPERTIES
    PREFIX ""
)
