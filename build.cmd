@echo off
rem Сборка RadialMenu.exe (MSYS2 ucrt64: cmake + ninja + g++)
setlocal
cd /d "%~dp0"
set PATH=D:\msys64\ucrt64\bin;%PATH%
taskkill /im RadialMenu.exe /f >nul 2>&1
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build || exit /b 1
echo OK: %~dp0RadialMenu.exe
