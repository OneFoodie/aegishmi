@echo off
rem =============================================================================
rem Multi-device demo: two simulator processes + one HMI
rem   dev1: 9527 / 5020 / 4840, 6 channels  (configs/devices/active.json)
rem   dev2: 9528 / 5021 / 4841, 32 channels (configs/devices/lab_32ch.json)
rem Run scripts\deploy.bat first to create dist.
rem =============================================================================
setlocal
set "DIST=%~dp0..\dist"
if not exist "%DIST%\aegis_sim.exe" (
    echo [error] dist\aegis_sim.exe not found. Run scripts\deploy.bat first.
    exit /b 1
)

start "aegis_sim-1" /D "%DIST%" "%DIST%\aegis_sim.exe" --port 9527 --modbus-port 5020 --opcua-port 4840 --config "%DIST%\configs\devices\active.json"
start "aegis_sim-2" /D "%DIST%" "%DIST%\aegis_sim.exe" --port 9528 --modbus-port 5021 --opcua-port 4841 --config "%DIST%\configs\devices\lab_32ch.json"

start "aegis_hmi" /D "%DIST%" "%DIST%\aegis_hmi.exe"
