cmake_minimum_required(VERSION 3.20)
project(RadialMenu C CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # для подсказок редактора (clangd)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# исходники раскладываются по папкам src/<часть>/, ресурсы - в src/res
file(GLOB_RECURSE RADIAL_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(FILTER RADIAL_SOURCES EXCLUDE REGEX "/src/radialtools/")  # src/radialtools - отдельный RadialTools.exe
# src/shutup/main.cpp - окно отдельного RadialShutUp.exe; каталог (catalog.cpp) общий с вкладкой «Приватность»
list(FILTER RADIAL_SOURCES EXCLUDE REGEX "/src/shutup/main\\.cpp$")
# страница телефона (src/phone/web) встраивается в exe строками - cmake/embed.cmake
file(GLOB PHONE_WEB LIST_DIRECTORIES false CONFIGURE_DEPENDS src/phone/web/* src/phone/web/img/*)
set(PHONE_WEB_CPP ${CMAKE_BINARY_DIR}/gen/phone_web.cpp)
add_custom_command(OUTPUT ${PHONE_WEB_CPP}
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/src/phone/web -DOUT=${PHONE_WEB_CPP} -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${PHONE_WEB} ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
  COMMENT "Страница телефона -> phone_web.cpp")
# хэши RadialTools.exe и RadialShutUp.exe - внутрь RadialMenu.exe: перед запуском он сверяет с ними файлы рядом
# (system/ownexe.cpp, cmake/ownhash.cmake). Снимаются с подписанных файлов - после POST_BUILD этих целей
set(OWN_HASHES_CPP ${CMAKE_BINARY_DIR}/gen/own_hashes.cpp)
add_custom_command(OUTPUT ${OWN_HASHES_CPP}
  COMMAND ${CMAKE_COMMAND} -DTOOLS=$<TARGET_FILE:RadialTools> -DSHUTUP=$<TARGET_FILE:RadialShutUp> -DOUT=${OWN_HASHES_CPP} -P ${CMAKE_SOURCE_DIR}/cmake/ownhash.cmake
  DEPENDS RadialTools RadialShutUp ${CMAKE_SOURCE_DIR}/cmake/ownhash.cmake
  COMMENT "Хэши своих exe -> own_hashes.cpp")
add_executable(RadialMenu WIN32 ${RADIAL_SOURCES} ${PHONE_WEB_CPP} ${OWN_HASHES_CPP} src/res/app.rc)
target_include_directories(RadialMenu PRIVATE src)
target_compile_definitions(RadialMenu PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(RadialMenu PRIVATE gdiplus shcore shlwapi shell32 ole32 oleaut32 wtsapi32 user32 gdi32 comctl32 comdlg32 uxtheme windowscodecs winmm uuid dwmapi version propsys winhttp dxva2 wbemuuid oleacc taskschd iphlpapi ws2_32 wininet rstrtmgr wintrust crypt32 bcrypt ncrypt secur32 setupapi pdh userenv cfgmgr32 dxgi wlanapi)

# свой код - без предупреждений: любое (вроде L"\*" - неизвестная escape-последовательность) ломает сборку
set(RADIAL_WERROR -Werror)
target_compile_options(RadialMenu PRIVATE ${RADIAL_WERROR})

option(RADIAL_STRIP "убрать символы из exe (OFF - для разбора падений по адресу)" ON)
if(MINGW)
  # символы убирает cmake/symbols.cmake после сборки, успев записать карту для разбора падений
  target_link_options(RadialMenu PRIVATE -municode -static)
endif()

# exe кладём в корень проекта рядом с radial.cfg и icons/;
# для проверочной сборки без остановки работающей копии: -DRADIAL_OUT_DIR=<папка>
# Линкер пишет в <сборка>/link, в RADIAL_OUT_DIR exe попадает только проверенным и целым (cmake/deploy.cmake)
set(RADIAL_OUT_DIR ${CMAKE_SOURCE_DIR} CACHE PATH "куда класть RadialMenu.exe")
set(RADIAL_LINK_DIR ${CMAKE_BINARY_DIR}/link)
set_target_properties(RadialMenu PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${RADIAL_LINK_DIR})

# RadialTools.exe - удаление и завершение процессов отдельным процессом по кнопке:
# постоянно работающий RadialMenu.exe сам ничего не удаляет и чужие процессы не трогает
add_executable(RadialTools src/radialtools/main.cpp src/core/text.cpp src/system/common.cpp)
target_include_directories(RadialTools PRIVATE src)
target_compile_definitions(RadialTools PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(RadialTools PRIVATE shlwapi shell32 ole32 user32 bcrypt)
target_compile_options(RadialTools PRIVATE ${RADIAL_WERROR})
if(MINGW)
  target_link_options(RadialTools PRIVATE -municode -static -s)
endif()
set_target_properties(RadialTools PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${RADIAL_LINK_DIR})

# RadialShutUp.exe - приватность Windows отдельным окном (как O&O ShutUp10): каталог настроек с откатом, тем же
# набором элементов интерфейса, что и студия
add_executable(RadialShutUp WIN32 src/shutup/main.cpp src/shutup/catalog.cpp src/shutup/details.cpp src/ui/ui.cpp src/gfx/icons.cpp
  src/gfx/draw.cpp src/gfx/color.cpp src/core/themes.cpp src/core/config.cpp src/core/text.cpp src/core/fx.cpp
  src/core/perf.cpp src/core/files.cpp src/shutup/shutup.rc)
target_include_directories(RadialShutUp PRIVATE src)
target_compile_definitions(RadialShutUp PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(RadialShutUp PRIVATE gdiplus shlwapi shell32 ole32 oleaut32 user32 gdi32 comctl32 comdlg32
  dwmapi windowscodecs uuid advapi32 taskschd)
target_compile_options(RadialShutUp PRIVATE ${RADIAL_WERROR})
if(MINGW)
  target_link_options(RadialShutUp PRIVATE -municode -static -s)
endif()
set_target_properties(RadialShutUp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${RADIAL_LINK_DIR})

# тесты без окон (tests/): правило TEMP, разбор хода чистки, обход папок, разбор MFT, журнал, дампы падений, фоновые
# задачи; запуск - ctest или build.cmd test
enable_testing()
add_executable(radial_tests EXCLUDE_FROM_ALL tests/test_usage_catalog.cpp src/helpers/markup_logic.cpp tests/tests.cpp tests/tests_top20.cpp tests/tests_more.cpp tests/tests_life.cpp tests/tests_sec.cpp tests/test_icon_catalog.cpp tests/own_hashes_stub.cpp src/core/usage_logic.cpp src/system/ownexe.cpp src/core/elevate_logic.cpp src/phone/phone_sec_logic.cpp src/phone/phone_logic.cpp src/phone/phone_more_logic.cpp src/phone/phone_remote_logic.cpp src/phone/phone_share_logic.cpp src/phone/phone_app_logic.cpp src/phone/cert.cpp src/helpers/shelf_logic.cpp src/helpers/preview_logic.cpp src/helpers/tidy_logic.cpp src/helpers/record_logic.cpp src/helpers/convert_logic.cpp src/system/auto/batcare_logic.cpp src/modes/gamehot_logic.cpp src/net/vpnexit_logic.cpp src/desktop/appbright_logic.cpp src/system/auto/capwatch_logic.cpp src/system/auto/btbattery_logic.cpp src/system/auto/nightupd_logic.cpp src/helpers/remind_logic.cpp src/helpers/picker_logic.cpp src/helpers/awake_logic.cpp src/core/edgecheck.cpp src/core/procsnap.cpp src/core/themes.cpp src/gfx/color.cpp src/net/speedlog.cpp src/system/weekdiff.cpp src/gfx/qr.cpp src/net/wifi.cpp src/system/extrights.cpp src/system/sandbox.cpp src/ring/dropundo.cpp src/system/helper.cpp src/app/rules_logic.cpp src/system/auto/weekreport_logic.cpp src/app/settingsio.cpp src/system/healthscore.cpp src/system/important.cpp src/cards/cardlog_logic.cpp src/system/builderr.cpp src/system/devserv.cpp src/system/drain.cpp src/media/appvol_logic.cpp src/helpers/resume_logic.cpp src/helpers/afterwork_logic.cpp src/modes/gameprep_logic.cpp src/modes/powerpick_logic.cpp src/modes/focus_logic.cpp src/modes/modestats_logic.cpp src/desktop/share_logic.cpp src/system/projver_logic.cpp src/system/winget_logic.cpp src/system/contests_logic.cpp src/system/scancache.cpp src/core/cfgdiff.cpp src/core/config.cpp src/core/files.cpp src/core/perf.cpp src/core/macro.cpp src/core/stroke.cpp src/core/fuzzy.cpp src/core/secrets.cpp src/desktop/zones.cpp src/system/guard.cpp src/system/common.cpp src/system/livesize.cpp
  src/system/mft_parse.cpp src/system/journal.cpp src/shutup/catalog.cpp src/shutup/details.cpp src/system/explain.cpp src/system/security_logic.cpp src/system/sysitems_logic.cpp src/ring/selring_logic.cpp src/helpers/ocrtext_logic.cpp src/helpers/translate_logic.cpp tests/test_translate.cpp src/helpers/snipname.cpp src/core/text.cpp src/core/bg.cpp src/core/crash.cpp src/system/selfwatch.cpp src/core/profile_logic.cpp tests/test_profile.cpp)
target_include_directories(radial_tests PRIVATE src)
target_compile_definitions(radial_tests PRIVATE RADIAL_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_definitions(radial_tests PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(radial_tests PRIVATE shlwapi shell32 ole32 oleaut32 uuid taskschd bcrypt gdiplus wlanapi iphlpapi crypt32 ncrypt)
target_compile_options(radial_tests PRIVATE ${RADIAL_WERROR})
if(MINGW)
  target_link_options(radial_tests PRIVATE -municode -static)
endif()
add_test(NAME radial_tests COMMAND radial_tests)

# карта символов для разбора падений, потом символы прочь из exe (cmake/symbols.cmake)
add_custom_command(TARGET RadialMenu POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:RadialMenu> -DNM=${CMAKE_NM} -DSTRIP_TOOL=${CMAKE_STRIP}
          -DSTRIP=${RADIAL_STRIP} -P ${CMAKE_SOURCE_DIR}/cmake/symbols.cmake)
# подпись своим сертификатом, если он настроен (cmake/sign.cmake), потом выкладка с проверкой (не пустой, начинается
# с MZ) и заменой одним переименованием (cmake/deploy.cmake), потом отметка целостности: SHA-256 выложенного exe -
# «Защита» заметит, если файл потом изменится (cmake/integrity.cmake)
foreach(t RadialMenu RadialTools RadialShutUp)
  set(dst ${RADIAL_OUT_DIR}/${t}.exe)
  add_custom_command(TARGET ${t} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:${t}> -P ${CMAKE_SOURCE_DIR}/cmake/sign.cmake
    COMMAND ${CMAKE_COMMAND} -DSRC=$<TARGET_FILE:${t}> -DDST=${dst} -P ${CMAKE_SOURCE_DIR}/cmake/deploy.cmake
    COMMAND ${CMAKE_COMMAND} -DEXE=${dst} -P ${CMAKE_SOURCE_DIR}/cmake/integrity.cmake)
endforeach()

# PlayerMoon lives in this repository and is built for Windows with the same toolchain.
# It starts on demand; the radial menu does not load the audio engine into its process.
option(RADIAL_PLAYER "Build the bundled PlayerMoon music player" ON)
if(RADIAL_PLAYER)
  add_subdirectory(player)
  add_custom_target(radial_player_bundle
    COMMAND ${CMAKE_COMMAND} -DSRC=$<TARGET_FILE:PlayerMoon>
            -DDST=${RADIAL_OUT_DIR}/apps/PlayerMoon/PlayerMoon.exe -P ${CMAKE_SOURCE_DIR}/cmake/deploy.cmake
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/player/assets ${RADIAL_OUT_DIR}/apps/PlayerMoon/assets
    COMMAND ${CMAKE_COMMAND} -E make_directory ${RADIAL_OUT_DIR}/apps/PlayerMoon/plugins/history ${RADIAL_OUT_DIR}/apps/PlayerMoon/plugins/downloader
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:pm_plugin_history> ${RADIAL_OUT_DIR}/apps/PlayerMoon/plugins/history/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/player/plugins/history/plugin.json ${RADIAL_OUT_DIR}/apps/PlayerMoon/plugins/history/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:pm_plugin_downloader> ${RADIAL_OUT_DIR}/apps/PlayerMoon/plugins/downloader/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/player/plugins/downloader/plugin.json ${RADIAL_OUT_DIR}/apps/PlayerMoon/plugins/downloader/
    DEPENDS PlayerMoon pm_plugin_history pm_plugin_downloader
    COMMENT "PlayerMoon -> apps/PlayerMoon")
  add_dependencies(RadialMenu radial_player_bundle)
endif()
