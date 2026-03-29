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
echo [bridge_261.dll] Building C++ 26.1 bridge...
echo.

cd McInjector
call build_261.bat
if %errorlevel% neq 0 (
    echo.
    echo [bridge_261.dll] BUILD FAILED.
    exit /b %errorlevel%
)
cd ..

echo.
echo [bridge.dll] Output:
echo   %~dp0LegoClickerCS\bin\Release\net8.0-windows\bridge.dll
echo   %~dp0LegoClickerCS\bin\Release\net8.0-windows\bridge_261.dll
