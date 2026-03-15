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

# ── ABI sanity check: prevent MSVC ↔ MinGW Qt mismatch ──
# Building with MSVC against MinGW Qt (or vice versa) compiles fine but
# crashes immediately at runtime (ABI mismatch in QString/QSettings internals).
if(MSVC AND "${${QT}_DIR}" MATCHES "mingw")
    message(FATAL_ERROR
        "Qt installation was built with MinGW but this project is being compiled with MSVC.\n"
        "  Qt found at: ${${QT}_DIR}\n"
        "This will compile but crash at startup due to ABI mismatch.\n"
        "Fix: install Qt for MSVC (e.g. msvc2019_64) and set CMAKE_PREFIX_PATH to it:\n"
        "  cmake -DCMAKE_PREFIX_PATH=C:/Qt/6.5.2/msvc2019_64 ..")
elseif(MINGW AND "${${QT}_DIR}" MATCHES "msvc")
    message(FATAL_ERROR
        "Qt installation was built with MSVC but this project is being compiled with MinGW.\n"
        "  Qt found at: ${${QT}_DIR}\n"
        "This will compile but crash at startup due to ABI mismatch.\n"
        "Fix: install Qt for MinGW and set CMAKE_PREFIX_PATH to it:\n"
        "  cmake -DCMAKE_PREFIX_PATH=C:/Qt/6.5.2/mingw_64 ..")
endif()

# ── MSVC compile flags ──
if(MSVC)
    # /utf-8: treat source and execution character sets as UTF-8
    # /MP: multi-processor compilation
    add_compile_options(/utf-8 /MP)
endif()

# Qt5 on Windows needs WinExtras for HICON conversion
set(_QT_WINEXTRAS "")
if(QT_VERSION_MAJOR EQUAL 5 AND WIN32)
    find_package(Qt5 REQUIRED COMPONENTS WinExtras)
    set(_QT_WINEXTRAS Qt5::WinExtras)
endif()

find_package(QScintilla REQUIRED)

# RawPDB — direct PDB file reader (no DIA SDK / msdia140.dll dependency)
file(GLOB RAW_PDB_SRCS third_party/raw_pdb/src/*.cpp)
add_library(raw_pdb STATIC ${RAW_PDB_SRCS})
target_include_directories(raw_pdb PUBLIC third_party/raw_pdb/src)
target_compile_features(raw_pdb PRIVATE cxx_std_11)
# PDB_CRT.h forward-declares printf/memcmp/etc with __cdecl which conflicts
# with non-MSVC compilers (GCC, Clang, MinGW).  Force-include a prefix header
# that pulls in the real CRT headers and strips __cdecl.
if(NOT MSVC)
    target_compile_options(raw_pdb PUBLIC
        -include "${CMAKE_CURRENT_SOURCE_DIR}/cmake/raw_pdb_prefix.h")
endif()
if(WIN32)
    target_link_libraries(raw_pdb PRIVATE rpcrt4)
endif()

# Fadec — generate decode tables (.inc files) from instrs.txt at configure time
find_package(Python3 3.9 REQUIRED)
set(FADEC_DIR "${CMAKE_SOURCE_DIR}/third_party/fadec")
if(NOT EXISTS "${FADEC_DIR}/fadec-decode-public.inc")
    message(STATUS "Generating fadec decode tables...")
    execute_process(
        COMMAND ${Python3_EXECUTABLE} "${FADEC_DIR}/parseinstrs.py" decode
                "${FADEC_DIR}/instrs.txt"
                "${FADEC_DIR}/fadec-decode-public.inc"
                "${FADEC_DIR}/fadec-decode-private.inc"
                --32 --64
        RESULT_VARIABLE _fadec_result
    )
    if(NOT _fadec_result EQUAL 0)
        message(FATAL_ERROR "Failed to generate fadec decode tables")
    endif()
endif()

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
    src/imports/import_reclass_xml.h
    src/imports/import_reclass_xml.cpp
    src/imports/import_source.h
    src/imports/import_source.cpp
    src/imports/export_reclass_xml.h
    src/imports/export_reclass_xml.cpp
    src/imports/import_pdb.h
    src/imports/import_pdb.cpp
    src/imports/import_pdb_dialog.h
    src/imports/import_pdb_dialog.cpp
    src/scanner.h
    src/scanner.cpp
    src/scannerpanel.h
    src/scannerpanel.cpp
    src/mainwindow.h
    src/startpage.h
    src/dock_tab_buttons.h
    src/optionsdialog.h
    src/optionsdialog.cpp
    src/titlebar.h
    src/titlebar.cpp
    src/macos_titlebar.h
    $<$<PLATFORM_ID:Darwin>:src/macos_titlebar.mm>
    src/mcp/mcp_bridge.h
    src/mcp/mcp_bridge.cpp
    src/addressparser.h
    src/addressparser.cpp
    src/symbolstore.h
    src/symbolstore.cpp
    src/symbol_downloader.h
    src/symbol_downloader.cpp
    src/imports/pe_debug_info.h
    src/imports/pe_debug_info.cpp
    src/disasm.h
    src/disasm.cpp
    third_party/fadec/decode.c
    third_party/fadec/format.c
    $<$<PLATFORM_ID:Windows>:src/app.rc>
)

if(APPLE)
    set_target_properties(Reclass PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_ICON_FILE "class.icns"
    )
    target_sources(Reclass PRIVATE src/icons/class.icns)
    set_source_files_properties(src/icons/class.icns
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
endif()

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
    target_link_libraries(Reclass PRIVATE dbghelp dwmapi psapi raw_pdb)

    # Copy Debugging Tools dbghelp.dll next to Reclass.exe so the Windows
    # loader picks it up (app dir > System32).  The system dbghelp.dll
    # lacks StackWalk2 which the tools dbgeng.dll needs for remote debug.
    set(_DBG_TOOLS_DIRS
        "C:/Program Files (x86)/Windows Kits/10/Debuggers/x64"
        "C:/Program Files/Windows Kits/10/Debuggers/x64")
    foreach(_dir ${_DBG_TOOLS_DIRS})
        if(EXISTS "${_dir}/dbghelp.dll")
            foreach(_dll dbghelp.dll dbgcore.dll symsrv.dll)
                if(EXISTS "${_dir}/${_dll}")
                    add_custom_command(TARGET Reclass POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${_dir}/${_dll}" $<TARGET_FILE_DIR:Reclass>
                        COMMENT "Copying ${_dll} from Debugging Tools")
                endif()
            endforeach()
            break()
        endif()
    endforeach()
endif()

add_executable(ReclassMcpBridge tools/rcx-mcp-stdio.cpp)
target_link_libraries(ReclassMcpBridge PRIVATE ${QT}::Core ${QT}::Network)
if(APPLE)
    add_custom_command(TARGET ReclassMcpBridge POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:ReclassMcpBridge>
            $<TARGET_FILE_DIR:Reclass>/ReclassMcpBridge
        COMMENT "Bundling ReclassMcpBridge into Reclass.app"
    )
endif()

# Copy built-in theme JSON files next to the executable.
# For single-config generators (Ninja/Make) the exe is in ${CMAKE_BINARY_DIR},
# for multi-config generators (MSVC/Xcode) it's in ${CMAKE_BINARY_DIR}/<config>.
# Using a post-build copy with $<TARGET_FILE_DIR:Reclass> handles both.
file(GLOB _theme_files "${CMAKE_SOURCE_DIR}/src/themes/defaults/*.json")

# Single-config: configure_file for IDE convenience (available before first build)
if(NOT CMAKE_CONFIGURATION_TYPES)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/themes")
    foreach(_tf ${_theme_files})
        get_filename_component(_name ${_tf} NAME)
        configure_file(${_tf} "${CMAKE_BINARY_DIR}/themes/${_name}" COPYONLY)
    endforeach()
endif()

# Post-build: always copy to the actual exe directory (works for all generators)
add_custom_command(TARGET Reclass POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Reclass>/themes"
    COMMENT "Creating themes directory next to executable")
foreach(_tf ${_theme_files})
    get_filename_component(_name ${_tf} NAME)
    add_custom_command(TARGET Reclass POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_tf}" "$<TARGET_FILE_DIR:Reclass>/themes/${_name}")
endforeach()

if(APPLE)
    target_sources(Reclass PRIVATE ${_theme_files})
    set_source_files_properties(${_theme_files}
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/themes")
endif()

# Copy example .rcx files next to the executable (same logic as themes)
file(GLOB _example_files "${CMAKE_SOURCE_DIR}/src/examples/*.rcx")

if(NOT CMAKE_CONFIGURATION_TYPES)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/examples")
    foreach(_ef ${_example_files})
        get_filename_component(_name ${_ef} NAME)
        configure_file(${_ef} "${CMAKE_BINARY_DIR}/examples/${_name}" COPYONLY)
    endforeach()
endif()

add_custom_command(TARGET Reclass POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Reclass>/examples"
    COMMENT "Creating examples directory next to executable")
foreach(_ef ${_example_files})
    get_filename_component(_name ${_ef} NAME)
    add_custom_command(TARGET Reclass POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_ef}" "$<TARGET_FILE_DIR:Reclass>/examples/${_name}")
endforeach()

if(APPLE)
    target_sources(Reclass PRIVATE ${_example_files})
    set_source_files_properties(${_example_files}
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/examples")
endif()

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

    add_executable(test_core tests/test_core.cpp src/format.cpp src/compose.cpp src/addressparser.cpp)
    target_include_directories(test_core PRIVATE src)
    target_link_libraries(test_core PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_core COMMAND test_core)

    add_executable(test_typeinfer tests/test_typeinfer.cpp)
    target_include_directories(test_typeinfer PRIVATE src)
    target_link_libraries(test_typeinfer PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_typeinfer COMMAND test_typeinfer)

    add_executable(test_format tests/test_format.cpp src/format.cpp src/addressparser.cpp)
    target_include_directories(test_format PRIVATE src)
    target_link_libraries(test_format PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_format COMMAND test_format)

    add_executable(test_compose tests/test_compose.cpp src/compose.cpp src/format.cpp src/addressparser.cpp)
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
        src/generator.cpp src/compose.cpp src/format.cpp src/addressparser.cpp)
    target_include_directories(test_generator PRIVATE src)
    target_link_libraries(test_generator PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_generator COMMAND test_generator)

    add_executable(test_import_xml tests/test_import_xml.cpp
        src/imports/import_reclass_xml.cpp src/format.cpp src/compose.cpp src/addressparser.cpp)
    target_include_directories(test_import_xml PRIVATE src)
    target_link_libraries(test_import_xml PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_import_xml COMMAND test_import_xml)

    add_executable(test_import_source tests/test_import_source.cpp
        src/imports/import_source.cpp src/format.cpp src/compose.cpp src/addressparser.cpp)
    target_include_directories(test_import_source PRIVATE src)
    target_link_libraries(test_import_source PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_import_source COMMAND test_import_source)

    add_executable(test_export_xml tests/test_export_xml.cpp
        src/imports/export_reclass_xml.cpp src/imports/import_reclass_xml.cpp src/format.cpp src/compose.cpp src/addressparser.cpp)
    target_include_directories(test_export_xml PRIVATE src)
    target_link_libraries(test_export_xml PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_export_xml COMMAND test_export_xml)

    add_executable(test_disasm tests/test_disasm.cpp
        src/disasm.cpp src/compose.cpp src/format.cpp src/addressparser.cpp
        third_party/fadec/decode.c third_party/fadec/format.c)
    target_include_directories(test_disasm PRIVATE src third_party/fadec)
    target_link_libraries(test_disasm PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_disasm COMMAND test_disasm)

    add_executable(test_addressparser tests/test_addressparser.cpp src/addressparser.cpp)
    target_include_directories(test_addressparser PRIVATE src)
    target_link_libraries(test_addressparser PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_addressparser COMMAND test_addressparser)

    add_executable(test_static_fields tests/test_static_fields.cpp src/compose.cpp src/format.cpp src/addressparser.cpp)
    target_include_directories(test_static_fields PRIVATE src)
    target_link_libraries(test_static_fields PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_static_fields COMMAND test_static_fields)

    add_executable(test_scanner tests/test_scanner.cpp src/scanner.cpp)
    target_include_directories(test_scanner PRIVATE src)
    target_link_libraries(test_scanner PRIVATE ${QT}::Core ${QT}::Concurrent ${QT}::Test)
    add_test(NAME test_scanner COMMAND test_scanner)

    add_executable(test_32bit_support tests/test_32bit_support.cpp
        src/generator.cpp src/imports/import_source.cpp src/imports/import_reclass_xml.cpp
        src/compose.cpp src/format.cpp src/addressparser.cpp)
    target_include_directories(test_32bit_support PRIVATE src
        ${CMAKE_SOURCE_DIR}/plugins/RemoteProcessMemory)
    target_link_libraries(test_32bit_support PRIVATE ${QT}::Core ${QT}::Widgets ${QT}::Test)
    add_test(NAME test_32bit_support COMMAND test_32bit_support)
    set_tests_properties(test_32bit_support PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    if(WIN32)
        add_executable(test_import_pdb tests/test_import_pdb.cpp
            src/imports/import_pdb.cpp src/format.cpp src/compose.cpp src/addressparser.cpp)
        target_include_directories(test_import_pdb PRIVATE src)
        target_link_libraries(test_import_pdb PRIVATE
            ${QT}::Core ${QT}::Test raw_pdb)
        add_test(NAME test_import_pdb COMMAND test_import_pdb)

        add_executable(bench_import_pdb tests/bench_import_pdb.cpp
            src/imports/import_pdb.cpp src/format.cpp src/compose.cpp src/addressparser.cpp)
        target_include_directories(bench_import_pdb PRIVATE src)
        target_link_libraries(bench_import_pdb PRIVATE
            ${QT}::Core ${QT}::Test raw_pdb)
        add_test(NAME bench_import_pdb COMMAND bench_import_pdb)
    endif()

    # ── UI tests (require Qt::Widgets / QScintilla / display — skip on headless CI) ──
    option(BUILD_UI_TESTS "Build tests that require a display (Qt Widgets)" ON)
    if(BUILD_UI_TESTS)

        add_executable(test_controller tests/test_controller.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/controller.cpp src/symbolstore.cpp
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

        add_executable(test_context_menu tests/test_context_menu.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/controller.cpp src/symbolstore.cpp
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

        add_executable(test_source_management tests/test_source_management.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/controller.cpp src/symbolstore.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${DISASM_SRCS})
        target_include_directories(test_source_management PRIVATE src third_party/fadec)
        target_link_libraries(test_source_management PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test
        QScintilla::QScintilla)
        if(WIN32)
            target_link_libraries(test_source_management PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
        endif()
        add_test(NAME test_source_management COMMAND test_source_management)

        add_executable(test_editor tests/test_editor.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp
        src/providerregistry.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${DISASM_SRCS})
        target_include_directories(test_editor PRIVATE src third_party/fadec)
        target_link_libraries(test_editor PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Test
        QScintilla::QScintilla)
        add_test(NAME test_editor COMMAND test_editor)

        add_executable(test_rendered_view tests/test_rendered_view.cpp
        src/generator.cpp src/compose.cpp src/format.cpp src/addressparser.cpp)
        target_include_directories(test_rendered_view PRIVATE src)
        target_link_libraries(test_rendered_view PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Test
        QScintilla::QScintilla)
        add_test(NAME test_rendered_view COMMAND test_rendered_view)

        add_executable(test_type_selector tests/test_type_selector.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/controller.cpp src/symbolstore.cpp
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

        add_executable(test_type_visibility tests/test_type_visibility.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/controller.cpp src/symbolstore.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${DISASM_SRCS})
        target_include_directories(test_type_visibility PRIVATE src third_party/fadec)
        target_link_libraries(test_type_visibility PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test
        QScintilla::QScintilla)
        if(WIN32)
            target_link_libraries(test_type_visibility PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
        endif()
        add_test(NAME test_type_visibility COMMAND test_type_visibility)

        add_executable(test_options_dialog tests/test_options_dialog.cpp
        src/optionsdialog.cpp src/themes/theme.cpp src/themes/thememanager.cpp)
        target_include_directories(test_options_dialog PRIVATE src)
        target_link_libraries(test_options_dialog PRIVATE ${QT}::Widgets ${QT}::Test)
        add_test(NAME test_options_dialog COMMAND test_options_dialog)

        add_executable(test_source_provider tests/test_source_provider.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/controller.cpp src/symbolstore.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${DISASM_SRCS}
        src/resources.qrc)
        target_include_directories(test_source_provider PRIVATE src third_party/fadec)
        target_link_libraries(test_source_provider PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
        if(WIN32)
            target_link_libraries(test_source_provider PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
        endif()
        add_test(NAME test_source_provider COMMAND test_source_provider)

        add_executable(test_scanner_ui tests/test_scanner_ui.cpp
        src/scanner.cpp src/scannerpanel.cpp src/addressparser.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp)
        target_include_directories(test_scanner_ui PRIVATE src)
        target_link_libraries(test_scanner_ui PRIVATE
        ${QT}::Widgets ${QT}::Concurrent ${QT}::Test)
        add_test(NAME test_scanner_ui COMMAND test_scanner_ui)

        add_executable(test_mcp tests/test_mcp.cpp)
        target_include_directories(test_mcp PRIVATE src)
        target_link_libraries(test_mcp PRIVATE ${QT}::Core ${QT}::Network ${QT}::Test)
        add_test(NAME test_mcp COMMAND test_mcp)

        if(WIN32)
            add_executable(test_windbg_provider tests/test_windbg_provider.cpp
            plugins/WinDbgMemory/WinDbgMemoryPlugin.cpp
            src/scanner.cpp)
            target_include_directories(test_windbg_provider PRIVATE src plugins/WinDbgMemory)
            target_link_libraries(test_windbg_provider PRIVATE
            ${QT}::Widgets ${QT}::Concurrent ${QT}::Test dbgeng ole32)
            add_test(NAME test_windbg_provider COMMAND test_windbg_provider)

            add_executable(test_kernel_provider tests/test_kernel_provider.cpp
            plugins/KernelMemory/KernelMemoryPlugin.cpp
            src/processpicker.cpp src/processpicker.ui
            src/scanner.cpp)
            target_include_directories(test_kernel_provider PRIVATE
            src plugins/KernelMemory)
            target_link_libraries(test_kernel_provider PRIVATE
            ${QT}::Widgets ${QT}::Concurrent ${QT}::Test
            psapi shell32 advapi32 ${_QT_WINEXTRAS})
            add_test(NAME test_kernel_provider COMMAND test_kernel_provider)
        endif()

        add_executable(bench_large_class tests/bench_large_class.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp
        src/providerregistry.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${DISASM_SRCS})
        target_include_directories(bench_large_class PRIVATE src third_party/fadec)
        target_link_libraries(bench_large_class PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test
        QScintilla::QScintilla)
        if(WIN32)
            target_link_libraries(bench_large_class PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
        endif()
        add_test(NAME bench_large_class COMMAND bench_large_class)

        add_executable(bench_project tests/bench_project.cpp)
        target_include_directories(bench_project PRIVATE src)
        target_link_libraries(bench_project PRIVATE ${QT}::Widgets ${QT}::Test)
        if(WIN32)
            target_link_libraries(bench_project PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
        endif()
        add_test(NAME bench_project COMMAND bench_project)

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
if(NOT APPLE)
    add_subdirectory(plugins/RemoteProcessMemory)
endif()
if(WIN32)
    add_subdirectory(plugins/KernelMemory)
    add_subdirectory(plugins/WinDbgMemory)
    add_subdirectory(plugins/RcNetPluginCompatLayer)
endif()
