@echo off
rem Сборка приложения «Компьютер рядом» (Android) - APK в android\MoonNear.apk
rem Нужны JDK 21, Android SDK (platform android-35, build-tools 35) и Gradle 8.14 - пути ниже, их можно
rem переопределить переменными ANDROID_HOME и GRADLE_HOME.
rem build.cmd install - ещё и поставить на телефон по кабелю (adb, отладка по USB включена)
setlocal
cd /d "%~dp0"
rem JDK 26 из системы Gradle 8.14 не запускает - берём 21, если он стоит
if exist D:\tools\jdk-21\bin\java.exe set JAVA_HOME=D:\tools\jdk-21
if "%ANDROID_HOME%"=="" set ANDROID_HOME=D:\tools\android-sdk
if "%GRADLE_HOME%"=="" set GRADLE_HOME=D:\tools\gradle-8.14.3
set PATH=%JAVA_HOME%\bin;%GRADLE_HOME%\bin;%PATH%
echo sdk.dir=%ANDROID_HOME:\=/%> local.properties
call gradle --no-daemon -q testDebugUnitTest assembleRelease || exit /b 1
copy /y app\build\outputs\apk\release\app-release.apk MoonNear.apk >nul || exit /b 1
echo OK: %~dp0MoonNear.apk
if /i not "%~1"=="install" exit /b 0
"%ANDROID_HOME%\platform-tools\adb.exe" install -r MoonNear.apk || exit /b 1
exit /b 0
