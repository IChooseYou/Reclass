# cmake/deploy.cmake - Dual-mode script for deploying Qt runtime DLLs
#
# Script mode:  cmake -P deploy.cmake <target_exe> <windeployqt>
# Include mode: include(deploy) from CMakeLists.txt (creates "deploy" target)

if(CMAKE_SCRIPT_MODE_FILE)
    set(TARGET_EXE ${CMAKE_ARGV3})
    set(WINDEPLOYQT ${CMAKE_ARGV4})
    get_filename_component(TARGET_DIR ${TARGET_EXE} DIRECTORY)

    # Skip if already deployed for this build
    if(EXISTS "${TARGET_DIR}/.qt_deployed")
        return()
    endif()

    message(STATUS "Running windeployqt on ${TARGET_EXE}")

    execute_process(
        COMMAND ${WINDEPLOYQT}
            --pdb
            --no-compiler-runtime
            --no-translations
            --no-opengl-sw
            --no-system-d3d-compiler
            --force
            ${TARGET_EXE}
        RESULT_VARIABLE _result
    )

    if(_result EQUAL 0)
        file(WRITE "${TARGET_DIR}/.qt_deployed" "")
        message(STATUS "windeployqt completed successfully")
    else()
        message(WARNING "windeployqt failed with exit code ${_result}")
    endif()

    return()
endif()

# ── Include mode: configure the deploy target ──

if(NOT WIN32)
    return()
endif()

# Discover windeployqt from qmake
if(NOT TARGET ${QT}::windeployqt AND TARGET ${QT}::qmake)
    get_target_property(_qt_qmake_location ${QT}::qmake IMPORTED_LOCATION)

    execute_process(
        COMMAND "${_qt_qmake_location}" -query QT_INSTALL_PREFIX
        RESULT_VARIABLE _return_code
        OUTPUT_VARIABLE _qt_install_prefix
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    set(_windeployqt "${_qt_install_prefix}/bin/windeployqt.exe")
    if(EXISTS ${_windeployqt})
        add_executable(${QT}::windeployqt IMPORTED)
        set_target_properties(${QT}::windeployqt PROPERTIES
            IMPORTED_LOCATION ${_windeployqt}
        )
        message(STATUS "Found windeployqt: ${_windeployqt}")
    else()
        message(WARNING "windeployqt not found at ${_windeployqt}")
    endif()
endif()

if(TARGET ${QT}::windeployqt)
    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/deploy.cmake
            $<TARGET_FILE:Reclass>
            $<TARGET_FILE:${QT}::windeployqt>
        DEPENDS Reclass
        COMMENT "Deploying Qt runtime DLLs..."
    )

    # Force re-deploy on rebuild
    set_target_properties(deploy PROPERTIES
        ADDITIONAL_CLEAN_FILES $<TARGET_FILE_DIR:Reclass>/.qt_deployed
    )
endif()
