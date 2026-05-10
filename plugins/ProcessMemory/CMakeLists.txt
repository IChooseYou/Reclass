cmake_minimum_required(VERSION 3.20)
project(ProcessMemoryPlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt is found by the parent project; QT variable (Qt5 or Qt6) is inherited

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

# Create shared library (DLL)
add_library(ProcessMemoryPlugin SHARED ${PLUGIN_SOURCES})

# Link Qt
target_link_libraries(ProcessMemoryPlugin PRIVATE ${QT}::Widgets ${QT}::Svg ${_QT_WINEXTRAS})

# Platform-specific linking
if(WIN32)
    target_link_libraries(ProcessMemoryPlugin PRIVATE psapi shell32)
endif()

# On Linux, hide all symbols by default so only RCX_PLUGIN_EXPORT-marked ones are exported
if(UNIX AND NOT APPLE)
    target_compile_options(ProcessMemoryPlugin PRIVATE -fvisibility=hidden)
endif()

# Include directories
target_include_directories(ProcessMemoryPlugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

# Output to Plugins folder
set_target_properties(ProcessMemoryPlugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Plugins"
)

if(APPLE AND TARGET Reclass)
    add_custom_command(TARGET ProcessMemoryPlugin POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Reclass>/../PlugIns"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:ProcessMemoryPlugin>"
            "$<TARGET_FILE_DIR:Reclass>/../PlugIns/"
        COMMENT "Copying ProcessMemoryPlugin into Reclass.app/Contents/PlugIns")
endif()
