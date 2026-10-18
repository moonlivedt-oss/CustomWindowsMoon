@echo off
rem Сборка RadialMenu.exe (MSYS2 ucrt64: cmake + ninja + g++)
rem build.cmd test - собрать и прогнать тесты без окон (radial_tests), работающий RadialMenu.exe не трогает;
rem упали - код ошибки
setlocal
cd /d "%~dp0"
set PATH=D:\msys64\ucrt64\bin;%PATH%
if /i "%~1"=="test" goto test
taskkill /im RadialMenu.exe /f >nul 2>&1
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build || exit /b 1
echo OK: %~dp0RadialMenu.exe
exit /b 0

:test
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build --target radial_tests || exit /b 1
build\radial_tests.exe || exit /b 1
echo OK: radial_tests
