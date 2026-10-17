# Проверка поведения программы в Песочнице Windows. Запускает RadialMenu («Защита» - «Песочница» - «Проверить
# поведение»); работает только внутри песочницы, где всё одноразовое.
#   1. снимок того, как программа может закрепиться в системе: автозапуск, службы, задачи, WMI, hosts, прокси,
#      исключения Defender, исполняемые файлы в AppData, ProgramData и TEMP, запущенные процессы;
#   2. Process Monitor пишет всё, что программа трогает на диске и в реестре; Process Explorer - дерево процессов;
#   3. запуск программы, вы с ней работаете;
#   4. снимок «после» и отчёт на рабочем столе: что появилось и что пропало.
param(
    [Parameter(Mandatory = $true)][string]$Target,
    [Parameter(Mandatory = $true)][string]$Tools,
    [switch]$Network
)
$ErrorActionPreference = 'SilentlyContinue'
$desk = [Environment]::GetFolderPath('Desktop')
$Host.UI.RawUI.WindowTitle = 'Проверка поведения'

function Snapshot {
    $out = New-Object System.Collections.Generic.List[string]
    $runKeys = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run',
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Run',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\RunOnce',
        'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run',
        'HKCU:\Software\Microsoft\Windows\CurrentVersion\RunOnce'
    )
    foreach ($k in $runKeys) {
        $p = Get-ItemProperty $k
        if ($p) {
            $p.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' } |
                ForEach-Object { $out.Add("Автозапуск | $k | $($_.Name) = $($_.Value)") }
        }
    }
    $w = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
    $out.Add("Вход в Windows | Shell = $($w.Shell)")
    $out.Add("Вход в Windows | Userinit = $($w.Userinit)")
    Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options' | ForEach-Object {
        $dbg = (Get-ItemProperty $_.PSPath).Debugger
        if ($dbg) { $out.Add("Подмена запуска | $($_.PSChildName) -> $dbg") }
    }
    Get-CimInstance Win32_Service | ForEach-Object { $out.Add("Служба | $($_.Name) | $($_.StartMode) | $($_.PathName)") }
    Get-ScheduledTask | ForEach-Object {
        $acts = ($_.Actions | ForEach-Object { "$($_.Execute) $($_.Arguments)" }) -join '; '
        $out.Add("Задача | $($_.TaskPath)$($_.TaskName) | $acts")
    }
    foreach ($f in [Environment]::GetFolderPath('Startup'), [Environment]::GetFolderPath('CommonStartup')) {
        Get-ChildItem $f -File | ForEach-Object { $out.Add("Папка автозагрузки | $($_.FullName)") }
    }
    Get-CimInstance -Namespace root/subscription -ClassName __EventConsumer |
        ForEach-Object { $out.Add("Подписка WMI | $($_.CimClass.CimClassName) | $($_.Name)") }
    Get-Content "$env:SystemRoot\System32\drivers\etc\hosts" | Where-Object { $_ -and $_ -notmatch '^\s*#' } |
        ForEach-Object { $out.Add("hosts | $_") }
    $i = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings'
    $out.Add("Прокси | включён=$($i.ProxyEnable) $($i.ProxyServer) $($i.AutoConfigURL)")
    $mp = Get-MpPreference
    foreach ($x in $mp.ExclusionPath) { $out.Add("Исключение Defender | папка $x") }
    foreach ($x in $mp.ExclusionProcess) { $out.Add("Исключение Defender | процесс $x") }
    foreach ($root in $env:APPDATA, $env:LOCALAPPDATA, $env:ProgramData, $env:TEMP, "$env:SystemRoot\Temp") {
        Get-ChildItem $root -Recurse -File -Force -Include *.exe, *.dll, *.scr, *.ps1, *.vbs, *.js, *.bat, *.cmd, *.lnk |
            ForEach-Object { $out.Add("Файл | $($_.FullName)") }
    }
    Get-Process | Where-Object Path | ForEach-Object { $out.Add("Процесс | $($_.Path)") }
    $out | Sort-Object -Unique
}

Write-Host 'Проверка поведения: снимок системы до запуска (до минуты)...' -ForegroundColor Cyan
$before = @(Snapshot)

$pml = Join-Path $desk 'procmon.pml'
Start-Process (Join-Path $Tools 'Procmon64.exe') -ArgumentList '/Quiet', '/Minimized', '/BackingFile', "`"$pml`""
Start-Process (Join-Path $Tools 'procexp64.exe')
if ($Network) { Start-Process (Join-Path $Tools 'tcpview64.exe') }
Write-Host 'Process Monitor и Process Explorer запущены (лицензию Sysinternals они спросят сами).'
Write-Host 'Когда Process Monitor начнёт запись, нажмите Enter - запущу программу.' -ForegroundColor Yellow
Read-Host | Out-Null

Write-Host "Запускаю $Target"
Start-Process $Target -WorkingDirectory (Split-Path $Target)
Write-Host ''
Write-Host 'Поработайте с программой: установите, откройте, понажимайте. Подождите минуту-две.' -ForegroundColor Yellow
Write-Host 'Когда хватит - вернитесь в это окно и нажмите Enter.' -ForegroundColor Yellow
Read-Host | Out-Null

Write-Host 'Снимок после запуска...' -ForegroundColor Cyan
$after = @(Snapshot)
Start-Process (Join-Path $Tools 'Procmon64.exe') -ArgumentList '/Terminate' -Wait

$added = Compare-Object $before $after | Where-Object SideIndicator -eq '=>' | ForEach-Object InputObject
$gone = Compare-Object $before $after | Where-Object SideIndicator -eq '<=' | ForEach-Object InputObject
# процессы сами по себе приходят и уходят; важно, что осталось работать от программы
$persist = $added | Where-Object { $_ -notlike 'Процесс |*' -and $_ -notlike 'Файл |*' }
$files = $added | Where-Object { $_ -like 'Файл |*' }
$procs = $added | Where-Object { $_ -like 'Процесс |*' }

$r = New-Object System.Collections.Generic.List[string]
$r.Add("Проверка поведения: $Target")
$r.Add("Сеть: $(if ($Network) { 'была' } else { 'выключена' })   Время: $(Get-Date -Format 'dd.MM.yyyy HH:mm')")
$r.Add('')
if ($persist) {
    $r.Add('ЗАКРЕПЛЕНИЕ В СИСТЕМЕ (запустится снова после перезагрузки) - главное, на что смотреть:')
    $persist | ForEach-Object { $r.Add("  + $_") }
} else {
    $r.Add('Закрепления в системе не найдено: автозапуск, службы, задачи, WMI, hosts и прокси не менялись.')
}
$r.Add('')
if ($procs) {
    $r.Add('Остались работать процессы:')
    $procs | ForEach-Object { $r.Add("  + $_") }
    $r.Add('')
}
if ($files) {
    $r.Add("Новые исполняемые файлы ($(@($files).Count)):")
    $files | Select-Object -First 80 | ForEach-Object { $r.Add("  + $_") }
    $r.Add('')
}
$changed = $gone | Where-Object { $_ -notlike 'Процесс |*' }
if ($changed) {
    $r.Add('Пропало или изменилось:')
    $changed | ForEach-Object { $r.Add("  - $_") }
    $r.Add('')
}
$r.Add('Как читать: установщик честно оставляет службу или автозапуск своей программы - это нормально, если вы ')
$r.Add('ставили именно её. Подозрительно: задача или Run, запускающие скрипт (powershell, wscript, mshta), exe в ')
$r.Add('TEMP или прямо в AppData, подписка WMI, правка hosts или прокси, исключения Defender.')
$r.Add('«Ничего не найдено» не гарантирует чистоту: хитрые вредоносы видят песочницу и затаиваются.')
$r.Add("Полный журнал действий - $pml (откройте Process Monitor, File - Open).")
$report = Join-Path $desk 'Отчёт проверки.txt'
$r | Set-Content $report -Encoding UTF8
Start-Process notepad.exe "`"$report`""
Write-Host "Отчёт: $report" -ForegroundColor Green
Write-Host 'Закроете окно песочницы - всё, что программа сделала, исчезнет.'
Read-Host 'Enter - закрыть это окно' | Out-Null
