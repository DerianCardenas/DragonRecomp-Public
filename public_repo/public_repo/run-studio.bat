@echo off
setlocal enabledelayedexpansion

REM ============================================================
REM   DragonRecomp Studio - Lanzador para Windows
REM   Compila (si hace falta) y abre la interfaz grafica.
REM   Haz doble clic en este archivo para iniciar.
REM ============================================================

set "REPO_ROOT=%~dp0"
cd /d "%REPO_ROOT%"

set "STUDIO_EXE=%REPO_ROOT%build\studio\dragon-studio.exe"
set "STUDIO_EXE_CFG=%REPO_ROOT%build\studio\Release\dragon-studio.exe"

REM --- Auto-detectar toolchain y dependencias locales en Windows ---
if exist "%USERPROFILE%\tools\w64devkit\bin" (
    set "PATH=%USERPROFILE%\tools\w64devkit\bin;!PATH!"
)
if exist "%USERPROFILE%\tools\SDL3-3.4.20\x86_64-w64-mingw32\bin" (
    set "PATH=%USERPROFILE%\tools\SDL3-3.4.20\x86_64-w64-mingw32\bin;!PATH!"
)

REM --- Si ya esta compilado, lanzarlo directamente ---
if exist "%STUDIO_EXE%" (
    start "" "%STUDIO_EXE%" %*
    goto :eof
)
if exist "%STUDIO_EXE_CFG%" (
    start "" "%STUDIO_EXE_CFG%" %*
    goto :eof
)

echo ========================================================
echo   DragonRecomp Studio no esta compilado todavia.
echo   Compilando por primera vez (esto puede tardar)...
echo ========================================================
echo.

where cmake >nul 2>nul
if %errorlevel% neq 0 (
    echo Error: CMake no esta instalado o no esta en el PATH.
    echo Descargalo desde https://cmake.org/download/
    echo.
    pause
    exit /b 1
)

REM --- Configurar ---
if not exist "%REPO_ROOT%build" mkdir "%REPO_ROOT%build"
cmake -S "%REPO_ROOT%." -B "%REPO_ROOT%build" -DCMAKE_BUILD_TYPE=Release
if %errorlevel% neq 0 (
    echo.
    echo Error en la configuracion de CMake.
    echo Si falla por SDL3, instalalo con vcpkg:
    echo     vcpkg install sdl3
    echo y vuelve a ejecutar con:
    echo     -DCMAKE_TOOLCHAIN_FILE=C:\ruta\a\vcpkg\scripts\buildsystems\vcpkg.cmake
    echo.
    pause
    exit /b 1
)

REM --- Compilar solo el objetivo de Studio ---
cmake --build "%REPO_ROOT%build" --config Release --target dragon-studio
if %errorlevel% neq 0 (
    echo.
    echo Error durante la compilacion.
    echo.
    pause
    exit /b 1
)

echo.
echo ========================================================
echo   Compilacion exitosa. Iniciando DragonRecomp Studio...
echo ========================================================

if exist "%STUDIO_EXE%" (
    start "" "%STUDIO_EXE%" %*
) else if exist "%STUDIO_EXE_CFG%" (
    start "" "%STUDIO_EXE_CFG%" %*
) else (
    echo No se encontro el ejecutable tras compilar. Revisa la salida anterior.
    pause
    exit /b 1
)

endlocal
