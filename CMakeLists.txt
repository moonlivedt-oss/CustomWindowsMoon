cmake_minimum_required(VERSION 3.20)
project(RadialMenu CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # для подсказок редактора (clangd)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# исходники раскладываются по папкам src/<часть>/, ресурсы - в src/res
file(GLOB_RECURSE RADIAL_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(FILTER RADIAL_SOURCES EXCLUDE REGEX "/src/act/")  # src/act - отдельный RadialTools.exe
add_executable(RadialMenu WIN32 ${RADIAL_SOURCES} src/res/app.rc)
target_include_directories(RadialMenu PRIVATE src)
target_compile_definitions(RadialMenu PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(RadialMenu PRIVATE gdiplus shcore shlwapi shell32 ole32 oleaut32 wtsapi32 user32 gdi32 comctl32 comdlg32 uxtheme windowscodecs winmm uuid dwmapi version propsys winhttp dxva2 wbemuuid oleacc taskschd iphlpapi ws2_32 wininet rstrtmgr wintrust crypt32 bcrypt setupapi pdh userenv cfgmgr32 dxgi)

# свой код - без предупреждений: любое (вроде L"\*" - неизвестная escape-последовательность) ломает сборку
set(RADIAL_WERROR -Werror)
target_compile_options(RadialMenu PRIVATE ${RADIAL_WERROR})

option(RADIAL_STRIP "убрать символы из exe (OFF - для разбора падений по адресу)" ON)
if(MINGW)
  target_link_options(RadialMenu PRIVATE -municode -static $<$<BOOL:${RADIAL_STRIP}>:-s>)
endif()

# exe кладём в корень проекта рядом с radial.cfg и icons/;
# для проверочной сборки без остановки работающей копии: -DRADIAL_OUT_DIR=<папка>
# Линкер пишет в <сборка>/link, в RADIAL_OUT_DIR exe попадает только проверенным и целым (cmake/deploy.cmake)
set(RADIAL_OUT_DIR ${CMAKE_SOURCE_DIR} CACHE PATH "куда класть RadialMenu.exe")
set(RADIAL_LINK_DIR ${CMAKE_BINARY_DIR}/link)
set_target_properties(RadialMenu PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${RADIAL_LINK_DIR})

# RadialTools.exe - удаление и завершение процессов отдельным процессом по кнопке:
# постоянно работающий RadialMenu.exe сам ничего не удаляет и чужие процессы не трогает
add_executable(RadialTools src/act/act.cpp src/core/text.cpp src/tools/common.cpp)
target_include_directories(RadialTools PRIVATE src)
target_compile_definitions(RadialTools PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(RadialTools PRIVATE shlwapi shell32 ole32 user32 bcrypt)
target_compile_options(RadialTools PRIVATE ${RADIAL_WERROR})
if(MINGW)
  target_link_options(RadialTools PRIVATE -municode -static -s)
endif()
set_target_properties(RadialTools PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${RADIAL_LINK_DIR})

# тесты без окон (tests/): правило TEMP, разбор хода чистки, обход папок, разбор MFT, журнал, дампы падений, фоновые
# задачи; запуск - ctest или build.cmd test
enable_testing()
add_executable(radial_tests EXCLUDE_FROM_ALL tests/tests.cpp tests/tests_top20.cpp src/core/macro.cpp src/core/stroke.cpp src/core/fuzzy.cpp src/core/secrets.cpp src/desk/zones.cpp src/tools/guard.cpp src/tools/common.cpp src/tools/livesize.cpp
  src/tools/mft_parse.cpp src/tools/journal.cpp src/core/text.cpp src/core/bg.cpp src/core/crash.cpp src/tools/selfwatch.cpp)
target_include_directories(radial_tests PRIVATE src)
target_compile_definitions(radial_tests PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(radial_tests PRIVATE shlwapi shell32 ole32 uuid bcrypt)
target_compile_options(radial_tests PRIVATE ${RADIAL_WERROR})
if(MINGW)
  target_link_options(radial_tests PRIVATE -municode -static)
endif()
add_test(NAME radial_tests COMMAND radial_tests)

# подпись своим сертификатом, если он настроен (cmake/sign.cmake), потом выкладка с проверкой (не пустой, начинается
# с MZ) и заменой одним переименованием (cmake/deploy.cmake), потом отметка целостности: SHA-256 выложенного exe -
# «Защита» заметит, если файл потом изменится (cmake/integrity.cmake)
foreach(t RadialMenu RadialTools)
  set(dst ${RADIAL_OUT_DIR}/${t}.exe)
  add_custom_command(TARGET ${t} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:${t}> -P ${CMAKE_SOURCE_DIR}/cmake/sign.cmake
    COMMAND ${CMAKE_COMMAND} -DSRC=$<TARGET_FILE:${t}> -DDST=${dst} -P ${CMAKE_SOURCE_DIR}/cmake/deploy.cmake
    COMMAND ${CMAKE_COMMAND} -DEXE=${dst} -P ${CMAKE_SOURCE_DIR}/cmake/integrity.cmake)
endforeach()
