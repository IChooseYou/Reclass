cmake_minimum_required(VERSION 3.20)
project(RcNetCompatPlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt is found by the parent project; QT variable (Qt5 or Qt6) is inherited

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Plugin sources
set(PLUGIN_SOURCES
    RcNetCompatPlugin.h
    RcNetCompatPlugin.cpp
    RcNetCompatProvider.h
    RcNetCompatProvider.cpp
    ReClassNET_Plugin.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/processpicker.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/processpicker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/processpicker.ui
    # Themed dialog widgets — required because processpicker now
    # subclasses rcx::ThemedDialog and uses rcx::ThemedMessageBox.
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/widgets/themed_dialog.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/widgets/themed_dialog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/widgets/themed_messagebox.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/widgets/themed_messagebox.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/widgets/dialog_button.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/widgets/dialog_button.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/themes/theme.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/themes/theme.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/themes/thememanager.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/themes/thememanager.cpp
)

# -- Optional .NET bridge -------------------------------------------------
# When the .NET SDK is available, build the C# bridge assembly and enable
# CLR hosting support in the C++ plugin.

find_program(DOTNET_EXE dotnet)
if(DOTNET_EXE)
    # Check that 'dotnet build' actually works for net472
    execute_process(
        COMMAND ${DOTNET_EXE} --list-sdks
        OUTPUT_VARIABLE _dotnet_sdks
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_dotnet_sdks)
        set(HAS_CLR_BRIDGE ON)
        message(STATUS "RcNetCompat: .NET SDK found -- building managed bridge")
    endif()
endif()

if(HAS_CLR_BRIDGE)
    list(APPEND PLUGIN_SOURCES
        ClrHost.h
        ClrHost.cpp
    )

    # Build the C# bridge assembly
    set(_bridge_src "${CMAKE_CURRENT_SOURCE_DIR}/bridge")
    set(_bridge_out "${CMAKE_BINARY_DIR}/Plugins/RcNetBridge.dll")

    add_custom_command(
        OUTPUT "${_bridge_out}"
        COMMAND ${DOTNET_EXE} build
            "${_bridge_src}/RcNetBridge.csproj"
            -c Release
            -o "${CMAKE_BINARY_DIR}/Plugins"
            --nologo -v quiet
        DEPENDS
            "${_bridge_src}/RcNetBridge.cs"
            "${_bridge_src}/RcNetBridge.csproj"
        COMMENT "Building RcNetBridge.dll (.NET bridge)..."
    )
    add_custom_target(RcNetBridge ALL DEPENDS "${_bridge_out}")
else()
    message(STATUS "RcNetCompat: .NET SDK not found -- managed plugin support disabled")
endif()

# Create shared library (DLL)
add_library(RcNetCompatPlugin SHARED ${PLUGIN_SOURCES})

if(HAS_CLR_BRIDGE)
    target_compile_definitions(RcNetCompatPlugin PRIVATE HAS_CLR_BRIDGE=1)
    add_dependencies(RcNetCompatPlugin RcNetBridge)
    # CLR hosting uses COM (ole32)
    target_link_libraries(RcNetCompatPlugin PRIVATE ole32)
endif()

# Link Qt
target_link_libraries(RcNetCompatPlugin PRIVATE ${QT}::Widgets ${QT}::Svg ${_QT_WINEXTRAS})

# Include directories
target_include_directories(RcNetCompatPlugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

# Output to Plugins folder
set_target_properties(RcNetCompatPlugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Plugins"
)
