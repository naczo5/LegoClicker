@echo off
setlocal

echo [Release] Starting full release build...

call build_exe.bat
if %errorlevel% neq 0 (
    echo [Release] C# Build failed.
    exit /b %errorlevel%
)

call build_dll.bat
if %errorlevel% neq 0 (
    echo [Release] C++ Build failed.
    exit /b %errorlevel%
)

echo [Release] Preparing release folder...
set RELEASE_DIR=LegoClicker_Release
set SOURCE_DIR=LegoClickerCS\bin\Release\net8.0-windows

if exist "%RELEASE_DIR%" rmdir /s /q "%RELEASE_DIR%"
mkdir "%RELEASE_DIR%"

echo [Release] Copying files...
copy /Y "%SOURCE_DIR%\LegoClicker.exe" "%RELEASE_DIR%\" >nul
copy /Y "%SOURCE_DIR%\LegoClicker.dll" "%RELEASE_DIR%\" >nul
copy /Y "%SOURCE_DIR%\LegoClicker.deps.json" "%RELEASE_DIR%\" >nul
copy /Y "%SOURCE_DIR%\LegoClicker.runtimeconfig.json" "%RELEASE_DIR%\" >nul
copy /Y "%SOURCE_DIR%\bridge.dll" "%RELEASE_DIR%\" >nul

echo [Release] Creating zip archive...
if exist "LegoClicker_Release.zip" del /q "LegoClicker_Release.zip"

powershell -Command "Compress-Archive -Path '%RELEASE_DIR%\*' -DestinationPath 'LegoClicker_Release.zip' -Force"

echo [Release] Cleaning up temporary folder...
rmdir /s /q "%RELEASE_DIR%"

echo.
echo =======================================
echo [Release] Build and packaging complete!
echo [Release] Output: %~dp0LegoClicker_Release.zip
echo =======================================

endlocal
exit /b 0
