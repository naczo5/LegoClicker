@echo off
"C:\mingw64\mingw64\bin\g++.exe" -m64 -std=c++11 -shared -o bridge.dll src/main/cpp/bridge.cpp -I"C:/Program Files/Java/jdk-17/include" -I"C:/Program Files/Java/jdk-17/include/win32" -lws2_32 -lopengl32 -lgdi32 -static-libgcc -static-libstdc++ -Wl,--add-stdcall-alias
if %errorlevel% neq 0 exit /b %errorlevel%
echo Compilation successful!
copy /Y bridge.dll "..\LegoClickerCS\bin\Release\net8.0-windows\bridge.dll"
echo Copied bridge.dll to LegoClickerCS Release folder.
