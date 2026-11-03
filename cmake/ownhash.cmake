# SHA-256 своих exe (RadialTools.exe, RadialShutUp.exe) - внутрь RadialMenu.exe: перед запуском он сверяет с ними
# файлы рядом (system/ownexe.cpp). Отметки в %APPDATA% может переписать любая программа пользователя, а то, что вшито в
# сам RadialMenu.exe, - только вместе с ним. Хэш снимается после подписи (sign.cmake идёт в POST_BUILD этих целей).
# Вызов: cmake -DTOOLS=<RadialTools.exe> -DSHUTUP=<RadialShutUp.exe> -DOUT=<файл .cpp> -P ownhash.cmake
file(SHA256 "${TOOLS}" tools)
file(SHA256 "${SHUTUP}" shutup)
set(code "// Создано cmake/ownhash.cmake - не править руками.
#include <cwchar>

namespace tools {

const wchar_t* OwnHash(const wchar_t* name) {
    if (!_wcsicmp(name, L\"RadialTools.exe\")) return L\"${tools}\";
    if (!_wcsicmp(name, L\"RadialShutUp.exe\")) return L\"${shutup}\";
    return L\"\";
}

}  // namespace tools
")
# тот же текст - файл не трогаем: RadialMenu не пересобирается зря
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL code)
  file(WRITE "${OUT}" "${code}")
endif()
