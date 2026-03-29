@echo off
setlocal
cd /d "%~dp0"

echo [Release] Starting full release build...

call build_dll.bat
if %errorlevel% neq 0 (
    echo [Release] C++ Build failed.
    exit /b %errorlevel%
)

call build_exe.bat
if %errorlevel% neq 0 (
    echo [Release] C# Build failed.
    exit /b %errorlevel%
)

echo [Release] Preparing release folder...
set "RELEASE_DIR=LegoClicker_Release"
set "SOURCE_DIR=LegoClickerCS\bin\Release\net8.0-windows\win-x64\publish"
set "ZIP_PATH=LegoClicker_Release.zip"

if not exist "%SOURCE_DIR%\" (
    echo [Release] ERROR: Publish output folder not found: %SOURCE_DIR%
    exit /b 1
)

if exist "%RELEASE_DIR%" rmdir /s /q "%RELEASE_DIR%"
mkdir "%RELEASE_DIR%"
if %errorlevel% neq 0 (
    echo [Release] ERROR: Failed to create release folder.
    exit /b %errorlevel%
)

echo [Release] Copying files...
xcopy /E /I /Y "%SOURCE_DIR%\*" "%RELEASE_DIR%\" >nul
set "XCOPY_EXIT=%errorlevel%"
if %XCOPY_EXIT% geq 2 (
    echo [Release] ERROR: Failed to copy publish files from %SOURCE_DIR%.
    exit /b %XCOPY_EXIT%
)

copy /Y "McInjector\bridge.dll" "%RELEASE_DIR%\" >nul
if %errorlevel% neq 0 (
    echo [Release] ERROR: Missing McInjector\bridge.dll.
    exit /b %errorlevel%
)

copy /Y "McInjector\bridge_261.dll" "%RELEASE_DIR%\" >nul
if %errorlevel% neq 0 (
    echo [Release] ERROR: Missing McInjector\bridge_261.dll.
    exit /b %errorlevel%
)

if not exist "%RELEASE_DIR%\Data" mkdir "%RELEASE_DIR%\Data"
copy /Y "LegoClickerCS\Data\gtb_wordlist.js" "%RELEASE_DIR%\Data\" >nul
if %errorlevel% neq 0 (
    echo [Release] ERROR: Missing LegoClickerCS\Data\gtb_wordlist.js.
    exit /b %errorlevel%
)

copy /Y "LegoClickerCS\Data\minecraftia.ttf" "%RELEASE_DIR%\Data\" >nul
if %errorlevel% neq 0 (
    echo [Release] ERROR: Missing LegoClickerCS\Data\minecraftia.ttf.
    exit /b %errorlevel%
)

if not exist "%RELEASE_DIR%\LegoClicker.exe" (
    echo [Release] ERROR: Expected executable not found in release folder.
    exit /b 1
)

if exist "%ZIP_PATH%" del /q "%ZIP_PATH%" >nul 2>&1

echo.
echo =======================================
echo [Release] Build complete!
echo [Release] Folder output: %~dp0%RELEASE_DIR%
echo [Release] Auto-zip disabled (zip was unreliable here).
echo [Release] Zip this folder manually when ready.
echo =======================================

endlocal
exit /b 0
