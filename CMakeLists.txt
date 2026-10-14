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
add_executable(RadialMenu WIN32 ${RADIAL_SOURCES} src/res/app.rc)
target_include_directories(RadialMenu PRIVATE src)
target_compile_definitions(RadialMenu PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(RadialMenu PRIVATE gdiplus shcore shlwapi shell32 ole32 oleaut32 wtsapi32 user32 gdi32 comctl32 comdlg32 uxtheme windowscodecs winmm uuid dwmapi version propsys winhttp dxva2 wbemuuid oleacc)

if(MINGW)
  target_link_options(RadialMenu PRIVATE -municode -static -s)
endif()

# exe кладём в корень проекта рядом с radial.cfg и icons/;
# для проверочной сборки без остановки работающей копии: -DRADIAL_OUT_DIR=<папка>
set(RADIAL_OUT_DIR ${CMAKE_SOURCE_DIR} CACHE PATH "куда класть RadialMenu.exe")
set_target_properties(RadialMenu PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${RADIAL_OUT_DIR})
