cmake_minimum_required(VERSION 3.20)
project(ProcessMemoryPlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Qt
find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Plugin sources
set(PLUGIN_SOURCES
    ProcessMemoryPlugin.h
    ProcessMemoryPlugin.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/processpicker.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/processpicker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/processpicker.ui
)

# Create shared library (DLL)
add_library(ProcessMemoryPlugin SHARED ${PLUGIN_SOURCES})

# Link Qt
target_link_libraries(ProcessMemoryPlugin PRIVATE Qt6::Widgets)

# Include directories
target_include_directories(ProcessMemoryPlugin PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

# Output to Plugins folder
set_target_properties(ProcessMemoryPlugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Plugins"
)
