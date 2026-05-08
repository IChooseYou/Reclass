cmake_minimum_required(VERSION 3.20)
project(KernelMemoryPlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt is found by the parent project; QT variable (Qt5 or Qt6) is inherited
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)   # run uic manually to avoid dupbuild with ProcessMemoryPlugin

# ─── Generate ui_processpicker.h in our own build dir ────────────────
set(_UI_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../src/processpicker.ui")
set(_UI_HDR "${CMAKE_CURRENT_BINARY_DIR}/ui_processpicker.h")

add_custom_command(
    OUTPUT  "${_UI_HDR}"
    COMMAND ${QT}::uic -o "${_UI_HDR}" "${_UI_SRC}"
    DEPENDS "${_UI_SRC}"
    COMMENT "UIC processpicker.ui (KernelMemoryPlugin)"
    VERBATIM
)

# ─── Plugin DLL ──────────────────────────────────────────────────────
set(PLUGIN_SOURCES
    KernelMemoryPlugin.h
    KernelMemoryPlugin.cpp
    rcx_drv_protocol.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/processpicker.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/processpicker.cpp
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
    "${_UI_HDR}"
)

add_library(KernelMemoryPlugin SHARED ${PLUGIN_SOURCES})

target_link_libraries(KernelMemoryPlugin PRIVATE
    ${QT}::Widgets
    ${_QT_WINEXTRAS}
)

if(WIN32)
    target_link_libraries(KernelMemoryPlugin PRIVATE psapi shell32 advapi32)
endif()

if(UNIX AND NOT APPLE)
    target_compile_options(KernelMemoryPlugin PRIVATE -fvisibility=hidden)
endif()

target_include_directories(KernelMemoryPlugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}   # for ui_processpicker.h
)

set_target_properties(KernelMemoryPlugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Plugins"
)

install(TARGETS KernelMemoryPlugin
    LIBRARY DESTINATION Plugins
    RUNTIME DESTINATION Plugins
)
