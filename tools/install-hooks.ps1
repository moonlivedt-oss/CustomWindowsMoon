<#
.SYNOPSIS
Ставит в этот репозиторий хуки git, которые перед коммитом и push запускают tools\check-personal.ps1.

.DESCRIPTION
pre-commit проверяет подготовленное к коммиту, pre-push - все коммиты, которые уходят на сервер. Нашлось личное -
коммит или push не выполняется (обойти разово: git commit --no-verify). Хуки лежат в .git\hooks и в репозиторий не
попадают. Уже стоящий свой хук не перезаписывается без -Force.

.EXAMPLE
powershell -ExecutionPolicy Bypass -File tools\install-hooks.ps1
#>
param([switch]$Force)

$ErrorActionPreference = 'Stop'
$hooks = (& git rev-parse --git-path hooks)
if ($LASTEXITCODE -ne 0) { Write-Host 'Не репозиторий git.' -ForegroundColor Red; exit 2 }
New-Item -ItemType Directory -Force $hooks | Out-Null

$ps = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/check-personal.ps1'
$bodies = @{
    'pre-commit' = "#!/bin/sh`n# check-personal (tools/install-hooks.ps1): личное в подготовленном к коммиту`nexec $ps`n"
    # pre-push получает на вход строки «местная-ссылка местный-sha удалённая-ссылка удалённый-sha»
    'pre-push'   = "#!/bin/sh`n# check-personal (tools/install-hooks.ps1): личное в коммитах, которые уходят`n" +
                   "z=0000000000000000000000000000000000000000`n" +
                   "while read lref lsha rref rsha; do`n" +
                   "  [ `"`$lsha`" = `"`$z`" ] && continue`n" +
                   "  if [ `"`$rsha`" = `"`$z`" ]; then`n" +
                   "    # новая ветка на сервере: от общего с основной веткой коммита (нет его - вся история)`n" +
                   "    base=`$(git merge-base origin/HEAD `"`$lsha`" 2>/dev/null || git rev-list --max-parents=0 `"`$lsha`" | tail -1)`n" +
                   "    range=`"`$base..`$lsha`"`n" +
                   "  else range=`"`$rsha..`$lsha`"; fi`n" +
                   "  $ps -Range `"`$range`" || exit 1`n" +
                   "done`n"
}
foreach ($name in $bodies.Keys) {
    $path = Join-Path $hooks $name
    if ((Test-Path $path) -and -not $Force -and -not (Select-String -Path $path -Pattern 'check-personal' -Quiet)) {
        Write-Host "$name уже есть и он чужой - не трогаю (-Force - заменить)." -ForegroundColor Yellow
        continue
    }
    [IO.File]::WriteAllText($path, $bodies[$name].Replace("`r`n", "`n"), (New-Object Text.UTF8Encoding $false))
    Write-Host "$name поставлен: $path" -ForegroundColor Green
}
