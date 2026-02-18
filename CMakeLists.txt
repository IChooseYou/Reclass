cmake_minimum_required(VERSION 3.20)
project(Reclass VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Find Qt6 or Qt5 (config mode first, then FindQt5.cmake module for auto-download)
set(_QT_COMPONENTS Core Widgets PrintSupport Svg Concurrent Network)
find_package(QT NAMES Qt6 Qt5 COMPONENTS ${_QT_COMPONENTS} QUIET)
if(NOT QT_FOUND)
    find_package(Qt5 REQUIRED COMPONENTS ${_QT_COMPONENTS})
    set(QT_VERSION_MAJOR 5)
endif()
# The NAMES variant only detects the version; load the actual component targets
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS ${_QT_COMPONENTS})
set(QT Qt${QT_VERSION_MAJOR})
message(STATUS "Using ${QT}: ${${QT}_DIR}")

# Qt5 on Windows needs WinExtras for HICON conversion
set(_QT_WINEXTRAS "")
if(QT_VERSION_MAJOR EQUAL 5 AND WIN32)
    find_package(Qt5 REQUIRED COMPONENTS WinExtras)
    set(_QT_WINEXTRAS Qt5::WinExtras)
endif()

find_package(QScintilla REQUIRED)

add_executable(Reclass
    src/main.cpp
    src/editor.h
    src/editor.cpp
    src/controller.h
    src/controller.cpp
    src/compose.cpp
    src/format.cpp
    src/generator.h
    src/generator.cpp
    src/processpicker.h
    src/processpicker.cpp
    src/processpicker.ui
    src/resources.qrc
    src/core.h
    src/workspace_model.h
    src/providers/buffer_provider.h src/providers/null_provider.h src/providers/provider.h src/providers/snapshot_provider.h
    src/providerregistry.cpp
    src/providerregistry.h
    src/pluginmanager.cpp
    src/pluginmanager.h
    src/typeselectorpopup.h
    src/typeselectorpopup.cpp
    src/themes/theme.h
    src/themes/theme.cpp
    src/themes/thememanager.h
    src/themes/thememanager.cpp
    src/themes/themeeditor.h
    src/themes/themeeditor.cpp
    src/import_reclass_xml.h
    src/import_reclass_xml.cpp
    src/import_source.h
    src/import_source.cpp
    src/export_reclass_xml.h
    src/export_reclass_xml.cpp
    src/mainwindow.h
    src/optionsdialog.h
    src/optionsdialog.cpp
    src/titlebar.h
    src/titlebar.cpp
    src/mcp/mcp_bridge.h
    src/mcp/mcp_bridge.cpp
    src/disasm.h
    src/disasm.cpp
    third_party/fadec/decode.c
    third_party/fadec/format.c
    $<$<PLATFORM_ID:Windows>:src/app.rc>
)

target_include_directories(Reclass PRIVATE src third_party/fadec)

target_link_libraries(Reclass PRIVATE
    ${QT}::Widgets
    ${QT}::PrintSupport
    ${QT}::Svg
    ${QT}::Concurrent
    ${QT}::Network
    QScintilla::QScintilla
    ${_QT_WINEXTRAS}
)
if(WIN32)
    target_link_libraries(Reclass PRIVATE dbghelp dwmapi psapi)
endif()

add_executable(ReclassMcpBridge tools/rcx-mcp-stdio.cpp)
target_link_libraries(ReclassMcpBridge PRIVATE ${QT}::Core ${QT}::Network)

# Copy built-in theme JSON files to build directory
file(GLOB _theme_files "${CMAKE_SOURCE_DIR}/src/themes/defaults/*.json")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/themes")
foreach(_tf ${_theme_files})
    get_filename_component(_name ${_tf} NAME)
    configure_file(${_tf} "${CMAKE_BINARY_DIR}/themes/${_name}" COPYONLY)
endforeach()

# Copy example .rcx files to build directory
file(GLOB _example_files "${CMAKE_SOURCE_DIR}/src/examples/*.rcx")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/examples")
foreach(_ef ${_example_files})
    get_filename_component(_name ${_ef} NAME)
    configure_file(${_ef} "${CMAKE_BINARY_DIR}/examples/${_name}" COPYONLY)
endforeach()

include(deploy)


set(_combine_script "${CMAKE_BINARY_DIR}/combine_sources.cmake")
file(WRITE ${_combine_script} "
set(_out \"${CMAKE_BINARY_DIR}/h_cpp_combined.txt\")
file(WRITE \${_out} \"\")
foreach(_f
    \"${CMAKE_SOURCE_DIR}/src/core.h\"
    \"${CMAKE_SOURCE_DIR}/src/editor.h\"
    \"${CMAKE_SOURCE_DIR}/src/editor.cpp\"
    \"${CMAKE_SOURCE_DIR}/src/controller.h\"
    \"${CMAKE_SOURCE_DIR}/src/controller.cpp\"
    \"${CMAKE_SOURCE_DIR}/src/compose.cpp\"
    \"${CMAKE_SOURCE_DIR}/src/format.cpp\"
    \"${CMAKE_SOURCE_DIR}/src/generator.cpp\"
    \"${CMAKE_SOURCE_DIR}/src/main.cpp\")
    file(READ \${_f} _content)
    file(APPEND \${_out} \"\${_content}\")
    file(APPEND \${_out} \"\\n\")
endforeach()
message(STATUS \"Combined sources -> \${_out}\")
")

add_custom_target(combined ALL
    COMMAND ${CMAKE_COMMAND} -P ${_combine_script}
    DEPENDS Reclass
    COMMENT "Combining all source files into h_cpp_combined.txt"
)

include(CTest)
if(BUILD_TESTING)
    find_package(${QT} REQUIRED COMPONENTS Test)
    enable_testing()

    # Disasm/Fadec sources needed by any test that links editor.cpp
    set(DISASM_SRCS src/disasm.cpp third_party/fadec/decode.c third_party/fadec/format.c)

    # ── Headless tests (Qt::Core only — safe for CI without a display) ──

    add_executable(test_core tests/test_core.cpp src/format.cpp src/compose.cpp)
    target_include_directories(test_core PRIVATE src)
    target_link_libraries(test_core PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_core COMMAND test_core)

    add_executable(test_format tests/test_format.cpp src/format.cpp)
    target_include_directories(test_format PRIVATE src)
    target_link_libraries(test_format PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_format COMMAND test_format)

    add_executable(test_compose tests/test_compose.cpp src/compose.cpp src/format.cpp)
    target_include_directories(test_compose PRIVATE src)
    target_link_libraries(test_compose PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_compose COMMAND test_compose)

    add_executable(test_provider tests/test_provider.cpp)
    target_include_directories(test_provider PRIVATE src)
    target_link_libraries(test_provider PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_provider COMMAND test_provider)

    add_executable(test_command_row tests/test_command_row.cpp)
    target_include_directories(test_command_row PRIVATE src)
    target_link_libraries(test_command_row PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_command_row COMMAND test_command_row)

    add_executable(test_generator tests/test_generator.cpp
        src/generator.cpp src/compose.cpp src/format.cpp)
    target_include_directories(test_generator PRIVATE src)
    target_link_libraries(test_generator PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_generator COMMAND test_generator)

    add_executable(test_import_xml tests/test_import_xml.cpp
        src/import_reclass_xml.cpp src/format.cpp src/compose.cpp)
    target_include_directories(test_import_xml PRIVATE src)
    target_link_libraries(test_import_xml PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_import_xml COMMAND test_import_xml)

    add_executable(test_import_source tests/test_import_source.cpp
        src/import_source.cpp src/format.cpp src/compose.cpp)
    target_include_directories(test_import_source PRIVATE src)
    target_link_libraries(test_import_source PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_import_source COMMAND test_import_source)

    add_executable(test_export_xml tests/test_export_xml.cpp
        src/export_reclass_xml.cpp src/import_reclass_xml.cpp src/format.cpp src/compose.cpp)
    target_include_directories(test_export_xml PRIVATE src)
    target_link_libraries(test_export_xml PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_export_xml COMMAND test_export_xml)

    add_executable(test_disasm tests/test_disasm.cpp
        src/disasm.cpp src/compose.cpp src/format.cpp
        third_party/fadec/decode.c third_party/fadec/format.c)
    target_include_directories(test_disasm PRIVATE src third_party/fadec)
    target_link_libraries(test_disasm PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_disasm COMMAND test_disasm)

    # ── UI tests (require Qt::Widgets / QScintilla / display — skip on headless CI) ──
    option(BUILD_UI_TESTS "Build tests that require a display (Qt Widgets)" ON)
    if(BUILD_UI_TESTS)

    add_executable(test_controller tests/test_controller.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/controller.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${DISASM_SRCS})
    target_include_directories(test_controller PRIVATE src third_party/fadec)
    target_link_libraries(test_controller PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test
        QScintilla::QScintilla)
    if(WIN32)
        target_link_libraries(test_controller PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
    endif()
    add_test(NAME test_controller COMMAND test_controller)

    add_executable(test_validation tests/test_validation.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/controller.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${DISASM_SRCS})
    target_include_directories(test_validation PRIVATE src third_party/fadec)
    target_link_libraries(test_validation PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test
        QScintilla::QScintilla)
    if(WIN32)
        target_link_libraries(test_validation PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
    endif()
    add_test(NAME test_validation COMMAND test_validation)

    add_executable(test_context_menu tests/test_context_menu.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/controller.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${DISASM_SRCS})
    target_include_directories(test_context_menu PRIVATE src third_party/fadec)
    target_link_libraries(test_context_menu PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test
        QScintilla::QScintilla)
    if(WIN32)
        target_link_libraries(test_context_menu PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
    endif()
    add_test(NAME test_context_menu COMMAND test_context_menu)

    add_executable(test_editor tests/test_editor.cpp
        src/editor.cpp src/compose.cpp src/format.cpp
        src/providerregistry.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${DISASM_SRCS})
    target_include_directories(test_editor PRIVATE src third_party/fadec)
    target_link_libraries(test_editor PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Test
        QScintilla::QScintilla)
    add_test(NAME test_editor COMMAND test_editor)

    add_executable(test_rendered_view tests/test_rendered_view.cpp
        src/generator.cpp src/compose.cpp src/format.cpp)
    target_include_directories(test_rendered_view PRIVATE src)
    target_link_libraries(test_rendered_view PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Test
        QScintilla::QScintilla)
    add_test(NAME test_rendered_view COMMAND test_rendered_view)

    add_executable(test_new_features tests/test_new_features.cpp
        src/generator.cpp src/compose.cpp src/format.cpp src/controller.cpp
        src/editor.cpp src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${DISASM_SRCS})
    target_include_directories(test_new_features PRIVATE src third_party/fadec)
    target_link_libraries(test_new_features PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test
        QScintilla::QScintilla)
    if(WIN32)
        target_link_libraries(test_new_features PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
    endif()
    add_test(NAME test_new_features COMMAND test_new_features)

    add_executable(test_type_selector tests/test_type_selector.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/controller.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${DISASM_SRCS})
    target_include_directories(test_type_selector PRIVATE src third_party/fadec)
    target_link_libraries(test_type_selector PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test
        QScintilla::QScintilla)
    if(WIN32)
        target_link_libraries(test_type_selector PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
    endif()
    add_test(NAME test_type_selector COMMAND test_type_selector)


    add_executable(test_options_dialog tests/test_options_dialog.cpp
        src/optionsdialog.cpp src/themes/theme.cpp src/themes/thememanager.cpp)
    target_include_directories(test_options_dialog PRIVATE src)
    target_link_libraries(test_options_dialog PRIVATE ${QT}::Widgets ${QT}::Test)
    add_test(NAME test_options_dialog COMMAND test_options_dialog)

    if(WIN32)
        add_executable(test_windbg_provider tests/test_windbg_provider.cpp
            plugins/WinDbgMemory/WinDbgMemoryPlugin.cpp)
        target_include_directories(test_windbg_provider PRIVATE src plugins/WinDbgMemory)
        target_link_libraries(test_windbg_provider PRIVATE
            ${QT}::Widgets ${QT}::Concurrent ${QT}::Test dbgeng ole32)
        add_test(NAME test_windbg_provider COMMAND test_windbg_provider)
    endif()

    # Standalone test: proves whether CoInitializeSecurity is needed for DebugConnect
    # Requires a running WinDbg debug server on port 5055
    if(WIN32)
        add_executable(test_com_security tests/test_com_security.cpp)
        target_link_libraries(test_com_security PRIVATE dbgeng ole32 version)
        add_test(NAME test_com_security COMMAND test_com_security)
    endif()

    # Deploy Qt runtime DLLs for tests (run windeployqt on a representative test exe
    # that links the broadest set of Qt modules; all test exes share the same output dir)
    if(TARGET ${QT}::windeployqt)
        add_custom_target(deploy_tests ALL
            COMMAND $<TARGET_FILE:${QT}::windeployqt>
                --no-compiler-runtime --no-translations
                --no-opengl-sw --no-system-d3d-compiler
                $<TARGET_FILE:test_controller>
            DEPENDS test_controller
            COMMENT "Deploying Qt runtime DLLs for tests..."
        )
    endif()

    endif() # BUILD_UI_TESTS
endif()
add_subdirectory(plugins/ProcessMemory)
if(WIN32)
    add_subdirectory(plugins/WinDbgMemory)
    add_subdirectory(plugins/RcNetPluginCompatLayer)
endif()
