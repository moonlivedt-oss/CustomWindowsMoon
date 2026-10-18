# Выкладка exe после сборки: линкер пишет во временную папку сборки, а в рабочую папку файл попадает только целым.
# Однажды RadialMenu.exe оказался 0 байт («Невозможно запустить приложение»): прерванная линковка писала прямо
# в рабочий exe. Теперь проверка (больше 0 байт и начинается с MZ) -> копия рядом в <имя>.new -> её проверка ->
# замена одним переименованием. Старый exe остаётся, пока новый не проверен целиком.
# Вызов: cmake -DSRC=<собранный exe> -DDST=<рабочий exe> -P deploy.cmake
function(check_exe path what)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "Сборка испорчена: ${what} не найден (${path})")
  endif()
  file(SIZE "${path}" size)
  if(size LESS 1024)
    message(FATAL_ERROR "Сборка испорчена: ${what} размером ${size} байт (${path}). Пересоберите: build.cmd")
  endif()
  file(READ "${path}" head LIMIT 2 HEX)
  if(NOT head STREQUAL "4d5a")
    message(FATAL_ERROR "Сборка испорчена: ${what} не начинается с MZ (${path}). Пересоберите: build.cmd")
  endif()
endfunction()

check_exe("${SRC}" "собранный exe")
if(SRC STREQUAL DST)
  return()
endif()
set(tmp "${DST}.new")
file(REMOVE "${tmp}")
file(COPY_FILE "${SRC}" "${tmp}")
check_exe("${tmp}" "копия exe")
file(RENAME "${tmp}" "${DST}" RESULT rc)
if(NOT rc STREQUAL "0")
  file(REMOVE "${tmp}")
  message(FATAL_ERROR "Не удалось заменить ${DST}: ${rc}. Программа ещё запущена? Закройте её и пересоберите")
endif()
check_exe("${DST}" "выложенный exe")
