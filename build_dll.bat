@echo off
echo [bridge.dll] Building C++ bridge...
echo.

cd McInjector
call build.bat
if %errorlevel% neq 0 (
    echo.
    echo [bridge.dll] BUILD FAILED.
    exit /b %errorlevel%
)
cd ..

echo.
echo [bridge.dll] Output:
echo   %~dp0LegoClickerCS\bridge.dll
