@echo off
echo [LegoClicker.exe] Building C# loader...
echo.

cd LegoClickerCS
dotnet build -c Release 2>&1
if %errorlevel% neq 0 (
    echo.
    echo [LegoClicker.exe] BUILD FAILED.
    exit /b %errorlevel%
)
cd ..

echo.
echo [LegoClicker.exe] Output:
echo   %~dp0LegoClickerCS\bin\Release\net8.0-windows\LegoClicker.exe
