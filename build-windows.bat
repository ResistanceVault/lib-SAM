@echo off
setlocal

set "ROOT_DIR=%~dp0"
cd /d "%ROOT_DIR%" || exit /b 1

set "BUILD_DIR=%ROOT_DIR%build"
set "CONFIG=%~1"
if not defined CONFIG set "CONFIG=Release"

if not defined CMAKE_GENERATOR set "CMAKE_GENERATOR=Visual Studio 17 2022"
if not defined CMAKE_ARCH set "CMAKE_ARCH=x64"

where cmake >nul 2>nul
if errorlevel 1 (
    echo [error] cmake was not found in PATH.
    exit /b 1
)

if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo [configure] generator=%CMAKE_GENERATOR% arch=%CMAKE_ARCH%
    cmake -S . -B "%BUILD_DIR%" -G "%CMAKE_GENERATOR%" -A "%CMAKE_ARCH%"
    if errorlevel 1 exit /b %errorlevel%
)

echo [build] config=%CONFIG% targets=tts,say
cmake --build "%BUILD_DIR%" --config "%CONFIG%" --target tts say
if errorlevel 1 exit /b %errorlevel%

echo.
echo Built Windows targets:
echo   bin\tts.exe
echo   bin\lua\say.dll
echo   bin\lua\lua54.dll

endlocal
