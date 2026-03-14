@echo off
setlocal enabledelayedexpansion

:: ── Auto-detect MSVC (override with MSVC env var) ──
if not defined MSVC (
    set "VSBASE=C:\Program Files\Microsoft Visual Studio\2022"
    for %%E in (Enterprise Professional Community BuildTools) do (
        if exist "!VSBASE!\%%E\VC\Tools\MSVC" (
            for /f "delims=" %%V in ('dir /b /ad /o-n "!VSBASE!\%%E\VC\Tools\MSVC" 2^>nul') do (
                if not defined MSVC set "MSVC=!VSBASE!\%%E\VC\Tools\MSVC\%%V"
            )
        )
    )
)
if not defined MSVC (
    echo ERROR: Could not find MSVC toolchain
    exit /b 1
)

:: ── Auto-detect WDK (override with WDK_INC_ROOT and WDK_LIB_ROOT env vars) ──
if not defined WDK_INC_ROOT (
    set "WDK=C:\Program Files (x86)\Windows Kits\10"
    set WDKVER=
    for /f "delims=" %%V in ('dir /b /ad /o-n "!WDK!\Include" 2^>nul') do (
        if exist "!WDK!\Include\%%V\km\ntddk.h" (
            if not defined WDKVER set "WDKVER=%%V"
        )
    )
    if not defined WDKVER (
        echo ERROR: Could not find WDK headers under !WDK!\Include
        echo Set WDK_INC_ROOT and WDK_LIB_ROOT environment variables to override.
        exit /b 1
    )
    set "WDK_INC_ROOT=!WDK!\Include\!WDKVER!"
    set "WDK_LIB_ROOT=!WDK!\Lib\!WDKVER!"
)

echo Using MSVC:    %MSVC%
echo Using WDK inc: %WDK_INC_ROOT%
echo Using WDK lib: %WDK_LIB_ROOT%

set "CL_EXE=%MSVC%\bin\Hostx64\x64\cl.exe"
set "LINK_EXE=%MSVC%\bin\Hostx64\x64\link.exe"

set "SRCDIR=%~dp0"
set "OUTDIR=%SRCDIR%build"

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

echo === Compiling rcxdrv.c ===
"%CL_EXE%" /nologo /c /Zi /W4 /WX- /O2 /GS- ^
  /D "NDEBUG" /D "_AMD64_" /D "AMD64" /D "_WIN64" /D "KERNEL" ^
  /D "NTDDI_VERSION=0x0A000000" ^
  /I "%WDK_INC_ROOT%\km" ^
  /I "%WDK_INC_ROOT%\km\crt" ^
  /I "%WDK_INC_ROOT%\shared" ^
  /kernel ^
  /Fo"%OUTDIR%\rcxdrv.obj" ^
  "%SRCDIR%rcxdrv.c"
if errorlevel 1 goto :fail

echo === Linking rcxdrv.sys ===
"%LINK_EXE%" /nologo ^
  /OUT:"%OUTDIR%\rcxdrv.sys" ^
  /DRIVER:WDM ^
  /SUBSYSTEM:NATIVE ^
  /ENTRY:DriverEntry ^
  /MACHINE:X64 ^
  /NODEFAULTLIB ^
  /RELEASE ^
  /MERGE:.rdata=.text ^
  /INTEGRITYCHECK ^
  /PDBALTPATH:rcxdrv.pdb ^
  /PDB:"%OUTDIR%\rcxdrv.pdb" ^
  "%OUTDIR%\rcxdrv.obj" ^
  "%WDK_LIB_ROOT%\km\x64\ntoskrnl.lib" ^
  "%WDK_LIB_ROOT%\km\x64\hal.lib" ^
  "%WDK_LIB_ROOT%\km\x64\BufferOverflowK.lib" ^
  "%MSVC%\lib\x64\libcmt.lib"
if errorlevel 1 goto :fail

echo.
echo === SUCCESS ===
echo Output: %OUTDIR%\rcxdrv.sys
goto :eof

:fail
echo.
echo === BUILD FAILED ===
exit /b 1
