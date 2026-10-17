# Отметка целостности: SHA-256 exe сразу после сборки.
# %APPDATA%\MoonDesk\integrity\<sha256 пути в нижнем регистре>.txt - «Защита» сверяет с ней exe (tools/syscheck.cpp).
# Вызов: cmake -DEXE=<путь к exe> -P integrity.cmake
file(SHA256 "${EXE}" hash)
string(REPLACE "/" "\\" native "${EXE}")
string(TOLOWER "${native}" key)
string(SHA256 name "${key}")
set(dir "$ENV{APPDATA}/MoonDesk/integrity")
file(MAKE_DIRECTORY "${dir}")
file(WRITE "${dir}/${name}.txt" "${hash}")
