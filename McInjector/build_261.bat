@echo off
pushd "%~dp0"
"C:\mingw64\mingw64\bin\g++.exe" -m64 -std=c++11 -shared -o bridge_261.dll src/main/cpp/bridge_261.cpp src/main/cpp/gl_loader.cpp src/main/cpp/jni_core/resolver.cpp src/main/cpp/jni_core/helper_bridge.cpp src/main/cpp/imgui/imgui.cpp src/main/cpp/imgui/imgui_draw.cpp src/main/cpp/imgui/imgui_tables.cpp src/main/cpp/imgui/imgui_widgets.cpp src/main/cpp/imgui/imgui_impl_win32.cpp src/main/cpp/imgui/imgui_impl_opengl3.cpp src/main/cpp/imgui/minhook_src/buffer.c src/main/cpp/imgui/minhook_src/hook.c src/main/cpp/imgui/minhook_src/trampoline.c src/main/cpp/imgui/minhook_src/hde/hde64.c -I"C:/Program Files/Java/jdk-17/include" -I"C:/Program Files/Java/jdk-17/include/win32" -I"src/main/cpp" -I"src/main/cpp/imgui" -I"src/main/cpp/imgui/minhook_src" -I"src/main/cpp/imgui/minhook_src/include" -lws2_32 -lopengl32 -lgdi32 -ldwmapi -static-libgcc -static-libstdc++ -Wl,--add-stdcall-alias
if %errorlevel% neq 0 exit /b %errorlevel%
echo Compilation successful!
if not exist "..\Aoko\bin\Release\net8.0-windows" mkdir "..\Aoko\bin\Release\net8.0-windows"
copy /Y bridge_261.dll "..\Aoko\bin\Release\net8.0-windows\bridge_261.dll"
echo Copied bridge_261.dll to Aoko Release folder.

REM Keep project root + common output folders in sync so Debug/Release/publish all use the same bridge.
copy /Y bridge_261.dll "..\Aoko\bridge_261.dll" >nul
echo Copied bridge_261.dll to ..\Aoko\bridge_261.dll

set "DBG=..\Aoko\bin\Debug\net8.0-windows"
set "REL=..\Aoko\bin\Release\net8.0-windows"
set "PUB=..\Aoko\bin\Release\net8.0-windows\win-x64\publish"
if exist "%DBG%\" (
	copy /Y bridge_261.dll "%DBG%\bridge_261.dll" >nul
	echo Copied bridge_261.dll to %DBG%
)
if exist "%REL%\" (
	copy /Y bridge_261.dll "%REL%\bridge_261.dll" >nul
	echo Copied bridge_261.dll to %REL%
)
if exist "%PUB%\" (
	copy /Y bridge_261.dll "%PUB%\bridge_261.dll" >nul
	echo Copied bridge_261.dll to %PUB%
)
popd
