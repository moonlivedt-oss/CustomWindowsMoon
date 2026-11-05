<#
.SYNOPSIS
Ищет в изменениях личное (имя Wi-Fi, имя пользователя Windows, имя ПК, свои слова) перед коммитом или push.

.DESCRIPTION
Метки - в файле ВНЕ репозитория: по умолчанию %APPDATA%\MoonDesk\personal-markers.txt, по одной в строке, «#» -
комментарий, короче 3 знаков - пропускаются. Сам список в репозиторий не кладётся: файл внутри репозитория скрипт
читать отказывается. Сверх файла всегда проверяются имя пользователя Windows, имя компьютера, Wi-Fi, к которой ПК
подключён, и все сохранённые сети Wi-Fi (-NoAuto - без них).

Что проверяется:
  без ключей     - подготовленное к коммиту (git diff --cached)
  -Range A..B    - коммиты диапазона: добавленные строки, имена файлов, сообщения и авторы
                   (перед push: -Range origin/main..HEAD)
  -WorkTree      - всё незакоммиченное, включая новые файлы

Найденное показывается как «файл:строка» с меткой, закрытой звёздочками. Код выхода: 0 - чисто, 1 - нашлось,
2 - ошибка (нет git, файл меток внутри репозитория).

.EXAMPLE
powershell -ExecutionPolicy Bypass -File tools\check-personal.ps1 -Range origin/main..HEAD
#>
param(
    [string]$Markers = (Join-Path $env:APPDATA 'MoonDesk\personal-markers.txt'),
    [string]$Range,
    [switch]$WorkTree,
    [switch]$NoAuto
)

$ErrorActionPreference = 'Stop'
$conEnc = [Console]::OutputEncoding  # кодировка консоли: в ней пишет netsh
[Console]::OutputEncoding = [Text.Encoding]::UTF8  # вывод git - в UTF-8 (кириллица в путях и строках)

function Fail([string]$msg) {
    Write-Host $msg -ForegroundColor Red
    exit 2
}

$root = (& git rev-parse --show-toplevel 2>$null)
if ($LASTEXITCODE -ne 0 -or -not $root) { Fail 'Не репозиторий git (или git не найден).' }
$root = [IO.Path]::GetFullPath($root)

# ---- метки ----
$list = New-Object System.Collections.Generic.List[string]
if (Test-Path -LiteralPath $Markers) {
    $full = [IO.Path]::GetFullPath($Markers)
    if ($full.StartsWith($root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        Fail "Файл меток лежит внутри репозитория ($full): так список личного уйдёт вместе с кодом. Перенесите его в %APPDATA%\MoonDesk."
    }
    foreach ($line in [IO.File]::ReadAllLines($full, [Text.Encoding]::UTF8)) {
        $t = $line.Trim()
        if ($t.Length -ge 3 -and -not $t.StartsWith('#')) { $list.Add($t) }
    }
} else {
    Write-Host "Файла меток нет ($Markers) - проверяю только имя пользователя, ПК и Wi-Fi." -ForegroundColor Yellow
}
if (-not $NoAuto) {
    foreach ($v in @($env:USERNAME, $env:COMPUTERNAME)) { if ($v -and $v.Length -ge 3) { $list.Add($v) } }
    # имена сетей: текущая (SSID) и сохранённые профили; вывод netsh - на языке Windows
    $netsh = Join-Path $env:SystemRoot 'System32\netsh.exe'
    if (Test-Path $netsh) {
        [Console]::OutputEncoding = $conEnc
        $ifs = @(& $netsh wlan show interfaces 2>$null)
        $profiles = @(& $netsh wlan show profiles 2>$null)
        [Console]::OutputEncoding = [Text.Encoding]::UTF8
        # без слов на языке Windows: в сведениях об адаптере - строка SSID, в списке профилей - «    <что> : <сеть>»
        foreach ($l in $ifs) { if ($l -match '^\s*SSID\s*:\s*(.+?)\s*$' -and $Matches[1].Length -ge 3) { $list.Add($Matches[1]) } }
        foreach ($l in $profiles) { if ($l -match '^\s{4}\S[^:]*:\s*(.+?)\s*$' -and $Matches[1].Length -ge 3) { $list.Add($Matches[1]) } }
    }
}
$marks = @($list | Sort-Object -Unique)
if ($marks.Count -eq 0) { Write-Host 'Меток нет - проверять нечего.'; exit 0 }

function Mask([string]$s) {
    if ($s.Length -le 2) { return '**' }
    return $s.Substring(0, 2) + ('*' * ($s.Length - 2))
}

$found = 0
function Check([string]$where, [string]$text) {
    foreach ($m in $marks) {
        $i = $text.IndexOf($m, [StringComparison]::OrdinalIgnoreCase)
        if ($i -ge 0) {
            $shown = $text.Replace($text.Substring($i, $m.Length), (Mask $m)).Trim()
            if ($shown.Length -gt 160) { $shown = $shown.Substring(0, 160) + '...' }
            Write-Host ("{0}: {1}" -f $where, $shown) -ForegroundColor Yellow
            $script:found++
            return
        }
    }
}

# добавленные строки из diff: «+++ b/путь», «@@ -a,b +c,d @@», «+строка»
function CheckDiff([string[]]$diff) {
    $file = ''
    $line = 0
    foreach ($l in $diff) {
        if ($l.StartsWith('+++ ')) {
            $file = $l.Substring(4) -replace '^b/', ''
            Check $file $file
            continue
        }
        if ($l -match '^@@ -\d+(?:,\d+)? \+(\d+)') { $line = [int]$Matches[1]; continue }
        if ($l.StartsWith('+')) {
            Check ("{0}:{1}" -f $file, $line) $l.Substring(1)
            $line++
        }
    }
}

$git = @('-c', 'core.quotepath=off')
if ($Range) {
    # каждый коммит отдельно: добавленное и убранное внутри диапазона в суммарной разнице не видно, а на сервер
    # уходят все коммиты
    CheckDiff (& git @git log -p -U0 --no-color --text --format= $Range)
    if ($LASTEXITCODE -ne 0) { Fail "git diff $Range не вышел." }
    foreach ($c in (& git @git log --format='%h' $Range)) {
        $head = (& git @git log -1 --format='%an <%ae>%n%cn <%ce>%n%B' $c) -join "`n"
        foreach ($l in $head -split "`n") { Check "коммит $c" $l }
    }
} elseif ($WorkTree) {
    CheckDiff (& git @git diff -U0 --no-color --text HEAD)
    # git считает UTF-16 двоичным: в разнице такие файлы выглядят мусором, поэтому читаем их целиком
    foreach ($f in (& git @git diff --name-only --diff-filter=d HEAD)) {
        $p = Join-Path $root $f
        if (-not (Test-Path -LiteralPath $p)) { continue }
        $i = (Get-Item -LiteralPath $p)
        if ($i.Length -lt 2 -or $i.Length -gt 4MB) { continue }
        $s = [IO.File]::OpenRead($p); $b0 = $s.ReadByte(); $b1 = $s.ReadByte(); $s.Close()
        if (($b0 -eq 0xFF -and $b1 -eq 0xFE) -or ($b0 -eq 0xFE -and $b1 -eq 0xFF)) {
            $n = 0
            foreach ($l in [IO.File]::ReadAllLines($p)) { $n++; Check ("{0}:{1}" -f $f, $n) $l }
        }
    }
    foreach ($f in (& git @git ls-files --others --exclude-standard)) {
        Check $f $f
        $p = Join-Path $root $f
        if ((Get-Item -LiteralPath $p).Length -gt 4MB) { continue }
        $n = 0
        # без указания кодировки .NET сам узнаёт UTF-16 по BOM (desktop.ini, .reg), остальное читает как UTF-8
        foreach ($l in [IO.File]::ReadAllLines($p)) {
            $n++
            Check ("{0}:{1}" -f $f, $n) $l
        }
    }
} else {
    CheckDiff (& git @git diff --cached -U0 --no-color --text)
}

if ($found) {
    Write-Host ("Найдено личного: {0}. Уберите перед коммитом или push." -f $found) -ForegroundColor Red
    exit 1
}
Write-Host ("Чисто: меток {0}, ничего не нашлось." -f $marks.Count) -ForegroundColor Green
exit 0
