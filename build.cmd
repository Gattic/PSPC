@echo off
rem Configure and build PSPC with MSVC + Ninja.
rem Usage: build.cmd [Release|RelWithDebInfo|Debug] [extra cmake --build args]
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=RelWithDebInfo

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if "%VSDIR%"=="" (
	echo Visual Studio C++ tools not found.
	exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

where ninja >nul 2>&1 || set "PATH=%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%"

set BUILDDIR=build\%CONFIG%
if not exist "%BUILDDIR%\build.ninja" (
	cmake -S . -B "%BUILDDIR%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
)
cmake --build "%BUILDDIR%" %2 %3 %4 %5 || exit /b 1
endlocal
