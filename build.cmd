@echo off
rem Сборка RadialMenu.exe (MSYS2 ucrt64: cmake + ninja + g++)
rem build.cmd lint - clang-tidy по src (.clang-tidy)
rem build.cmd snaps [update] - эталонные снимки студии (tests\snaps)
rem build.cmd test - собрать и прогнать тесты без окон (radial_tests), работающий RadialMenu.exe не трогает;
rem упали - код ошибки
setlocal
cd /d "%~dp0"
set PATH=D:\msys64\ucrt64\bin;%PATH%
if /i "%~1"=="test" goto test
if /i "%~1"=="lint" goto lint
if /i "%~1"=="snaps" goto snaps
taskkill /im RadialMenu.exe /f >nul 2>&1
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build || exit /b 1
echo OK: %~dp0RadialMenu.exe
exit /b 0

rem build.cmd lint - clang-tidy (проверки bugprone из .clang-tidy) по всему src; работающий exe не трогает
:lint
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
python D:\msys64\ucrt64\bin\run-clang-tidy -p build -quiet "\.cpp$" || exit /b 1
echo OK: lint
exit /b 0

rem build.cmd snaps [update] - эталонные снимки студии: собрать в build-check (рядом нужны icons и themes -
rem junction), снять разделы с RADIAL_DEMO=1 и сравнить с tests\snaps; update - записать эталоны заново
:snaps
if not exist build-check\icons mklink /J build-check\icons icons >nul
if not exist build-check\themes mklink /J build-check\themes themes >nul
cmake -S . -B build-check -G Ninja -DCMAKE_BUILD_TYPE=Release -DRADIAL_OUT_DIR=%~dp0build-check || exit /b 1
cmake --build build-check --target RadialMenu || exit /b 1
build-check\RadialMenu.exe --snap-suite "%~dp0tests\snaps" "%~dp0build-check\snaps" %2
set CODE=%ERRORLEVEL%
type build-check\snaps_out.txt
exit /b %CODE%

:test
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build --target radial_tests || exit /b 1
build\radial_tests.exe || exit /b 1
echo OK: radial_tests
