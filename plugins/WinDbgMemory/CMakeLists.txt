cmake_minimum_required(VERSION 3.20)
project(WinDbgMemoryPlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt is found by the parent project; QT variable (Qt5 or Qt6) is inherited

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Plugin sources
set(PLUGIN_SOURCES
    WinDbgMemoryPlugin.h
    WinDbgMemoryPlugin.cpp
)

# Create shared library (DLL)
add_library(WinDbgMemoryPlugin SHARED ${PLUGIN_SOURCES})

# Link Qt + DbgEng
target_link_libraries(WinDbgMemoryPlugin PRIVATE ${QT}::Widgets dbgeng ole32)

# Include directories
target_include_directories(WinDbgMemoryPlugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

# Output to Plugins folder
set_target_properties(WinDbgMemoryPlugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Plugins"
)
