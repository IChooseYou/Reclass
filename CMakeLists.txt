cmake_minimum_required(VERSION 3.20)
project(ReclassX VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets PrintSupport Svg Concurrent)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
find_package(QScintilla REQUIRED)

add_executable(ReclassX
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
    src/providers/buffer_provider.h src/providers/null_provider.h src/providers/process_provider.h src/providers/provider.h src/providers/snapshot_provider.h
    src/providerregistry.cpp
    src/providerregistry.h
    src/pluginmanager.cpp
    src/pluginmanager.h
)

target_include_directories(ReclassX PRIVATE src)

target_link_libraries(ReclassX PRIVATE
    Qt6::Widgets
    Qt6::PrintSupport
    Qt6::Svg
    Qt6::Concurrent
    QScintilla::QScintilla
    dbghelp
    psapi
)

add_custom_target(screenshot ALL
    COMMAND ReclassX --screenshot ${CMAKE_BINARY_DIR}/screenshot.png
    DEPENDS ReclassX
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Capturing UI screenshot with class open..."
)

add_custom_target(copy_demo ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_BINARY_DIR}/demo.rcx
        ${CMAKE_SOURCE_DIR}/src/examples/demo.rcx
    DEPENDS screenshot
    COMMENT "Copying demo.rcx to src/examples..."
)

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
    file(APPEND \${_out} \${_content})
    file(APPEND \${_out} \"\\n\")
endforeach()
message(STATUS \"Combined sources -> \${_out}\")
")

add_custom_target(combined ALL
    COMMAND ${CMAKE_COMMAND} -P ${_combine_script}
    DEPENDS ReclassX
    COMMENT "Combining all source files into h_cpp_combined.txt"
)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()

    add_executable(test_core tests/test_core.cpp src/format.cpp src/compose.cpp)
    target_include_directories(test_core PRIVATE src)
    target_link_libraries(test_core PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME test_core COMMAND test_core)

    add_executable(test_format tests/test_format.cpp src/format.cpp)
    target_include_directories(test_format PRIVATE src)
    target_link_libraries(test_format PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME test_format COMMAND test_format)

    add_executable(test_compose tests/test_compose.cpp src/compose.cpp src/format.cpp)
    target_include_directories(test_compose PRIVATE src)
    target_link_libraries(test_compose PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME test_compose COMMAND test_compose)

    add_executable(test_editor tests/test_editor.cpp src/editor.cpp src/compose.cpp src/format.cpp src/providerregistry.cpp)
    target_include_directories(test_editor PRIVATE src)
    target_link_libraries(test_editor PRIVATE
        Qt6::Widgets Qt6::PrintSupport Qt6::Test
        QScintilla::QScintilla)
    add_test(NAME test_editor COMMAND test_editor)

    add_executable(test_provider tests/test_provider.cpp)
    target_include_directories(test_provider PRIVATE src)
    target_link_libraries(test_provider PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME test_provider COMMAND test_provider)

    add_executable(test_command_row tests/test_command_row.cpp)
    target_include_directories(test_command_row PRIVATE src)
    target_link_libraries(test_command_row PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME test_command_row COMMAND test_command_row)

    add_executable(test_provider_getSymbol tests/test_provider_getSymbol.cpp)
    target_include_directories(test_provider_getSymbol PRIVATE src)
    target_link_libraries(test_provider_getSymbol PRIVATE Qt6::Core Qt6::Test)
    if(WIN32)
        target_link_libraries(test_provider_getSymbol PRIVATE psapi)
    endif()
    add_test(NAME test_provider_getSymbol COMMAND test_provider_getSymbol)

    add_executable(test_controller tests/test_controller.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/controller.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp)
    target_include_directories(test_controller PRIVATE src)
    target_link_libraries(test_controller PRIVATE
        Qt6::Widgets Qt6::PrintSupport Qt6::Concurrent Qt6::Test
        QScintilla::QScintilla dbghelp psapi)
    add_test(NAME test_controller COMMAND test_controller)

    add_executable(test_validation tests/test_validation.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/controller.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp)
    target_include_directories(test_validation PRIVATE src)
    target_link_libraries(test_validation PRIVATE
        Qt6::Widgets Qt6::PrintSupport Qt6::Concurrent Qt6::Test
        QScintilla::QScintilla dbghelp psapi)
    add_test(NAME test_validation COMMAND test_validation)

    add_executable(test_generator tests/test_generator.cpp
        src/generator.cpp src/compose.cpp src/format.cpp)
    target_include_directories(test_generator PRIVATE src)
    target_link_libraries(test_generator PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME test_generator COMMAND test_generator)

    add_executable(test_context_menu tests/test_context_menu.cpp
        src/editor.cpp src/compose.cpp src/format.cpp src/controller.cpp
        src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp)
    target_include_directories(test_context_menu PRIVATE src)
    target_link_libraries(test_context_menu PRIVATE
        Qt6::Widgets Qt6::PrintSupport Qt6::Concurrent Qt6::Test
        QScintilla::QScintilla dbghelp psapi)
    add_test(NAME test_context_menu COMMAND test_context_menu)

    add_executable(test_new_features tests/test_new_features.cpp
        src/generator.cpp src/compose.cpp src/format.cpp src/controller.cpp
        src/editor.cpp src/processpicker.cpp src/processpicker.ui src/providerregistry.cpp)
    target_include_directories(test_new_features PRIVATE src)
    target_link_libraries(test_new_features PRIVATE
        Qt6::Widgets Qt6::PrintSupport Qt6::Concurrent Qt6::Test
        QScintilla::QScintilla dbghelp psapi)
    add_test(NAME test_new_features COMMAND test_new_features)
endif()
add_subdirectory(plugins/ProcessMemory)
