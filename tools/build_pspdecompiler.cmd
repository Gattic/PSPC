@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Copyright (C) 2026 PSPC contributors
rem
rem Build Humberto Naves' pspdecompiler (AFL-2.0, reference\pspdecompiler) out of
rem tree with MSVC. It is a separate reverse-engineering tool and is never linked
rem into PSPC; nothing under reference\ is modified.
rem
rem Usage (any directory): tools\build_pspdecompiler.cmd [Release|Debug|RelWithDebInfo]
rem Output: build\tools\pspdecompiler\pspdecompiler.exe
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

pushd "%~dp0.." || exit /b 1

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if "%VSDIR%"=="" (
	echo Visual Studio C++ tools not found.
	popd
	exit /b 1
)
rem vcvars' extension scripts call vswhere.exe via PATH; make it findable.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || (popd & exit /b 1)

where ninja >nul 2>&1 || set "PATH=%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%"
set GENERATOR=Ninja
where ninja >nul 2>&1 || set "GENERATOR=NMake Makefiles"

set BUILDDIR=build\tools\pspdecompiler
if not exist "%BUILDDIR%\CMakeCache.txt" (
	cmake -S tools\pspdecompiler -B "%BUILDDIR%" -G "%GENERATOR%" -DCMAKE_BUILD_TYPE=%CONFIG% || (popd & exit /b 1)
) else (
	cmake -S tools\pspdecompiler -B "%BUILDDIR%" -DCMAKE_BUILD_TYPE=%CONFIG% || (popd & exit /b 1)
)
cmake --build "%BUILDDIR%" || (popd & exit /b 1)

echo Built %CD%\%BUILDDIR%\pspdecompiler.exe
popd
endlocal
