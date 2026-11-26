@echo off
setlocal

set "QT_MSVC=D:/Qt/6.8.3/msvc2022_64"
set "QT_MINGW=D:/Qt/6.8.3/mingw_64"
set "MINGW_BIN=D:\Qt\Tools\mingw1310_64\bin"
set "MSVC_DIR=D:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.44.35207"
set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Release"

if exist "%MSVC_DIR%\bin\HostX64\x64\cl.exe" goto :msvc
goto :mingw

:msvc
echo [info] using MSVC toolchain
call "D:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set "PATH=%MSVC_DIR%\bin\HostX64\x64;%PATH%"
set "INCLUDE=%MSVC_DIR%\include;%INCLUDE%"
set "LIB=%MSVC_DIR%\lib\x64;%LIB%"
set "BUILD_DIR=%~dp0..\build"
cmake -S "%~dp0.." -B "%BUILD_DIR%" -G Ninja ^
      -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
      -DCMAKE_PREFIX_PATH=%QT_MSVC%
if errorlevel 1 exit /b 1
goto :build

:mingw
echo [info] using MinGW toolchain
set "PATH=%MINGW_BIN%;%PATH%"
set "BUILD_DIR=%~dp0..\build-mingw"
cmake -S "%~dp0.." -B "%BUILD_DIR%" -G Ninja ^
      -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
      -DCMAKE_PREFIX_PATH=%QT_MINGW% ^
      -DCMAKE_C_COMPILER=%MINGW_BIN%\gcc.exe ^
      -DCMAKE_CXX_COMPILER=%MINGW_BIN%\g++.exe
if errorlevel 1 exit /b 1
goto :build

:build
cmake --build "%BUILD_DIR%"
if errorlevel 1 exit /b 1
echo BUILD_OK
