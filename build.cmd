@echo off
rem Configure and build PSPC with MSVC + Ninja.
rem Usage: build.cmd [Release|RelWithDebInfo|Debug] [extra cmake --build args]
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=RelWithDebInfo

if defined VSINSTALLDIR goto haveenv
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
:haveenv

where ninja >nul 2>&1 || set "PATH=%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%"

rem PSPC_BUILD_DIR lets parallel workers use separate build trees.
set BUILDDIR=build\%CONFIG%
if defined PSPC_BUILD_DIR set BUILDDIR=%PSPC_BUILD_DIR%
cmake -S . -B "%BUILDDIR%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% >nul
if errorlevel 1 exit /b 1
cmake --build "%BUILDDIR%" %2 %3 %4 %5
if errorlevel 1 exit /b 1
endlocal
exit /b 0

:novs
echo Visual Studio C++ tools not found.
exit /b 1
