@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Ghidra headless pipeline for PSP modules.
rem Usage: tools\ghidra_analyze.cmd [options] <prx|dir> [<prx|dir> ...]
rem   Options are passed to tools\ghidra\ghidra_analyze.py (--help for the list).
rem   Select Ghidra with --ghidra or %GHIDRA_INSTALL_DIR%.
rem   JDK discovery is handled by the Python helper (--java-home, environment, installed JDKs, PATH).
setlocal
set "PY=%PSPC_PYTHON%"
if "%PY%"=="" set "PY=C:\Users\yoelr\AppData\Local\Programs\Python\Python312\python.exe"
"%PY%" -I "%~dp0ghidra\ghidra_analyze.py" %*
exit /b %ERRORLEVEL%
