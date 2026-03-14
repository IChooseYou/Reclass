@echo off
setlocal enabledelayedexpansion

:: ── Auto-detect MSVC ──
set "VSBASE=C:\Program Files\Microsoft Visual Studio\2022"
set MSVC=
for %%E in (Enterprise Professional Community BuildTools) do (
    if exist "!VSBASE!\%%E\VC\Tools\MSVC" (
        for /f "delims=" %%V in ('dir /b /ad /o-n "!VSBASE!\%%E\VC\Tools\MSVC" 2^>nul') do (
            if not defined MSVC set "MSVC=!VSBASE!\%%E\VC\Tools\MSVC\%%V"
        )
    )
)
if not defined MSVC (
    echo ERROR: Could not find MSVC toolchain under !VSBASE!
    exit /b 1
)

:: ── Auto-detect WDK ──
set "WDK=C:\Program Files (x86)\Windows Kits\10"
set WDKVER=
for /f "delims=" %%V in ('dir /b /ad /o-n "!WDK!\Include" 2^>nul') do (
    if exist "!WDK!\Include\%%V\km\ntddk.h" (
        if not defined WDKVER set "WDKVER=%%V"
    )
)
if not defined WDKVER (
    echo ERROR: Could not find WDK headers under !WDK!\Include
    exit /b 1
)

echo Using MSVC: %MSVC%
echo Using WDK:  %WDK% (%WDKVER%)

set "CL_EXE=%MSVC%\bin\Hostx64\x64\cl.exe"
set "LINK_EXE=%MSVC%\bin\Hostx64\x64\link.exe"

set "SRCDIR=%~dp0"
set "OUTDIR=%SRCDIR%build"

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

echo === Compiling rcxdrv.c ===
"%CL_EXE%" /nologo /c /Zi /W4 /WX- /O2 /GS- ^
  /D "NDEBUG" /D "_AMD64_" /D "AMD64" /D "_WIN64" /D "KERNEL" ^
  /D "NTDDI_VERSION=0x0A000000" ^
  /I "%WDK%\Include\%WDKVER%\km" ^
  /I "%WDK%\Include\%WDKVER%\km\crt" ^
  /I "%WDK%\Include\%WDKVER%\shared" ^
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
  "%WDK%\Lib\%WDKVER%\km\x64\ntoskrnl.lib" ^
  "%WDK%\Lib\%WDKVER%\km\x64\hal.lib" ^
  "%WDK%\Lib\%WDKVER%\km\x64\BufferOverflowK.lib" ^
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
