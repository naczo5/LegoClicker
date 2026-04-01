@echo off
"C:\mingw64\mingw64\bin\g++.exe" -m64 -std=c++11 -shared -o bridge.dll src/main/cpp/bridge.cpp -I"C:/Program Files/Java/jdk-17/include" -I"C:/Program Files/Java/jdk-17/include/win32" -lws2_32 -lopengl32 -lgdi32 -static-libgcc -static-libstdc++ -Wl,--add-stdcall-alias
if %errorlevel% neq 0 exit /b %errorlevel%
echo Compilation successful!
copy /Y bridge.dll "..\LegoClickerCS\bin\Release\net8.0-windows\bridge.dll"
echo Copied bridge.dll to LegoClickerCS Release folder.

REM Keep project root + common output folders in sync so Debug/Release/publish all use the same bridge.
copy /Y bridge.dll "..\LegoClickerCS\bridge.dll" >nul
echo Copied bridge.dll to ..\LegoClickerCS\bridge.dll

set "DBG=..\LegoClickerCS\bin\Debug\net8.0-windows"
set "REL=..\LegoClickerCS\bin\Release\net8.0-windows"
set "PUB=..\LegoClickerCS\bin\Release\net8.0-windows\win-x64\publish"
if exist "%DBG%\" (
	copy /Y bridge.dll "%DBG%\bridge.dll" >nul
	echo Copied bridge.dll to %DBG%
)
if exist "%REL%\" (
	copy /Y bridge.dll "%REL%\bridge.dll" >nul
	echo Copied bridge.dll to %REL%
)
if exist "%PUB%\" (
	copy /Y bridge.dll "%PUB%\bridge.dll" >nul
	echo Copied bridge.dll to %PUB%
)
