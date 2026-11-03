@echo off
rem Сборка RadialMenu.exe (MSYS2 ucrt64: cmake + ninja + g++). Все сборки - в build\:
rem   build\main  - основная: RadialMenu.exe кладётся в корень проекта, тесты - здесь же
rem   build\check - проверочная: exe остаётся внутри, работающий RadialMenu.exe не трогается
rem build.cmd lint - clang-tidy по src (.clang-tidy)
rem build.cmd snaps [update] - эталонные снимки студии (tests\snaps)
rem build.cmd test - собрать и прогнать тесты без окон (radial_tests, pm_tests), работающий RadialMenu.exe не трогает;
rem упали - код ошибки
setlocal
cd /d "%~dp0"
set PATH=D:\msys64\ucrt64\bin;%PATH%
set MAIN=build\main
set CHECK=build\check
if /i "%~1"=="test" goto test
if /i "%~1"=="lint" goto lint
if /i "%~1"=="snaps" goto snaps
taskkill /im RadialMenu.exe /f >nul 2>&1
cmake -S . -B %MAIN% -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build %MAIN% || exit /b 1
echo OK: %~dp0RadialMenu.exe
exit /b 0

rem build.cmd lint - clang-tidy (проверки bugprone из .clang-tidy) по всему src; работающий exe не трогает
:lint
cmake -S . -B %MAIN% -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
python D:\msys64\ucrt64\bin\run-clang-tidy -p %MAIN% -quiet "\.cpp$" || exit /b 1
echo OK: lint
exit /b 0

rem build.cmd snaps [update] - эталонные снимки студии: собрать в build\check (рядом нужны icons, themes и
rem images - junction), снять разделы с RADIAL_DEMO=1 и сравнить с tests\snaps; update - записать эталоны заново
:snaps
if not exist %CHECK% mkdir %CHECK%
for %%d in (icons themes images) do if not exist %CHECK%\%%d mklink /J %CHECK%\%%d %%d >nul
cmake -S . -B %CHECK% -G Ninja -DCMAKE_BUILD_TYPE=Release -DRADIAL_OUT_DIR=%~dp0%CHECK% || exit /b 1
cmake --build %CHECK% --target RadialMenu || exit /b 1
%CHECK%\RadialMenu.exe --snap-suite "%~dp0tests\snaps" "%~dp0%CHECK%\snaps" %2
set CODE=%ERRORLEVEL%
type %CHECK%\snaps_out.txt
exit /b %CODE%

:test
cmake -S . -B %MAIN% -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build %MAIN% --target radial_tests pm_tests || exit /b 1
%MAIN%\radial_tests.exe || exit /b 1
%MAIN%\player\pm_tests.exe || exit /b 1
echo OK: radial_tests
