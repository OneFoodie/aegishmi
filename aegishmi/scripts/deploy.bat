@echo off
setlocal

set "QT_BIN=D:\Qt\6.8.3\mingw_64\bin"
set "MINGW_BIN=D:\Qt\Tools\mingw1310_64\bin"
set "SRC=%~dp0..\build-mingw\bin"
set "DIST=%~dp0..\dist"
set "PATH=%QT_BIN%;%MINGW_BIN%;%PATH%"

if not exist "%SRC%\aegis_hmi.exe" (
    echo [error] aegis_hmi.exe not found. Run scripts\build.bat first.
    exit /b 1
)

rem Recreate a clean dist folder.
if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%"

copy /y "%SRC%\aegis_hmi.exe" "%DIST%\" >nul
copy /y "%SRC%\aegis_sim.exe" "%DIST%\" >nul
copy /y "%SRC%\aegis_linkcheck.exe" "%DIST%\" >nul

rem open62541 (OPC UA) is built as a shared library in amalgamation mode.
rem Copy it to dist, otherwise the exe exits with 0xC0000135 (missing DLL).
if exist "%SRC%\libopen62541.dll" copy /y "%SRC%\libopen62541.dll" "%DIST%\" >nul

rem windeployqt copies required Qt DLLs, plugins and (with --compiler-runtime) MinGW runtime.
windeployqt --release --no-translations --compiler-runtime "%DIST%\aegis_hmi.exe"
if errorlevel 1 (
    echo [error] windeployqt failed for aegis_hmi
    exit /b 1
)

windeployqt --release --no-translations --compiler-runtime "%DIST%\aegis_sim.exe"
if errorlevel 1 (
    echo [error] windeployqt failed for aegis_sim
    exit /b 1
)

rem Copy example test flow configs so the app can load them directly from dist.
if exist "%~dp0..\configs" (
    xcopy /E /I /Y "%~dp0..\configs" "%DIST%\configs" >nul
)

echo DEPLOY_OK
