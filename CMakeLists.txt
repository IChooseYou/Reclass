cmake_minimum_required(VERSION 3.20)
project(RC VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Find Qt6 or Qt5 (config mode first, then FindQt5.cmake module for auto-download)
set(_QT_COMPONENTS Core Gui Widgets PrintSupport Svg Concurrent Network)
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

# Class timelines: the byte-history store (src/timeline/). QtCore only, so its
# tests run headless and it never depends on the widget layer.
add_library(rcx_timeline STATIC
    src/timeline/tl_types.h
    src/timeline/tl_varint.h
    src/timeline/tl_hash.h
    src/timeline/tl_xorrun.h
    src/timeline/tl_pyramid.h
    src/timeline/tl_store.h
    src/timeline/tl_store.cpp
    src/timeline/tl_clock.h
    src/timeline/tl_model.h
    src/timeline/executor.h
    src/timeline/capture_context.h
    src/timeline/capture_context.cpp
    src/timeline/timeline_service.h
    src/timeline/timeline_service.cpp
    src/timeline/timeline_hub.h
    src/timeline/timeline_hub.cpp
    src/timeline/frame_provider.h
    src/timeline/class_changes.h
    src/timeline/spill_store.h
    src/timeline/spill_store.cpp
)
target_include_directories(rcx_timeline PUBLIC src)
target_link_libraries(rcx_timeline PUBLIC ${QT}::Core)

add_executable(RC
    src/main.cpp
    src/editor.h
    src/editor.cpp
    src/controller.h
    src/controller.cpp
    src/ribbon_actions.h
    src/ribbon_actions.cpp
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
    src/sourcechooserpopup.h
    src/sourcechooserpopup.cpp
    src/dockoverlay.h
    src/hextoolbarpopup.h
    src/hextoolbarpopup.cpp
    src/pixelglyphs.h
    src/ribbon_icons.h
    src/ribbon_spec.h
    src/ribbon.h
    src/ribbon.cpp
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
    src/names/name_provider.h
    src/names/name_provider.cpp
    src/names/name_registry.h
    src/names/name_registry.cpp
    src/names/symbol_demangle.h
    src/names/symbol_demangle.cpp
    src/names/pdb_name_provider.h
    src/names/pdb_name_provider.cpp
    src/names/pdb_type_provider.h
    src/names/pdb_type_provider.cpp
    src/names/rtti_name_provider.h
    src/names/rtti_name_provider.cpp
    src/names/bookmark_name_provider.h
    src/names/bookmark_name_provider.cpp
    src/scanner.h
    src/scanner.cpp
    src/scannerpanel.h
    src/scannerpanel.cpp
    src/profiler.h
    src/profiler.cpp
    src/profilerdialog.h
    src/profilerdialog.cpp
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
    src/rtti.h
    src/rtti.cpp
    src/rttibrowser.h
    src/gotoaddressdialog.h
    src/commandpalette.h
    src/widgets/dialog_button.h
    src/widgets/dialog_button.cpp
    src/widgets/themed_dialog.h
    src/widgets/themed_dialog.cpp
    src/widgets/themed_messagebox.h
    src/widgets/themed_messagebox.cpp
    src/widgets/themed_inputdialog.h
    src/widgets/themed_inputdialog.cpp
    src/widgets/category_chip.h
    src/widgets/fuzzy_match.h
    src/widgets/unified_symbol_panel.h
    src/widgets/dock_header.h
    src/widgets/panel_search_field.h
    src/widgets/section_header.h
    src/widgets/enum_picker_popup.h
    third_party/fadec/decode.c
    third_party/fadec/format.c
    $<$<PLATFORM_ID:Windows>:src/app.rc>
)

if(APPLE)
    set_target_properties(RC PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_ICON_FILE "class.icns"
    )
    target_sources(RC PRIVATE src/icons/class.icns)
    set_source_files_properties(src/icons/class.icns
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
endif()

if(WIN32)
    # GUI (windows) subsystem: the OS allocates NO console for the app, so
    # there is no console window and no startup flash. A console is conjured
    # on demand (View > Show Console / on a crash) via AllocConsole. stderr
    # still reaches an inheriting terminal or the test harness's pipe because
    # the CRT wires fd 1/2 to inherited handles regardless of subsystem, so
    # the --screenshot/--profile/QT_FORCE_STDERR_LOGGING flow is intact.
    # MinGW maps this to -mwindows and keeps main() as the entry point (no
    # WinMain shim needed).
    set_target_properties(RC PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

target_include_directories(RC PRIVATE src third_party/fadec)

target_link_libraries(RC PRIVATE
    ${QT}::Widgets
    ${QT}::PrintSupport
    ${QT}::Svg
    ${QT}::Concurrent
    ${QT}::Network
    QScintilla::QScintilla
    ${_QT_WINEXTRAS}
)
# Qt6::GuiPrivate (for QZipReader, used by ReClass.NET .rcnet import)
# isn't shipped in the binary Qt distribution on macOS. Gate the link
# and a compile define; if absent, the .rcnet path falls back to an
# error message ("supported on Linux/Windows builds only").
if(TARGET ${QT}::GuiPrivate)
    target_link_libraries(RC PRIVATE ${QT}::GuiPrivate)
    target_compile_definitions(RC PRIVATE RCX_HAVE_QZIPREADER)
endif()
if(WIN32)
    target_link_libraries(RC PRIVATE dbghelp dwmapi psapi raw_pdb)

    # Copy Debugging Tools dbghelp.dll next to RC.exe so the Windows
    # loader picks it up (app dir > System32).  The system dbghelp.dll
    # lacks StackWalk2 which the tools dbgeng.dll needs for remote debug.
    set(_DBG_TOOLS_DIRS
        "C:/Program Files (x86)/Windows Kits/10/Debuggers/x64"
        "C:/Program Files/Windows Kits/10/Debuggers/x64")
    foreach(_dir ${_DBG_TOOLS_DIRS})
        if(EXISTS "${_dir}/dbghelp.dll")
            foreach(_dll dbghelp.dll dbgcore.dll symsrv.dll)
                if(EXISTS "${_dir}/${_dll}")
                    add_custom_command(TARGET RC POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${_dir}/${_dll}" $<TARGET_FILE_DIR:RC>
                        COMMENT "Copying ${_dll} from Debugging Tools")
                endif()
            endforeach()
            break()
        endif()
    endforeach()
endif()

add_executable(RCMcpBridge tools/rcx-mcp-stdio.cpp)
target_link_libraries(RCMcpBridge PRIVATE ${QT}::Core ${QT}::Network)
if(APPLE)
    add_custom_command(TARGET RCMcpBridge POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:RCMcpBridge>
            $<TARGET_FILE_DIR:RC>/RCMcpBridge
        COMMENT "Bundling RCMcpBridge into RC.app"
    )
endif()

# Copy built-in theme JSON files next to the executable.
# For single-config generators (Ninja/Make) the exe is in ${CMAKE_BINARY_DIR},
# for multi-config generators (MSVC/Xcode) it's in ${CMAKE_BINARY_DIR}/<config>.
# Using a post-build copy with $<TARGET_FILE_DIR:RC> handles both.
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
add_custom_command(TARGET RC POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:RC>/themes"
    COMMENT "Creating themes directory next to executable")
foreach(_tf ${_theme_files})
    get_filename_component(_name ${_tf} NAME)
    add_custom_command(TARGET RC POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_tf}" "$<TARGET_FILE_DIR:RC>/themes/${_name}")
endforeach()

if(APPLE)
    target_sources(RC PRIVATE ${_theme_files})
    set_source_files_properties(${_theme_files}
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/themes")
endif()

# Copy example .rcx files next to the executable (same logic as themes).
# Also pick up sibling sample binaries (.png) referenced by example
# .rcx files via "savedSources" — png.rcx ships with sample.png so the
# user opens it and immediately sees real bytes instead of all zeros.
file(GLOB _example_files
    "${CMAKE_SOURCE_DIR}/src/examples/*.rcx"
    "${CMAKE_SOURCE_DIR}/src/examples/*.png")

if(NOT CMAKE_CONFIGURATION_TYPES)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/examples")
    foreach(_ef ${_example_files})
        get_filename_component(_name ${_ef} NAME)
        configure_file(${_ef} "${CMAKE_BINARY_DIR}/examples/${_name}" COPYONLY)
    endforeach()
endif()

add_custom_command(TARGET RC POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:RC>/examples"
    COMMENT "Creating examples directory next to executable")
foreach(_ef ${_example_files})
    get_filename_component(_name ${_ef} NAME)
    add_custom_command(TARGET RC POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_ef}" "$<TARGET_FILE_DIR:RC>/examples/${_name}")
endforeach()

if(APPLE)
    target_sources(RC PRIVATE ${_example_files})
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
    \"${CMAKE_SOURCE_DIR}/src/hextoolbarpopup.h\"
    \"${CMAKE_SOURCE_DIR}/src/hextoolbarpopup.cpp\"
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
    DEPENDS RC
    COMMENT "Combining all source files into h_cpp_combined.txt"
)

include(CTest)
if(BUILD_TESTING)
    find_package(${QT} REQUIRED COMPONENTS Test)
    enable_testing()

    # Disasm/Fadec sources needed by any test that links editor.cpp
    # Disasm/Fadec sources. compose.cpp needs these directly now: the Asm node
    # kind decodes its own bytes into instruction rows, so EVERY target that
    # compiles compose.cpp must link them, not just the ones that pull in
    # editor.cpp for the hover popup.
    set(DISASM_SRCS src/disasm.cpp third_party/fadec/decode.c third_party/fadec/format.c)

    # Themed dialog widget sources — required by every test that links
    # processpicker.cpp / controller.cpp / scannerpanel.cpp (they all
    # use ThemedMessageBox / ThemedDialog).
    set(WIDGET_SRCS
        src/widgets/themed_dialog.cpp
        src/widgets/themed_messagebox.cpp
        src/widgets/themed_inputdialog.cpp
        src/widgets/dialog_button.cpp)

    # ── Headless tests (Qt::Core only — safe for CI without a display) ──

    add_executable(test_core tests/test_core.cpp src/format.cpp src/compose.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/symbolstore.cpp
        ${DISASM_SRCS})
    target_include_directories(test_core PRIVATE src third_party/fadec)
    target_link_libraries(test_core PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_core COMMAND test_core)

    add_executable(test_typeinfer tests/test_typeinfer.cpp)
    target_include_directories(test_typeinfer PRIVATE src)
    target_link_libraries(test_typeinfer PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_typeinfer COMMAND test_typeinfer)

    add_executable(test_dock_resize tests/test_dock_resize.cpp)
    target_link_libraries(test_dock_resize
        PRIVATE ${QT}::Core ${QT}::Widgets ${QT}::Test)
    add_test(NAME test_dock_resize COMMAND test_dock_resize)

    add_executable(test_format tests/test_format.cpp src/format.cpp src/addressparser.cpp)
    target_include_directories(test_format PRIVATE src)
    target_link_libraries(test_format PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_format COMMAND test_format)

    # The bouncing-ball example a first launch opens on (headless).
    add_executable(test_demo_simulation tests/test_demo_simulation.cpp)
    target_include_directories(test_demo_simulation PRIVATE src)
    target_link_libraries(test_demo_simulation PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_demo_simulation COMMAND test_demo_simulation)

    add_executable(test_compose tests/test_compose.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/symbolstore.cpp
        ${DISASM_SRCS})
    target_include_directories(test_compose PRIVATE src third_party/fadec)
    target_link_libraries(test_compose PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_compose COMMAND test_compose)

    # Drawn tree lines without a window: spans from composed rows, the copied
    # text, and the device-pixel geometry swept over scale / phase / zoom.
    add_executable(test_tree_guides tests/test_tree_guides.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/symbolstore.cpp
        ${DISASM_SRCS})
    target_include_directories(test_tree_guides PRIVATE src third_party/fadec)
    target_link_libraries(test_tree_guides PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_tree_guides COMMAND test_tree_guides)

    # One instance of a row: hover, selection and focus mark the place pointed
    # at or clicked, never every place the same node is shown.
    add_executable(test_row_instances tests/test_row_instances.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/symbolstore.cpp
        ${DISASM_SRCS})
    target_include_directories(test_row_instances PRIVATE src third_party/fadec)
    target_link_libraries(test_row_instances PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_row_instances COMMAND test_row_instances)

    # The frameless window's resize zones: every edge and corner, none when maximized.
    add_executable(test_resize_edges tests/test_resize_edges.cpp)
    target_include_directories(test_resize_edges PRIVATE src)
    target_link_libraries(test_resize_edges PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_resize_edges COMMAND test_resize_edges)

    # Unit tests for the unified tail-chip model (Enum / TypeHint / Rtti /
    # Comment chips emitted by compose). Same source set as test_compose
    # since chips are populated entirely inside compose.cpp.
    add_executable(test_chips tests/test_chips.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/symbolstore.cpp
        ${DISASM_SRCS})
    target_include_directories(test_chips PRIVATE src third_party/fadec)
    target_link_libraries(test_chips PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_chips COMMAND test_chips)

    # Controller-level test for RTTI overlay chip → class creation flow.
    # When the user clicks the overlay RTTI chip, MainWindow calls
    # RcxController::attachRttiClassToPointer(nodeId, demangledName) —
    # this verifies the contract: always-new class with _N suffix on
    # name collision, undo-atomic, leaves the node as Pointer64 with
    # refId pointing at the new class.
    add_executable(test_overlay_classcreate tests/test_overlay_classcreate.cpp
        src/controller.cpp src/compose.cpp src/format.cpp src/addressparser.cpp
        src/profiler.cpp src/rtti.cpp src/symbolstore.cpp
        src/hextoolbarpopup.cpp src/processpicker.cpp src/processpicker.ui
        src/providerregistry.cpp src/typeselectorpopup.cpp src/sourcechooserpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS}
        src/editor.cpp src/resources.qrc)
    target_include_directories(test_overlay_classcreate PRIVATE src third_party/fadec)
    target_link_libraries(test_overlay_classcreate PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
    if(WIN32)
        target_link_libraries(test_overlay_classcreate PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
    endif()
    add_test(NAME test_overlay_classcreate COMMAND test_overlay_classcreate)

    # Widget-level test for ChipOverlay — drives a live RcxEditor, asserts
    # chip rects land where Scintilla's POSITIONFROMLINE/POINTXFROMPOSITION
    # math says they should, verifies clicks-on-rect fire signals and
    # clicks-off-rect pass through to Scintilla, and confirms scroll +
    # resize keep the overlay glued to its text.
    add_executable(test_overlay_widget tests/test_overlay_widget.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp
        src/profiler.cpp src/rtti.cpp src/controller.cpp src/hextoolbarpopup.cpp
        src/symbolstore.cpp src/processpicker.cpp src/processpicker.ui
        src/providerregistry.cpp src/typeselectorpopup.cpp src/sourcechooserpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS}
        src/resources.qrc)
    target_include_directories(test_overlay_widget PRIVATE src third_party/fadec)
    target_link_libraries(test_overlay_widget PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
    if(WIN32)
        target_link_libraries(test_overlay_widget PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
    endif()
    add_test(NAME test_overlay_widget COMMAND test_overlay_widget)

    # Compose-only test for the null-vtable "(Name class…)" chip. The
    # rename-current-tab-class path that fires on click is plumbed
    # through MainWindow and validated by test_editor's inline-edit
    # tests — here we just lock in the chip emission contract.
    add_executable(test_overlay_null_rtti tests/test_overlay_null_rtti.cpp
        src/compose.cpp src/format.cpp src/addressparser.cpp
        src/profiler.cpp src/rtti.cpp src/symbolstore.cpp
        ${DISASM_SRCS})
    target_include_directories(test_overlay_null_rtti PRIVATE src third_party/fadec)
    target_link_libraries(test_overlay_null_rtti PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_overlay_null_rtti COMMAND test_overlay_null_rtti)

    # Workspace project-explorer model (workspace_model.h): empty-state row count
    # that drives the EmptyHintTreeView overlay + section/type row construction.
    # Header-only logic, so no extra .cpp sources are needed.
    # Also pins the WorkspaceDelegate section-row spec + selection accent,
    # so theme.cpp/thememanager.cpp come along (section_header.h reads the
    # live theme) plus Svg for the shared widget headers.
    add_executable(test_workspace tests/test_workspace.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp)
    target_include_directories(test_workspace PRIVATE src)
    target_link_libraries(test_workspace PRIVATE ${QT}::Widgets ${QT}::Svg ${QT}::Test)
    add_test(NAME test_workspace COMMAND test_workspace)

    # Tooltip-flicker detection. Drives mouse-move events into a live
    # RcxEditor whose first row carries an Rtti chip, then counts show/
    # hide transitions on rcx::sharedRcxTooltip(). A non-flickering chip
    # tooltip shows once on chip-entry and hides once on chip-exit per
    # oscillation; a flickering one ratchets up unbounded counts.
    add_executable(test_tooltip_flicker tests/test_tooltip_flicker.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp
        src/profiler.cpp src/rtti.cpp src/controller.cpp src/hextoolbarpopup.cpp
        src/symbolstore.cpp src/processpicker.cpp src/processpicker.ui
        src/providerregistry.cpp src/typeselectorpopup.cpp src/sourcechooserpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS}
        src/resources.qrc)
    target_include_directories(test_tooltip_flicker PRIVATE src third_party/fadec)
    target_link_libraries(test_tooltip_flicker PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
    if(WIN32)
        target_link_libraries(test_tooltip_flicker PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
    endif()
    add_test(NAME test_tooltip_flicker COMMAND test_tooltip_flicker)

    add_executable(test_default_class_footer tests/test_default_class_footer.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp
        src/profiler.cpp src/rtti.cpp src/controller.cpp src/hextoolbarpopup.cpp
        src/symbolstore.cpp src/processpicker.cpp src/processpicker.ui
        src/providerregistry.cpp src/typeselectorpopup.cpp src/sourcechooserpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS}
        src/resources.qrc)
    target_include_directories(test_default_class_footer PRIVATE src third_party/fadec)
    target_link_libraries(test_default_class_footer PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
    if(WIN32)
        target_link_libraries(test_default_class_footer PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
    endif()
    add_test(NAME test_default_class_footer COMMAND test_default_class_footer)

    add_executable(test_provider tests/test_provider.cpp)
    target_include_directories(test_provider PRIVATE src)
    target_link_libraries(test_provider PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_provider COMMAND test_provider)

    add_executable(test_timeline tests/test_timeline.cpp)
    target_link_libraries(test_timeline PRIVATE rcx_timeline ${QT}::Test)
    add_test(NAME test_timeline COMMAND test_timeline)

    add_executable(test_timeline_strip_model tests/test_timeline_strip_model.cpp)
    target_include_directories(test_timeline_strip_model PRIVATE src)
    target_link_libraries(test_timeline_strip_model PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_timeline_strip_model COMMAND test_timeline_strip_model)

    add_executable(test_command_row tests/test_command_row.cpp)
    target_include_directories(test_command_row PRIVATE src)
    target_link_libraries(test_command_row PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_command_row COMMAND test_command_row)

    # ── Address bar model + nav history: pure headers over NodeTree ──
    # Core + Test only (format.cpp for fmt::typeNameRaw, which core.h declares).
    # Deliberately NO controller / editor / widgets: the model must stay
    # buildable without a QWidget so the bar never grows tree logic of its own.
    add_executable(test_address_bar_model tests/test_address_bar_model.cpp
        src/format.cpp src/addressparser.cpp)
    target_include_directories(test_address_bar_model PRIVATE src)
    target_link_libraries(test_address_bar_model PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_address_bar_model COMMAND test_address_bar_model)

    # What the View > Font menu may offer. Needs Widgets + the resource
    # bundle: the two guaranteed families are loaded out of resources.qrc,
    # and the monospace check measures real glyph advances.
    add_executable(test_font_choices tests/test_font_choices.cpp src/resources.qrc)
    target_include_directories(test_font_choices PRIVATE src)
    target_link_libraries(test_font_choices PRIVATE ${QT}::Widgets ${QT}::Test)
    add_test(NAME test_font_choices COMMAND test_font_choices)

    add_executable(test_generator tests/test_generator.cpp
        src/generator.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/symbolstore.cpp
        ${DISASM_SRCS})
    target_include_directories(test_generator PRIVATE src third_party/fadec)
    target_link_libraries(test_generator PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_generator COMMAND test_generator)

    add_executable(test_import_xml tests/test_import_xml.cpp
        src/imports/import_reclass_xml.cpp src/format.cpp src/compose.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/symbolstore.cpp
        ${DISASM_SRCS})
    target_include_directories(test_import_xml PRIVATE src third_party/fadec)
    target_link_libraries(test_import_xml PRIVATE ${QT}::Core ${QT}::Test)
    if(TARGET ${QT}::GuiPrivate)
        target_link_libraries(test_import_xml PRIVATE ${QT}::GuiPrivate)
        target_compile_definitions(test_import_xml PRIVATE RCX_HAVE_QZIPREADER)
    endif()
    add_test(NAME test_import_xml COMMAND test_import_xml)

    add_executable(test_import_source tests/test_import_source.cpp
        src/imports/import_source.cpp src/format.cpp src/compose.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/symbolstore.cpp
        ${DISASM_SRCS})
    target_include_directories(test_import_source PRIVATE src third_party/fadec)
    target_link_libraries(test_import_source PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_import_source COMMAND test_import_source)

    add_executable(test_export_xml tests/test_export_xml.cpp
        src/imports/export_reclass_xml.cpp src/imports/import_reclass_xml.cpp src/format.cpp src/compose.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/symbolstore.cpp
        ${DISASM_SRCS})
    target_include_directories(test_export_xml PRIVATE src third_party/fadec)
    target_link_libraries(test_export_xml PRIVATE ${QT}::Core ${QT}::Test)
    if(TARGET ${QT}::GuiPrivate)
        target_link_libraries(test_export_xml PRIVATE ${QT}::GuiPrivate)
        target_compile_definitions(test_export_xml PRIVATE RCX_HAVE_QZIPREADER)
    endif()
    add_test(NAME test_export_xml COMMAND test_export_xml)

    add_executable(test_disasm tests/test_disasm.cpp
        src/disasm.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp
        src/rtti.cpp src/symbolstore.cpp
        third_party/fadec/decode.c third_party/fadec/format.c)
    target_include_directories(test_disasm PRIVATE src third_party/fadec)
    target_link_libraries(test_disasm PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_disasm COMMAND test_disasm)

    add_executable(test_addressparser tests/test_addressparser.cpp src/addressparser.cpp)
    target_include_directories(test_addressparser PRIVATE src)
    target_link_libraries(test_addressparser PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_addressparser COMMAND test_addressparser)

    # ── RTTI walker (headless: pure parser over a synthetic byte buffer) ──
    # Cross-platform: parser uses only Provider::read*, no platform APIs.
    add_executable(test_rtti tests/test_rtti.cpp src/rtti.cpp src/symbolstore.cpp)
    target_include_directories(test_rtti PRIVATE src)
    target_link_libraries(test_rtti PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_rtti COMMAND test_rtti)

    # ── RTTI inline-hint compose integration ──
    # Runs the full compose pipeline against a synthetic-RTTI buffer and
    # asserts that LineMeta picks up rttiHint / rttiVtableAddr correctly,
    # including the per-pass module cache (one enumerateModules call).
    add_executable(test_rtti_hint tests/test_rtti_hint.cpp
        src/compose.cpp src/format.cpp src/addressparser.cpp
        src/profiler.cpp src/rtti.cpp src/symbolstore.cpp
        ${DISASM_SRCS})
    target_include_directories(test_rtti_hint PRIVATE src third_party/fadec)
    target_link_libraries(test_rtti_hint PRIVATE ${QT}::Core ${QT}::Test)
    add_test(NAME test_rtti_hint COMMAND test_rtti_hint)

    # ── Tutorial flow simulation ──
    # End-to-end synthetic of selfTest's RcxEditor demo: typed Pointer64
    # with refId, value points at an Itanium-shape vtable in a fake module.
    # Asserts the typed-pointer compose path attaches the RTTI hint AND
    # tree.initialClass round-trips through JSON.
    add_executable(test_tutorial tests/test_tutorial.cpp
        src/compose.cpp src/format.cpp src/addressparser.cpp
        src/profiler.cpp src/rtti.cpp src/symbolstore.cpp
        ${DISASM_SRCS})
    target_include_directories(test_tutorial PRIVATE src third_party/fadec)
    target_link_libraries(test_tutorial PRIVATE ${QT}::Core ${QT}::Test)
    if(WIN32)
        # psapi for the SelfProvider's EnumProcessModulesEx probe.
        target_link_libraries(test_tutorial PRIVATE psapi)
    endif()
    add_test(NAME test_tutorial COMMAND test_tutorial)

    add_executable(test_scanner tests/test_scanner.cpp src/scanner.cpp)
    target_include_directories(test_scanner PRIVATE src)
    target_link_libraries(test_scanner PRIVATE ${QT}::Core ${QT}::Concurrent ${QT}::Test)
    if(WIN32)
        # psapi for the self-attach end-to-end test (EnumProcessModulesEx).
        target_link_libraries(test_scanner PRIVATE psapi)
    endif()
    add_test(NAME test_scanner COMMAND test_scanner)

    # Structure-aware matrix-scan predicate + engine + rescan-narrowing loop
    # (the logic the MCP scanner.find_matrix / scanner.rescan tools drive).
    add_executable(test_matrixscan tests/test_matrixscan.cpp src/scanner.cpp)
    target_include_directories(test_matrixscan PRIVATE src)
    target_link_libraries(test_matrixscan PRIVATE ${QT}::Core ${QT}::Concurrent ${QT}::Test)
    if(WIN32)
        target_link_libraries(test_matrixscan PRIVATE psapi)
    endif()
    add_test(NAME test_matrixscan COMMAND test_matrixscan)

    # Offscreen render harness for the Memory Scanner panel — grabs the panel
    # to a PNG under `-platform offscreen` for deterministic visual checks of
    # the UI (no display / session needed). Not a ctest; run manually.
    add_executable(scanner_render EXCLUDE_FROM_ALL tools/scanner_render.cpp
        src/scanner.cpp src/scannerpanel.cpp src/addressparser.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS}
        src/resources.qrc)   # so :/vsicons render in the grab (representative)
    target_include_directories(scanner_render PRIVATE src)
    target_link_libraries(scanner_render PRIVATE
        ${QT}::Widgets ${QT}::Concurrent ${QT}::Svg)
    if(WIN32)
        target_link_libraries(scanner_render PRIVATE psapi)
    endif()

    # UI regression tests for the ScannerPanel widget — first enabled CI coverage
    # of the panel (test_scanner_ui is disabled/stale). Construct-only + assert,
    # no modal/wait, so it won't hang the suite. Same link set as scanner_render.
    add_executable(test_scanner_panel tests/test_scanner_panel.cpp
        src/scanner.cpp src/scannerpanel.cpp src/addressparser.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS}
        src/resources.qrc)   # so :/vsicons load (silences svg-not-found warnings)
    target_include_directories(test_scanner_panel PRIVATE src)
    target_link_libraries(test_scanner_panel PRIVATE
        ${QT}::Widgets ${QT}::Concurrent ${QT}::Svg ${QT}::Test)
    if(WIN32)
        target_link_libraries(test_scanner_panel PRIVATE psapi)
    endif()
    add_test(NAME test_scanner_panel COMMAND test_scanner_panel)

    # Offscreen render harness for the custom window title bar — grabs the
    # chrome buttons + icons to a PNG so vertical-centering of the SVG glyphs
    # can be verified deterministically. qrc linked so :/vsicons resolve.
    add_executable(titlebar_render EXCLUDE_FROM_ALL tools/titlebar_render.cpp
        src/titlebar.cpp src/themes/theme.cpp src/themes/thememanager.cpp
        src/resources.qrc)
    target_include_directories(titlebar_render PRIVATE src)
    target_link_libraries(titlebar_render PRIVATE
        ${QT}::Widgets ${QT}::Svg)

    # Offscreen render harness for the ReClassEx-style ribbon — renders both
    # tabs at 760/1350/1920 px for a theme (dark default or a JSON path) so the
    # panel/caption/column metrics and the pixel-glyph icons can be eyeballed.
    #   ribbon_render <out-prefix> [themeJsonPath|themeName] [labels 0|1|2]
    add_executable(ribbon_render EXCLUDE_FROM_ALL tools/ribbon_render.cpp
        src/ribbon.cpp src/themes/theme.cpp src/themes/thememanager.cpp
        src/resources.qrc)
    target_include_directories(ribbon_render PRIVATE src)
    target_compile_definitions(ribbon_render PRIVATE RCX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(ribbon_render PRIVATE
        ${QT}::Widgets ${QT}::Svg)

    # Render harness for rcx::AddressBar. Builds the bar standalone (no
    # controller, no editor — the widget is header-only over the Core
    # sources) with synthetic 1 / 3 / 6-crumb states and grabs the three
    # stacked at 300/480/760/1080/1920 px, printing every laid-out cell's
    # logical rect + dpr so a pixel scan can crop. Hidden desktop only.
    #   address_bar_render <out-prefix> [themeJsonPath|themeName]
    #   address_bar_render <out-prefix> <theme> edit   -> the inline overlay + the help block
    add_executable(address_bar_render EXCLUDE_FROM_ALL tools/address_bar_render.cpp
        src/format.cpp src/addressparser.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp
        src/resources.qrc)
    target_include_directories(address_bar_render PRIVATE src)
    target_compile_definitions(address_bar_render PRIVATE RCX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(address_bar_render PRIVATE
        ${QT}::Widgets ${QT}::Svg)

    # Render harness for rcx::TimelineStrip: every capture state at six
    # widths, stacked, with the laid-out cell rects on stdout.
    #   timeline_render <out-prefix> [themeJsonPath|themeName]
    add_executable(timeline_render EXCLUDE_FROM_ALL tools/timeline_render.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp
        src/resources.qrc)
    target_include_directories(timeline_render PRIVATE src)
    target_compile_definitions(timeline_render PRIVATE RCX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(timeline_render PRIVATE ${QT}::Widgets ${QT}::Svg)

    add_executable(test_timeline_strip tests/test_timeline_strip.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp
        src/resources.qrc)
    target_include_directories(test_timeline_strip PRIVATE src)
    target_link_libraries(test_timeline_strip PRIVATE ${QT}::Widgets ${QT}::Svg ${QT}::Test)
    add_test(NAME test_timeline_strip COMMAND test_timeline_strip)

    # Render harness for rcx::RcxTooltip. Tooltips are separate top-level
    # windows, so MainWindow::grab() never contains them — this grabs the
    # tooltip widget itself. Renders short / multi-line / titled / long-wrapping
    # cases into one contact sheet.
    #   tooltip_render <out.png> [themeJsonPath|themeName]
    add_executable(tooltip_render EXCLUDE_FROM_ALL tools/tooltip_render.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp)
    target_include_directories(tooltip_render PRIVATE src)
    target_compile_definitions(tooltip_render PRIVATE RCX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(tooltip_render PRIVATE ${QT}::Widgets ${QT}::Svg)

    # Offscreen render harness for the RcxEditor — grabs the structure view
    # to a PNG so a multi-row byte selection (grey M_SELECTED rows + accent
    # IND_BYTE_SEL digits coexisting) can be verified deterministically, and
    # prints the controller's selectedIds() count to prove the byte→row sync.
    # Same heavy link set as the editor integration tests. Not a ctest.
    add_executable(editor_render EXCLUDE_FROM_ALL tools/editor_render.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp
        src/profiler.cpp src/rtti.cpp src/controller.cpp src/hextoolbarpopup.cpp
        src/symbolstore.cpp src/processpicker.cpp src/processpicker.ui
        src/providerregistry.cpp src/typeselectorpopup.cpp src/sourcechooserpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS}
        src/resources.qrc)
    target_include_directories(editor_render PRIVATE src third_party/fadec)
    target_link_libraries(editor_render PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Svg
        QScintilla::QScintilla)
    if(WIN32)
        target_link_libraries(editor_render PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
    endif()

    # Offscreen render harness for the TypeSelectorPopup — grabs the popup in
    # its default SIMPLE view to a PNG so the common-by-default list + "Show
    # all types" toggle row can be verified. Same heavy link set. Not a ctest.
    add_executable(typeselector_render EXCLUDE_FROM_ALL tools/typeselector_render.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp
        src/profiler.cpp src/rtti.cpp src/controller.cpp src/hextoolbarpopup.cpp
        src/symbolstore.cpp src/processpicker.cpp src/processpicker.ui
        src/providerregistry.cpp src/typeselectorpopup.cpp src/sourcechooserpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS}
        src/resources.qrc)
    target_include_directories(typeselector_render PRIVATE src third_party/fadec)
    target_link_libraries(typeselector_render PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Svg
        QScintilla::QScintilla)
    if(WIN32)
        target_link_libraries(typeselector_render PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
    endif()

    # Offscreen render harness for the SourceChooserPopup — grabs the data
    # source popup (Connected/Add Source sections + per-row × delete) to a PNG.
    # Self-contained (the popup only emits signals), so no controller needed.
    # Not a ctest. Pass "many" to verify the list scrolls past ~12 sources.
    add_executable(sourcechooser_render EXCLUDE_FROM_ALL tools/sourcechooser_render.cpp
        src/sourcechooserpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp src/resources.qrc)
    target_include_directories(sourcechooser_render PRIVATE src)
    target_link_libraries(sourcechooser_render PRIVATE
        ${QT}::Widgets ${QT}::Svg)
    if(WIN32)
        target_link_libraries(sourcechooser_render PRIVATE ${_QT_WINEXTRAS})
    endif()

    # Offscreen render harness for the "Code" tab syntax highlighting — grabs
    # generated code (any CodeFormat) with the themed per-language lexer to a
    # PNG so coloring can be verified per language. Not a ctest.
    add_executable(coderender EXCLUDE_FROM_ALL tools/coderender.cpp
        src/generator.cpp src/compose.cpp src/format.cpp src/addressparser.cpp
        src/profiler.cpp src/rtti.cpp src/symbolstore.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp
        ${DISASM_SRCS})
    target_include_directories(coderender PRIVATE src third_party/fadec)
    target_link_libraries(coderender PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Svg
        QScintilla::QScintilla)
    if(WIN32)
        target_link_libraries(coderender PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
    endif()

    # Non-UI combinatorial sweep — exercises every (mode, condition,
    # value type, alignment, region filter) tuple that the panel can
    # produce, against a synthetic provider, asserting the engine
    # returns the expected result count for each. No QtWidgets, no
    # popups, no flashing windows. Catches regressions when scan
    # request fields drift apart from the engine's interpretation.
    add_executable(test_scanner_combinations
        tests/test_scanner_combinations.cpp
        src/scanner.cpp)
    target_include_directories(test_scanner_combinations PRIVATE src)
    target_link_libraries(test_scanner_combinations
        PRIVATE ${QT}::Core ${QT}::Concurrent ${QT}::Test)
    add_test(NAME test_scanner_combinations COMMAND test_scanner_combinations)

    # ClipboardCodec — JSON MIME roundtrip + plaintext fallback
    add_executable(test_clipboard tests/test_clipboard.cpp)
    target_include_directories(test_clipboard PRIVATE src)
    target_link_libraries(test_clipboard PRIVATE ${QT}::Core ${QT}::Gui ${QT}::Test)
    add_test(NAME test_clipboard COMMAND test_clipboard)

    if(WIN32)
        add_executable(test_import_pdb tests/test_import_pdb.cpp
            src/imports/import_pdb.cpp src/format.cpp src/compose.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/symbolstore.cpp
        ${DISASM_SRCS})
        target_include_directories(test_import_pdb PRIVATE src third_party/fadec)
        target_link_libraries(test_import_pdb PRIVATE
            ${QT}::Core ${QT}::Test raw_pdb)
        add_test(NAME test_import_pdb COMMAND test_import_pdb)

        add_executable(bench_import_pdb tests/bench_import_pdb.cpp
            src/imports/import_pdb.cpp src/format.cpp src/compose.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/symbolstore.cpp
        ${DISASM_SRCS})
        target_include_directories(bench_import_pdb PRIVATE src third_party/fadec)
        target_link_libraries(bench_import_pdb PRIVATE
            ${QT}::Core ${QT}::Test raw_pdb)
        add_test(NAME bench_import_pdb COMMAND bench_import_pdb)
    endif()

    # ── UI tests (require Qt::Widgets / QScintilla / display — skip on headless CI) ──
    option(BUILD_UI_TESTS "Build tests that require a display (Qt Widgets)" ON)
    if(BUILD_UI_TESTS)

        add_executable(test_controller tests/test_controller.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/controller.cpp src/hextoolbarpopup.cpp src/symbolstore.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp src/sourcechooserpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS})
        target_include_directories(test_controller PRIVATE src third_party/fadec)
        target_link_libraries(test_controller PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
        if(WIN32)
            target_link_libraries(test_controller PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
        endif()
        add_test(NAME test_controller COMMAND test_controller)

        # ── Drill-down trail (focus path + AddressBar widget) ──
        # resources.qrc: the bar's nav cells are vsicons; the disabled-cell
        # opacity probe needs real ink to measure.
        add_executable(test_breadcrumb tests/test_breadcrumb.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/controller.cpp src/hextoolbarpopup.cpp src/symbolstore.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp src/sourcechooserpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS}
        src/resources.qrc)
        target_include_directories(test_breadcrumb PRIVATE src third_party/fadec)
        target_compile_definitions(test_breadcrumb PRIVATE RCX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
        target_link_libraries(test_breadcrumb PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
        if(WIN32)
            target_link_libraries(test_breadcrumb PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
        endif()
        add_test(NAME test_breadcrumb COMMAND test_breadcrumb)

        # ── New Class self-attach (doc-owned buffer + processmemory loopback) ──
        add_executable(test_new_class_selfattach tests/test_new_class_selfattach.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/controller.cpp src/hextoolbarpopup.cpp src/symbolstore.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp src/sourcechooserpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS})
        target_include_directories(test_new_class_selfattach PRIVATE src third_party/fadec)
        target_link_libraries(test_new_class_selfattach PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
        if(WIN32)
            target_link_libraries(test_new_class_selfattach PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
        endif()
        add_test(NAME test_new_class_selfattach COMMAND test_new_class_selfattach)

        # ── Byte-selection ↔ controller integration ──
        add_executable(test_byte_selection_controller tests/test_byte_selection_controller.cpp src/resources.qrc
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/controller.cpp src/hextoolbarpopup.cpp src/symbolstore.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp src/sourcechooserpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS})
        target_include_directories(test_byte_selection_controller PRIVATE src third_party/fadec)
        target_link_libraries(test_byte_selection_controller PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
        if(WIN32)
            target_link_libraries(test_byte_selection_controller PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
        endif()
        add_test(NAME test_byte_selection_controller COMMAND test_byte_selection_controller)

        # ── Ribbon action wiring (RibbonActions ↔ controller selection ops) ──
        add_executable(test_ribbon_actions tests/test_ribbon_actions.cpp
        src/ribbon_actions.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/controller.cpp src/hextoolbarpopup.cpp src/symbolstore.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp src/sourcechooserpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS})
        target_include_directories(test_ribbon_actions PRIVATE src third_party/fadec)
        target_link_libraries(test_ribbon_actions PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
        if(WIN32)
            target_link_libraries(test_ribbon_actions PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
        endif()
        add_test(NAME test_ribbon_actions COMMAND test_ribbon_actions)

        # -- Ribbon id parity: RibbonActions ids <-> RibbonBar spec ids + data --
        add_executable(test_ribbon_ids tests/test_ribbon_ids.cpp
        src/ribbon.cpp src/ribbon_actions.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/controller.cpp src/hextoolbarpopup.cpp src/symbolstore.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp src/sourcechooserpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp src/resources.qrc ${WIDGET_SRCS} ${DISASM_SRCS})
        target_include_directories(test_ribbon_ids PRIVATE src third_party/fadec)
        target_link_libraries(test_ribbon_ids PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
        if(WIN32)
            target_link_libraries(test_ribbon_ids PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
        endif()
        add_test(NAME test_ribbon_ids COMMAND test_ribbon_ids)

        # ── Refresh speedups (memory-source-only optimizations) ──
        add_executable(test_refresh_speedups tests/test_refresh_speedups.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/controller.cpp src/hextoolbarpopup.cpp src/symbolstore.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp src/sourcechooserpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS})
        target_include_directories(test_refresh_speedups PRIVATE src third_party/fadec)
        target_link_libraries(test_refresh_speedups PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
        if(WIN32)
            target_link_libraries(test_refresh_speedups PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
        endif()
        add_test(NAME test_refresh_speedups COMMAND test_refresh_speedups)

        add_executable(test_timeline_capture tests/test_timeline_capture.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/controller.cpp src/hextoolbarpopup.cpp src/symbolstore.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp src/sourcechooserpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS})
        target_include_directories(test_timeline_capture PRIVATE src third_party/fadec)
        target_link_libraries(test_timeline_capture PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
        if(WIN32)
            target_link_libraries(test_timeline_capture PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
        endif()
        add_test(NAME test_timeline_capture COMMAND test_timeline_capture)

        add_executable(test_context_menu tests/test_context_menu.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/controller.cpp src/hextoolbarpopup.cpp src/symbolstore.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp src/sourcechooserpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS})
        target_include_directories(test_context_menu PRIVATE src third_party/fadec)
        target_link_libraries(test_context_menu PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
        if(WIN32)
            target_link_libraries(test_context_menu PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
        endif()
        add_test(NAME test_context_menu COMMAND test_context_menu)

        add_executable(test_source_management tests/test_source_management.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/controller.cpp src/hextoolbarpopup.cpp src/symbolstore.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp src/sourcechooserpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS})
        target_include_directories(test_source_management PRIVATE src third_party/fadec)
        target_link_libraries(test_source_management PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
        if(WIN32)
            target_link_libraries(test_source_management PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
        endif()
        add_test(NAME test_source_management COMMAND test_source_management)

        add_executable(test_editor tests/test_editor.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp
        src/rtti.cpp src/symbolstore.cpp
        src/providerregistry.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS})
        target_include_directories(test_editor PRIVATE src third_party/fadec)
        target_link_libraries(test_editor PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
        add_test(NAME test_editor COMMAND test_editor)

        add_executable(test_byte_selection tests/test_byte_selection.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp
        src/rtti.cpp src/symbolstore.cpp
        src/providerregistry.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS})
        target_include_directories(test_byte_selection PRIVATE src third_party/fadec)
        target_link_libraries(test_byte_selection PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
        add_test(NAME test_byte_selection COMMAND test_byte_selection)

        # ── Lenient hex paste parser (clipboard.h, no GUI deps) ──
        add_executable(test_lenient_hex tests/test_lenient_hex.cpp)
        target_include_directories(test_lenient_hex PRIVATE src)
        target_link_libraries(test_lenient_hex PRIVATE ${QT}::Core ${QT}::Test)
        add_test(NAME test_lenient_hex COMMAND test_lenient_hex)

        add_executable(test_rendered_view tests/test_rendered_view.cpp
        src/generator.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/symbolstore.cpp
        ${DISASM_SRCS})
        target_include_directories(test_rendered_view PRIVATE src third_party/fadec)
        target_link_libraries(test_rendered_view PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
        add_test(NAME test_rendered_view COMMAND test_rendered_view)

        add_executable(test_type_selector tests/test_type_selector.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/controller.cpp src/hextoolbarpopup.cpp src/symbolstore.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp src/sourcechooserpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS})
        target_include_directories(test_type_selector PRIVATE src third_party/fadec)
        target_link_libraries(test_type_selector PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
        if(WIN32)
            target_link_libraries(test_type_selector PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
        endif()
        add_test(NAME test_type_selector COMMAND test_type_selector)

        # SourceChooserPopup interaction tests (× delete click/key). The popup
        # only emits signals, so the target is lean — no controller/editor.
        add_executable(test_source_chooser tests/test_source_chooser.cpp
            src/sourcechooserpopup.cpp
            src/themes/theme.cpp src/themes/thememanager.cpp src/resources.qrc)
        target_include_directories(test_source_chooser PRIVATE src)
        # The chrome tests load tw / vs from src/themes/defaults by path, so
        # the frame rule is pinned under both polarities.
        target_compile_definitions(test_source_chooser PRIVATE RCX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
        target_link_libraries(test_source_chooser PRIVATE
            ${QT}::Widgets ${QT}::Test ${QT}::Svg)
        if(WIN32)
            target_link_libraries(test_source_chooser PRIVATE ${_QT_WINEXTRAS})
        endif()
        add_test(NAME test_source_chooser COMMAND test_source_chooser)

        add_executable(test_type_visibility tests/test_type_visibility.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/controller.cpp src/hextoolbarpopup.cpp src/symbolstore.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp src/sourcechooserpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS})
        target_include_directories(test_type_visibility PRIVATE src third_party/fadec)
        target_link_libraries(test_type_visibility PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
        if(WIN32)
            target_link_libraries(test_type_visibility PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
        endif()
        add_test(NAME test_type_visibility COMMAND test_type_visibility)

        add_executable(test_options_dialog tests/test_options_dialog.cpp
        src/optionsdialog.cpp src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS})
        target_include_directories(test_options_dialog PRIVATE src)
        target_link_libraries(test_options_dialog PRIVATE ${QT}::Widgets ${QT}::Test ${QT}::Svg)
        add_test(NAME test_options_dialog COMMAND test_options_dialog)

        # ── Goto Address dialog (UI: real QLineEdit + live validation) ──
        add_executable(test_goto_address tests/test_goto_address.cpp
            src/addressparser.cpp src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS})
        target_include_directories(test_goto_address PRIVATE src)
        target_link_libraries(test_goto_address PRIVATE ${QT}::Widgets ${QT}::Test ${QT}::Svg)
        add_test(NAME test_goto_address COMMAND test_goto_address)

        # ── Command Palette (UI: real QMenuBar + QAction enumeration) ──
        add_executable(test_command_palette tests/test_command_palette.cpp
            src/themes/theme.cpp src/themes/thememanager.cpp)
        target_include_directories(test_command_palette PRIVATE src)
        target_link_libraries(test_command_palette PRIVATE ${QT}::Widgets ${QT}::Test)
        add_test(NAME test_command_palette COMMAND test_command_palette)

        add_executable(test_titlebar_border tests/test_titlebar_border.cpp
        src/titlebar.cpp src/themes/theme.cpp src/themes/thememanager.cpp
        src/resources.qrc)
        target_include_directories(test_titlebar_border PRIVATE src)
        target_link_libraries(test_titlebar_border PRIVATE ${QT}::Widgets ${QT}::Test ${QT}::Svg)
        add_test(NAME test_titlebar_border COMMAND test_titlebar_border)

        # ── Device-px hairline phase sweep (real-window grab at forced DPR 1.25) ──
        add_executable(test_hairline_dpr tests/test_hairline_dpr.cpp)
        target_include_directories(test_hairline_dpr PRIVATE src)
        target_link_libraries(test_hairline_dpr PRIVATE ${QT}::Widgets ${QT}::Test)
        add_test(NAME test_hairline_dpr COMMAND test_hairline_dpr)

        # ── Drawn tree lines on a real editor, read back from the backing store
        # at five display scales (the exe re-runs itself once per scale) ──
        add_executable(test_tree_guides_render tests/test_tree_guides_render.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp
        src/rtti.cpp src/symbolstore.cpp
        src/providerregistry.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS}
        src/resources.qrc)
        target_include_directories(test_tree_guides_render PRIVATE src third_party/fadec)
        target_link_libraries(test_tree_guides_render PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
        if(TARGET ${QT}::GuiPrivate)
            target_link_libraries(test_tree_guides_render PRIVATE ${QT}::GuiPrivate)
        endif()
        add_test(NAME test_tree_guides_render COMMAND test_tree_guides_render)

        add_executable(test_doc_tab_chrome tests/test_doc_tab_chrome.cpp src/resources.qrc)
        target_include_directories(test_doc_tab_chrome PRIVATE src)
        target_link_libraries(test_doc_tab_chrome PRIVATE ${QT}::Widgets ${QT}::Test)
        add_test(NAME test_doc_tab_chrome COMMAND test_doc_tab_chrome)

        add_executable(test_tab_source_icon tests/test_tab_source_icon.cpp src/resources.qrc)
        target_include_directories(test_tab_source_icon PRIVATE src)
        target_link_libraries(test_tab_source_icon PRIVATE ${QT}::Widgets ${QT}::Svg ${QT}::Test)
        add_test(NAME test_tab_source_icon COMMAND test_tab_source_icon)

        # ── Shared dock chrome: DockHeader / PanelSearchField / SectionHeader ──
        # Pins the dock-header height against kDocTabBarHeight (the Project
        # header seam has to land on the doc-tab seam), the one section-header
        # spec, and the collapsed-rail truth table. Needs Svg for themedVsIcon
        # and the qrc for :/vsicons.
        add_executable(test_dock_header tests/test_dock_header.cpp
            src/themes/theme.cpp src/themes/thememanager.cpp src/resources.qrc)
        target_include_directories(test_dock_header PRIVATE src)
        target_link_libraries(test_dock_header PRIVATE
            ${QT}::Widgets ${QT}::Svg ${QT}::Test)
        add_test(NAME test_dock_header COMMAND test_dock_header)

        # ── Ribbon pixel-glyph icons: crispness / scale rule / family contrast ──
        # Headless (QImage only). Loads the built-in themes straight from
        # src/themes/defaults via RCX_SOURCE_DIR so it doesn't depend on
        # ThemeManager's exe-dir lookup.
        add_executable(test_pixel_glyphs tests/test_pixel_glyphs.cpp
            src/ribbon.cpp src/themes/theme.cpp src/themes/thememanager.cpp
            src/resources.qrc)
        target_include_directories(test_pixel_glyphs PRIVATE src)
        target_compile_definitions(test_pixel_glyphs PRIVATE RCX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
        target_link_libraries(test_pixel_glyphs PRIVATE ${QT}::Widgets ${QT}::Svg ${QT}::Test)
        add_test(NAME test_pixel_glyphs COMMAND test_pixel_glyphs)

        # ── RibbonBar layout engine: overlap / 3-per-column / overflow / focus ──
        add_executable(test_ribbon_layout tests/test_ribbon_layout.cpp
            src/ribbon.cpp src/themes/theme.cpp src/themes/thememanager.cpp
            src/resources.qrc)
        target_include_directories(test_ribbon_layout PRIVATE src)
        target_compile_definitions(test_ribbon_layout PRIVATE RCX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
        target_link_libraries(test_ribbon_layout PRIVATE ${QT}::Widgets ${QT}::Svg ${QT}::Test)
        add_test(NAME test_ribbon_layout COMMAND test_ribbon_layout)

        # -- Ribbon in a real QMainWindow: sits between the title bar and the
        # dock area, fills the width, and DockOverlay drop zones skip it --
        add_executable(test_ribbon_mainwindow tests/test_ribbon_mainwindow.cpp
            src/dockoverlay.h src/ribbon.cpp src/titlebar.cpp src/themes/theme.cpp src/themes/thememanager.cpp
            src/resources.qrc)
        target_include_directories(test_ribbon_mainwindow PRIVATE src)
        target_compile_definitions(test_ribbon_mainwindow PRIVATE RCX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
        target_link_libraries(test_ribbon_mainwindow PRIVATE ${QT}::Widgets ${QT}::Svg ${QT}::Test)
        add_test(NAME test_ribbon_mainwindow COMMAND test_ribbon_mainwindow)

        add_executable(test_source_provider tests/test_source_provider.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp src/rtti.cpp src/controller.cpp src/hextoolbarpopup.cpp src/symbolstore.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp src/sourcechooserpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS}
        src/resources.qrc)
        target_include_directories(test_source_provider PRIVATE src third_party/fadec)
        target_link_libraries(test_source_provider PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
        if(WIN32)
            target_link_libraries(test_source_provider PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
        endif()
        add_test(NAME test_source_provider COMMAND test_source_provider)

        # test_scanner_ui is currently disabled — it hangs the suite.
        # Revisit when the scanner UI is reworked.
        # add_executable(test_scanner_ui tests/test_scanner_ui.cpp
        # src/scanner.cpp src/scannerpanel.cpp src/addressparser.cpp
        # src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS})
        # target_include_directories(test_scanner_ui PRIVATE src)
        # target_link_libraries(test_scanner_ui PRIVATE
        # ${QT}::Widgets ${QT}::Concurrent ${QT}::Test ${QT}::Svg)
        # add_test(NAME test_scanner_ui COMMAND test_scanner_ui)

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
            src/scanner.cpp
            src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS})
            target_include_directories(test_kernel_provider PRIVATE
            src plugins/KernelMemory)
            target_link_libraries(test_kernel_provider PRIVATE
            ${QT}::Widgets ${QT}::Concurrent ${QT}::Test ${QT}::Svg
            psapi shell32 advapi32 ${_QT_WINEXTRAS})
            add_test(NAME test_kernel_provider COMMAND test_kernel_provider)
        endif()

        add_executable(bench_large_class tests/bench_large_class.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp
        src/rtti.cpp src/symbolstore.cpp
        src/providerregistry.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS})
        target_include_directories(bench_large_class PRIVATE src third_party/fadec)
        target_link_libraries(bench_large_class PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test ${QT}::Svg
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

        # Spam-append + type-cycle benchmark — exercises the meta-diff
        # narrowing path with the runtime profiler enabled and dumps
        # aggregated bucket stats so we can verify (a) refresh stays
        # cheap during rapid edits, (b) per-line pass call counts match
        # type-change events (regression guard for the sameLine bug),
        # (c) IND_HEX_DIM stays applied to ALL hex64 lines after each
        # press — including the controller-driven path that goes
        # editor -> appendSingleFieldRequested -> insertNode ->
        # applyCommand -> refresh.
        add_executable(bench_spam_append tests/bench_spam_append.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/addressparser.cpp src/profiler.cpp
        src/rtti.cpp src/controller.cpp src/hextoolbarpopup.cpp src/symbolstore.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp
        src/typeselectorpopup.cpp src/sourcechooserpopup.cpp
        src/themes/theme.cpp src/themes/thememanager.cpp ${WIDGET_SRCS} ${DISASM_SRCS})
        target_include_directories(bench_spam_append PRIVATE src third_party/fadec)
        target_link_libraries(bench_spam_append PRIVATE
        ${QT}::Widgets ${QT}::PrintSupport ${QT}::Concurrent ${QT}::Test ${QT}::Svg
        QScintilla::QScintilla)
        if(WIN32)
            target_link_libraries(bench_spam_append PRIVATE dbghelp psapi ${_QT_WINEXTRAS})
        endif()
        add_test(NAME bench_spam_append COMMAND bench_spam_append)

        # ── Theme tone ladder: the shipped JSON, not the loader ──
        # Guards two orderings the editor's hierarchy depends on and that
        # nothing else reads together: selected > hover on dark themes, and
        # textFaint < textMuted < textDim < text. Parses the files straight
        # from src/themes/defaults so it fails on the DATA, whatever fallback
        # ThemeManager might grow.
        # The heat ramp is derived, not shipped: check what Theme::fromJson
        # hands a light-paper and a dark-paper theme.
        add_executable(test_theme_heat tests/test_theme_heat.cpp src/themes/theme.cpp)
        target_include_directories(test_theme_heat PRIVATE src)
        target_link_libraries(test_theme_heat PRIVATE ${QT}::Core ${QT}::Gui ${QT}::Test)
        add_test(NAME test_theme_heat COMMAND test_theme_heat)

        add_executable(test_theme_tones tests/test_theme_tones.cpp)
        target_include_directories(test_theme_tones PRIVATE src)
        target_compile_definitions(test_theme_tones PRIVATE
            RCX_THEME_DIR="${CMAKE_SOURCE_DIR}/src/themes/defaults")
        target_link_libraries(test_theme_tones PRIVATE ${QT}::Gui ${QT}::Test)
        add_test(NAME test_theme_tones COMMAND test_theme_tones)
        # ── Chrome contracts extracted from main.cpp: the ONE pane-tab sheet
        # and the status bar's location segment. Deliberately cheap (no
        # controller, no editor): both live in pure headers precisely because
        # MainWindow itself is not linkable from a test target.
        add_executable(test_pane_chrome tests/test_pane_chrome.cpp src/themes/theme.cpp)
        target_include_directories(test_pane_chrome PRIVATE src)
        target_link_libraries(test_pane_chrome PRIVATE ${QT}::Widgets ${QT}::Test)
        add_test(NAME test_pane_chrome COMMAND test_pane_chrome)

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

# ── Keep provider plugins in lockstep with the app (ABI safety) ──
# Every plugin here derives from rcx::Provider and is loaded at RUNTIME via
# CreatePlugin(), so there is no link dependency on RC. But each plugin
# COMPILES against providers/provider.h. Provider is a fragile base class:
# adding a data member to it (e.g. the modulesCached() cache) grows
# sizeof(Provider) and shifts every plugin-side derived member. If only the
# RC target is rebuilt after such a change (e.g. `cmake --build build
# --target RC`), the stale plugin DLLs become ABI-incompatible: the
# host reads the new base-class members where the plugin laid out the
# subclass's own data, producing a garbage QVector<ModuleEntry> and an
# access-violation crash the first time RTTI module resolution runs.
# (Symptom: lock-add on a near-null pointer inside rttiForVtable, reproduced
# by attaching a process source then refreshing.)
#
# Making RC depend on the plugin targets means building the app ALWAYS
# (re)builds the plugins first, so a partial app-only build can never leave
# an ABI-stale plugin behind. The plugins still pick up provider.h changes
# through Ninja/Make header-dependency tracking — this just guarantees they
# are considered whenever the app is.
foreach(_rcx_plugin
        ProcessMemoryPlugin
        RemoteProcessMemoryPlugin
        KernelMemoryPlugin
        WinDbgMemoryPlugin
        RcNetCompatPlugin)
    if(TARGET ${_rcx_plugin})
        add_dependencies(RC ${_rcx_plugin})
    endif()
endforeach()

# Class timelines: every target that compiles the controller links the
# timeline library, since the controller owns capture. One rule here instead
# of editing each of the ~20 test and harness targets that list
# src/controller.cpp by hand (and forgetting the next one).
get_property(_rcx_all_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_rcx_t ${_rcx_all_targets})
    get_target_property(_rcx_srcs ${_rcx_t} SOURCES)
    if(_rcx_srcs AND "${_rcx_srcs}" MATCHES "src/controller\\.cpp")
        target_link_libraries(${_rcx_t} PRIVATE rcx_timeline)
    endif()
endforeach()
