@echo off
rem Regenerate the soak report. Safe to run at any time; it only reads the log and the health CSV.
setlocal
cd /d "%~dp0"
"C:\esp\.espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe" tools\soak_report.py "%~dp0soak-%1h.log" "%~dp0soak-%1h-report.md" >> "%~dp0soak-report.out" 2>&1
