@echo off
setlocal
echo ============================================
echo   AudioRouter - Build Script
echo ============================================

where cmake >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: CMake not found in PATH
    echo Download from https://cmake.org/download/
    pause & exit /b 1
)

where git >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: Git not found in PATH
    echo Download from https://git-scm.com/
    pause & exit /b 1
)

:: Detect VS
set VS_PATH=
for %%v in (2022 2019) do (
    if exist "C:\Program Files\Microsoft Visual Studio\%%v\Community\Common7\IDE\devenv.exe" (
        set VS_YEAR=%%v
        goto :found_vs
    )
    if exist "C:\Program Files\Microsoft Visual Studio\%%v\Professional\Common7\IDE\devenv.exe" (
        set VS_YEAR=%%v
        goto :found_vs
    )
)
echo ERROR: Visual Studio 2019 or 2022 not found
pause & exit /b 1
:found_vs
echo Found Visual Studio %VS_YEAR%

echo.
echo Configuring...
if exist build rmdir /s /q build
cmake -B build -G "Visual Studio 17 2022" -A x64 2>nul
if %errorlevel% neq 0 (
    cmake -B build -G "Visual Studio 16 2019" -A x64
    if %errorlevel% neq 0 (
        echo CMake configuration failed!
        pause & exit /b 1
    )
)

echo.
echo Building Release...
cmake --build build --config Release --parallel
if %errorlevel% neq 0 (
    echo Build failed!
    pause & exit /b 1
)

echo.
echo ============================================
echo   SUCCESS!
echo   Output: build\bin\Release\AudioRouter.exe
echo ============================================
pause
