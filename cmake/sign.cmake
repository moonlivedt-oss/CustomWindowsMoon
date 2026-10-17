# Подпись exe своим сертификатом CN=MoonRadial Local (создаётся кнопкой «Настроить подпись» в студии, вкладка
# «Защита»). Нет сертификата или PowerShell не справился - тихо пропускаем: сборка от подписи не зависит.
# Идёт до integrity.cmake: отметка целостности снимается уже с подписанного файла.
# Вызов: cmake -DEXE=<путь к exe> -P sign.cmake
string(REPLACE "/" "\\" native "${EXE}")
set(ps "$c = Get-ChildItem Cert:\\CurrentUser\\My -CodeSigningCert | Where-Object Subject -eq 'CN=MoonRadial Local' | Select-Object -First 1; if ($c) { $r = Set-AuthenticodeSignature -FilePath '${native}' -Certificate $c -HashAlgorithm SHA256; 'sign: ' + $r.Status }")
execute_process(
  COMMAND powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "${ps}"
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
  RESULT_VARIABLE rc
  TIMEOUT 60)
string(STRIP "${out}" out)
if(out)
  message(STATUS "${out}")
endif()
