@echo off
rem Сборка RadialMenu.exe (MSYS2 ucrt64: cmake + ninja + g++). Все сборки - в build\:
rem   build\main  - основная: RadialMenu.exe кладётся в корень проекта, тесты - здесь же
rem   build\check - проверочная: exe остаётся внутри, работающий RadialMenu.exe не трогается
rem build.cmd lint - clang-tidy по src (.clang-tidy)
rem build.cmd snaps [update] - эталонные снимки студии (tests\snaps)
rem build.cmd uitest - снимок в настоящих окнах (tests\test_snip_ui.ps1), окна на полторы минуты; работающий exe не трогает
rem build.cmd test - собрать и прогнать тесты без окон (radial_tests, pm_tests), работающий RadialMenu.exe не трогает;
rem упали - код ошибки
setlocal
cd /d "%~dp0"
if not defined MSYS2_ROOT set MSYS2_ROOT=D:\msys64
set PATH=%MSYS2_ROOT%\ucrt64\bin;%PATH%
set MAIN=build\main
set CHECK=build\check
if /i "%~1"=="test" goto test
if /i "%~1"=="lint" goto lint
if /i "%~1"=="snaps" goto snaps
if /i "%~1"=="uitest" goto uitest
taskkill /im RadialMenu.exe /f >nul 2>&1
rem PlayerMoon после закрытия окна работает в фоне, а работающий exe нельзя заменить. Сначала просим выйти
rem (сохранит состояние), через 3 секунды закрываем принудительно
tasklist /fi "imagename eq PlayerMoon.exe" | "%SystemRoot%\System32\find.exe" /i "PlayerMoon.exe" >nul && (
  taskkill /im PlayerMoon.exe >nul 2>&1
  "%SystemRoot%\System32\ping.exe" -n 4 127.0.0.1 >nul
  taskkill /im PlayerMoon.exe /f >nul 2>&1
)
cmake -S . -B %MAIN% -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build %MAIN% || exit /b 1
echo OK: %~dp0RadialMenu.exe
exit /b 0

rem build.cmd lint - clang-tidy (проверки bugprone из .clang-tidy) по всему src; работающий exe не трогает
:lint
cmake -S . -B %MAIN% -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
python %MSYS2_ROOT%\ucrt64\bin\run-clang-tidy -p %MAIN% -quiet -extra-arg=-Wno-unknown-warning-option "\.cpp$" || exit /b 1
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

:uitest
if not exist %CHECK% mkdir %CHECK%
for %%d in (icons themes images) do if not exist %CHECK%\%%d mklink /J %CHECK%\%%d %%d >nul
cmake -S . -B %CHECK% -G Ninja -DCMAKE_BUILD_TYPE=Release -DRADIAL_OUT_DIR=%~dp0%CHECK% || exit /b 1
cmake --build %CHECK% --target RadialMenu || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File tests\test_snip_ui.ps1 -Exe "%~dp0%CHECK%\RadialMenu.exe"
exit /b %ERRORLEVEL%

:test
cmake -S . -B %MAIN% -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build %MAIN% --target radial_tests pm_tests || exit /b 1
%MAIN%\radial_tests.exe || exit /b 1
%MAIN%\player\pm_tests.exe || exit /b 1
echo OK: radial_tests
