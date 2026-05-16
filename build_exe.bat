@echo off
echo [Aoko.exe] Building C# loader...
echo.

cd Aoko
set "PUBLISH_DIR=bin\Release\net8.0-windows\win-x64\publish"
if exist "%PUBLISH_DIR%" rmdir /s /q "%PUBLISH_DIR%"
dotnet publish -c Release 2>&1
if %errorlevel% neq 0 (
    echo.
    echo [Aoko.exe] BUILD FAILED.
    exit /b %errorlevel%
)
cd ..

echo.
echo [Aoko.exe] Output:
echo   %~dp0Aoko\bin\Release\net8.0-windows\win-x64\publish\Aoko.exe
