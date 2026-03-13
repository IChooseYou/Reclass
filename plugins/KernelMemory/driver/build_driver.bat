@echo off
setlocal

set MSVC=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\MSVC\14.39.33519
set WDK=C:\Program Files (x86)\Windows Kits\10
set WDKVER=10.0.22621.0

set CL_EXE=%MSVC%\bin\Hostx64\x64\cl.exe
set LINK_EXE=%MSVC%\bin\Hostx64\x64\link.exe

set SRCDIR=%~dp0
set OUTDIR=%SRCDIR%build

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
