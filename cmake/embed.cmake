# Страница «Телефона рядом» лежит обычными файлами (src/phone/web) - их можно открыть в браузере и править как веб;
# при сборке они попадают в exe строками: cmake -DSRC=<папка web> -DOUT=<phone_web.cpp> -P embed.cmake
set(PAIRS "page.html|PageHtml" "sw.js|ServiceWorkerJs" "manifest.json|ManifestJson")
set(BODY "// Создано cmake/embed.cmake из src/phone/web - правьте те файлы, не этот\n#include <cstddef>\n#include <string>\nnamespace phone::detail {\n")
foreach(P ${PAIRS})
  string(REPLACE "|" ";" P "${P}")
  list(GET P 0 FILE)
  list(GET P 1 FUNC)
  file(READ "${SRC}/${FILE}" TEXT)
  string(FIND "${TEXT}" ")MOONWEB\"" BAD)
  if(NOT BAD EQUAL -1)
    message(FATAL_ERROR "${FILE}: в тексте есть )MOONWEB\" - строку не встроить")
  endif()
  string(APPEND BODY "const char* ${FUNC}() {\n    return R\"MOONWEB(${TEXT})MOONWEB\";\n}\n")
endforeach()
# картинки страницы (web/img: png, webp, svg, jpg) - байтами; страница берёт их как img/<имя>. Держите их лёгкими
# (webp до ~150 КБ): всё это едет внутри exe
file(GLOB IMGS RELATIVE "${SRC}/img" "${SRC}/img/*.png" "${SRC}/img/*.webp" "${SRC}/img/*.svg" "${SRC}/img/*.jpg")
list(SORT IMGS)
set(TABLE "")
set(N 0)
foreach(F ${IMGS})
  if(NOT F MATCHES "^[A-Za-z0-9._-]+$")
    message(FATAL_ERROR "web/img/${F}: в имени только латиница, цифры, точка, дефис и подчёркивание")
  endif()
  file(READ "${SRC}/img/${F}" HEX HEX)
  string(LENGTH "${HEX}" HL)
  math(EXPR LEN "${HL} / 2")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BYTES "${HEX}")
  string(APPEND BODY "static const unsigned char kImg${N}[] = {${BYTES}0};\n")
  string(APPEND TABLE "        {\"${F}\", kImg${N}, ${LEN}},\n")
  math(EXPR N "${N} + 1")
endforeach()
string(APPEND BODY "const unsigned char* WebImage(const std::string& name, size_t* len) {\n"
  "    struct Img {\n        const char* name;\n        const unsigned char* data;\n        size_t len;\n    };\n"
  "    static const Img kAll[] = {\n${TABLE}        {nullptr, nullptr, 0}};\n"
  "    for (const Img& i : kAll)\n        if (i.name && name == i.name) {\n            *len = i.len;\n"
  "            return i.data;\n        }\n    return nullptr;\n}\n")
string(APPEND BODY "}  // namespace phone::detail\n")
# файл переписывается, только если текст поменялся - иначе пересборка без нужды
if(EXISTS "${OUT}")
  file(READ "${OUT}" OLD)
endif()
if(NOT "${OLD}" STREQUAL "${BODY}")
  file(WRITE "${OUT}" "${BODY}")
endif()
