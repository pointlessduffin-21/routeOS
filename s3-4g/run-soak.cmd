@echo off
rem Four hours of RVDP soak against the P4 display, detached from the editor so it survives closing VS Code.
rem Usage: run-soak.cmd [hours] [host]
setlocal
set HOURS=%~1
if "%HOURS%"=="" set HOURS=4
set TARGET=%~2
if "%TARGET%"=="" set TARGET=192.168.254.125
cd /d "%~dp0"
"C:\esp\.espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe" tools\rvdp_soak.py %TARGET% %HOURS% "%~dp0soak-%HOURS%h.log" >> "%~dp0soak-%HOURS%h.out" 2>&1
