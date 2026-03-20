@echo off
"C:\mingw64\mingw64\bin\g++.exe" -m64 -std=c++11 -shared -o bridge_121.dll src/main/cpp/bridge_121.cpp src/main/cpp/gl_loader.cpp src/main/cpp/imgui/imgui.cpp src/main/cpp/imgui/imgui_draw.cpp src/main/cpp/imgui/imgui_tables.cpp src/main/cpp/imgui/imgui_widgets.cpp src/main/cpp/imgui/imgui_impl_win32.cpp src/main/cpp/imgui/imgui_impl_opengl3.cpp src/main/cpp/imgui/minhook_src/buffer.c src/main/cpp/imgui/minhook_src/hook.c src/main/cpp/imgui/minhook_src/trampoline.c src/main/cpp/imgui/minhook_src/hde/hde64.c -I"C:/Program Files/Java/jdk-17/include" -I"C:/Program Files/Java/jdk-17/include/win32" -I"src/main/cpp/imgui" -I"src/main/cpp/imgui/minhook_src" -I"src/main/cpp/imgui/minhook_src/include" -lws2_32 -lopengl32 -lgdi32 -ldwmapi -static-libgcc -static-libstdc++ -Wl,--add-stdcall-alias
if %errorlevel% neq 0 exit /b %errorlevel%
echo 1.21 Compilation successful!
copy /Y bridge_121.dll "..\LegoClickerCS\bridge_121.dll"
echo Copied bridge_121.dll to LegoClickerCS folder.

REM Also copy to common build output folders so running the exe uses the newest bridge.
set "DBG=..\LegoClickerCS\bin\Debug\net8.0-windows"
set "REL=..\LegoClickerCS\bin\Release\net8.0-windows"
if exist "%DBG%\" (
	copy /Y bridge_121.dll "%DBG%\bridge_121.dll" >nul
	echo Copied bridge_121.dll to %DBG%
)
if exist "%REL%\" (
	copy /Y bridge_121.dll "%REL%\bridge_121.dll" >nul
	echo Copied bridge_121.dll to %REL%
)
