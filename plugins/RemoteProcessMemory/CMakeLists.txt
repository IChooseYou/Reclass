cmake_minimum_required(VERSION 3.20)
project(RemoteProcessMemory LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt is found by the parent project; QT variable (Qt5 or Qt6) is inherited
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)   # run uic manually to avoid dupbuild with ProcessMemoryPlugin

# ─── 1. Payload DLL/SO (no Qt, minimal dependencies) ────────────────

add_library(rcx_payload SHARED
    payload/rcx_payload.cpp
    rcx_rpc_protocol.h
)

set_target_properties(rcx_payload PROPERTIES PREFIX "")  # rcx_payload.dll / rcx_payload.so

target_include_directories(rcx_payload PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_link_libraries(rcx_payload PRIVATE psapi)
else()
    target_link_libraries(rcx_payload PRIVATE pthread rt)
    target_compile_options(rcx_payload PRIVATE -fvisibility=hidden)
endif()

# Output payload to Plugins/ (same dir as plugin DLL, discovered at runtime)
set_target_properties(rcx_payload PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Plugins"
)

# Install rule: copy both DLLs to install Plugins/ folder
install(TARGETS rcx_payload
    LIBRARY DESTINATION Plugins
    RUNTIME DESTINATION Plugins
)

# ─── 2. Plugin DLL (Qt, implements IProviderPlugin) ──────────────────

# Generate ui_processpicker.h in our own build dir (avoids dupbuild with ProcessMemoryPlugin)
set(_UI_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../src/processpicker.ui")
set(_UI_HDR "${CMAKE_CURRENT_BINARY_DIR}/ui_processpicker.h")

add_custom_command(
    OUTPUT  "${_UI_HDR}"
    COMMAND ${QT}::uic -o "${_UI_HDR}" "${_UI_SRC}"
    DEPENDS "${_UI_SRC}"
    COMMENT "UIC processpicker.ui (RemoteProcessMemory)"
    VERBATIM
)

set(PLUGIN_SOURCES
    RemoteProcessMemoryPlugin.h
    RemoteProcessMemoryPlugin.cpp
    rcx_rpc_protocol.h
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

add_library(RemoteProcessMemoryPlugin SHARED ${PLUGIN_SOURCES})

target_link_libraries(RemoteProcessMemoryPlugin PRIVATE
    ${QT}::Widgets
    ${QT}::Svg
    ${_QT_WINEXTRAS}
)

if(WIN32)
    target_link_libraries(RemoteProcessMemoryPlugin PRIVATE psapi shell32)
else()
    target_link_libraries(RemoteProcessMemoryPlugin PRIVATE rt dl)
    target_compile_options(RemoteProcessMemoryPlugin PRIVATE -fvisibility=hidden)
endif()

target_include_directories(RemoteProcessMemoryPlugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}   # for ui_processpicker.h
)

set_target_properties(RemoteProcessMemoryPlugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Plugins"
)

install(TARGETS RemoteProcessMemoryPlugin
    LIBRARY DESTINATION Plugins
    RUNTIME DESTINATION Plugins
)

# Plugin must be able to find the payload at runtime
add_dependencies(RemoteProcessMemoryPlugin rcx_payload)

# ─── 3. Test executables (no Qt) ────────────────────────────────────

# Host: loads payload in-process, exposes test buffer
add_executable(test_rpc_host tests/test_rpc_host.cpp)
target_include_directories(test_rpc_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
    target_link_libraries(test_rpc_host PRIVATE psapi)
else()
    target_link_libraries(test_rpc_host PRIVATE pthread rt dl)
endif()
set_target_properties(test_rpc_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Plugins"
)
add_dependencies(test_rpc_host rcx_payload)

# Client: connects to host, tests + benchmarks
add_executable(test_rpc_client tests/test_rpc_client.cpp)
target_include_directories(test_rpc_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
    target_link_libraries(test_rpc_client PRIVATE psapi)
else()
    target_link_libraries(test_rpc_client PRIVATE pthread rt)
endif()
set_target_properties(test_rpc_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Plugins"
)
add_dependencies(test_rpc_client test_rpc_host)
