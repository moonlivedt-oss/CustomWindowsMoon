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
target_link_libraries(RadialMenu PRIVATE gdiplus shcore shlwapi shell32 ole32 oleaut32 wtsapi32 user32 gdi32 comctl32 comdlg32 uxtheme windowscodecs winmm uuid dwmapi version propsys winhttp dxva2 wbemuuid oleacc taskschd iphlpapi ws2_32 wininet rstrtmgr wintrust crypt32 bcrypt setupapi pdh userenv)

option(RADIAL_STRIP "убрать символы из exe (OFF - для разбора падений по адресу)" ON)
if(MINGW)
  target_link_options(RadialMenu PRIVATE -municode -static $<$<BOOL:${RADIAL_STRIP}>:-s>)
endif()

# exe кладём в корень проекта рядом с radial.cfg и icons/;
# для проверочной сборки без остановки работающей копии: -DRADIAL_OUT_DIR=<папка>
set(RADIAL_OUT_DIR ${CMAKE_SOURCE_DIR} CACHE PATH "куда класть RadialMenu.exe")
set_target_properties(RadialMenu PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${RADIAL_OUT_DIR})

# RadialTools.exe - удаление и завершение процессов отдельным процессом по кнопке:
# постоянно работающий RadialMenu.exe сам ничего не удаляет и чужие процессы не трогает
add_executable(RadialTools src/act/act.cpp src/core/text.cpp src/tools/common.cpp)
target_include_directories(RadialTools PRIVATE src)
target_compile_definitions(RadialTools PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(RadialTools PRIVATE shlwapi shell32 ole32 user32 bcrypt)
if(MINGW)
  target_link_options(RadialTools PRIVATE -municode -static -s)
endif()
set_target_properties(RadialTools PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${RADIAL_OUT_DIR})
