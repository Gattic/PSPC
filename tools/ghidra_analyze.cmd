@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Ghidra headless pipeline for PSP modules.
rem Usage: tools\ghidra_analyze.cmd [options] <prx|dir> [<prx|dir> ...]
rem   Options are passed to tools\ghidra\ghidra_analyze.py (--help for the list).
rem   Uses JDK 21 (newest C:\Program Files\Eclipse Adoptium\jdk-21*, or %PSPC_JAVA_HOME%)
rem   and Ghidra at %GHIDRA_INSTALL_DIR% (default C:\Users\yoelr\Tools\ghidra_12.1.3_PUBLIC).
setlocal
set "PY=%PSPC_PYTHON%"
if "%PY%"=="" set "PY=C:\Users\yoelr\AppData\Local\Programs\Python\Python312\python.exe"
if "%PSPC_JAVA_HOME%"=="" (
	for /d %%j in ("%ProgramFiles%\Eclipse Adoptium\jdk-21*") do set "PSPC_JAVA_HOME=%%~fj"
)
if not "%PSPC_JAVA_HOME%"=="" set "JAVA_HOME=%PSPC_JAVA_HOME%"
"%PY%" -I "%~dp0ghidra\ghidra_analyze.py" %*
exit /b %ERRORLEVEL%
