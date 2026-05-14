@echo off
rem EcoRoute routeOS USB Serial Bridge
rem Usage: start-bridge.bat [COM_PORT] [HTTP_PORT]
rem   COM_PORT   e.g. COM3  (default: auto-detect)
rem   HTTP_PORT  e.g. 8765  (default: 8765)

cd /d "%~dp0"

set PORT=%1
set HTTP_PORT=%2
if "%HTTP_PORT%"=="" set HTTP_PORT=8765

rem Auto-detect port if not given
if "%PORT%"=="" (
    for /f "delims=" %%i in ('python -c "import serial.tools.list_ports; ports=list(serial.tools.list_ports.comports()); [print(p.device) for p in ports if any(x in (p.description or '').lower() for x in ['cp210','ch340','usb serial','uart'])] or ([print(ports[0].device)] if ports else [])" 2^>nul') do set PORT=%%i
)

if "%PORT%"=="" (
    echo [bridge] No serial port found. Available ports:
    python -m serial.tools.list_ports
    echo.
    echo Usage: %~nx0 COM3 [http-port]
    pause
    exit /b 1
)

rem Check pyserial
python -c "import serial" 2>nul || (
    echo [bridge] Installing pyserial...
    pip install pyserial
)

echo [bridge] Port: %PORT%
echo [bridge] Open: http://127.0.0.1:%HTTP_PORT%
echo [bridge] Press Ctrl+C to stop
echo.

python serial-web.py --port %PORT% --http %HTTP_PORT%
pause
