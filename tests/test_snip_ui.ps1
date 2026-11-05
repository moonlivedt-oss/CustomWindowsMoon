# Проверка снимка в настоящих окнах: карточка, лента, сравнение, закреплённый снимок, разметка.
# Щелчки идут сообщениями окнам, настоящая мышь не двигается. Итог проверяется по пикселям готовой картинки.
# Буфер обмена и настройки пользователя не трогаются: --snip-try пишет в файл (snipimg::SetTestOutput).
# Запуск: build.cmd uitest. Код выхода - число провалов.
param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference = 'Stop'
Add-Type -ReferencedAssemblies System.Drawing @"
using System;using System.Text;using System.Drawing;using System.Runtime.InteropServices;
public class Win{
 public delegate bool EP(IntPtr h,IntPtr l);
 [StructLayout(LayoutKind.Sequential)]public struct R{public int L,T,Rt,B;}
 [DllImport("user32.dll")]public static extern bool EnumWindows(EP e,IntPtr l);
 [DllImport("user32.dll")]public static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out R r);
 [DllImport("user32.dll")]public static extern bool GetClientRect(IntPtr h,out R r);
 [DllImport("user32.dll")]public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")]public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")]public static extern uint GetDpiForWindow(IntPtr h);
 [DllImport("user32.dll")]public static extern bool GetLayeredWindowAttributes(IntPtr h,out uint k,out byte a,out uint f);
 [DllImport("user32.dll",EntryPoint="GetWindowLongPtrW")]public static extern IntPtr GetLong(IntPtr h,int i);
 public static IntPtr Find(uint pid,string cls){IntPtr f=IntPtr.Zero;EnumWindows((h,l)=>{uint p;GetWindowThreadProcessId(h,out p);if(p!=pid)return true;var s=new StringBuilder(64);GetClassName(h,s,64);if(s.ToString()==cls){f=h;return false;}return true;},IntPtr.Zero);return f;}
 public static int Count(uint pid,string cls){int n=0;EnumWindows((h,l)=>{uint p;GetWindowThreadProcessId(h,out p);if(p==pid){var s=new StringBuilder(64);GetClassName(h,s,64);if(s.ToString()==cls)n++;}return true;},IntPtr.Zero);return n;}
 public static bool Near(Bitmap b,int x,int y,int r,int g,int bl,int tol){var c=b.GetPixel(x,y);return Math.Abs(c.R-r)<=tol&&Math.Abs(c.G-g)<=tol&&Math.Abs(c.B-bl)<=tol;}
 public static int CountNear(Bitmap b,int x0,int y0,int x1,int y1,int r,int g,int bl,int tol){int n=0;for(int y=y0;y<y1;y+=1)for(int x=x0;x<x1;x+=1)if(Near(b,x,y,r,g,bl,tol))n++;return n;}
}
"@
$script:failures = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "ok     $what" } else { Write-Host "FAIL   $what"; $script:failures++ }
}
function Param2($x, $y) { [IntPtr]((([int]$y -band 0xffff) -shl 16) -bor ([int]$x -band 0xffff)) }
function MouseAt($h, $x, $y, $buttons = 0) { [void][Win]::PostMessage($h, 0x200, [IntPtr]$buttons, (Param2 $x $y)); Start-Sleep -m 25 }
function ClickAt($h, $x, $y) {
    MouseAt $h $x $y; Start-Sleep -m 50
    [void][Win]::PostMessage($h, 0x201, [IntPtr]1, (Param2 $x $y)); Start-Sleep -m 30
    [void][Win]::PostMessage($h, 0x202, [IntPtr]0, (Param2 $x $y)); Start-Sleep -m 170
}
function RightClickAt($h, $x, $y) {
    MouseAt $h $x $y; Start-Sleep -m 50
    [void][Win]::PostMessage($h, 0x204, [IntPtr]2, (Param2 $x $y)); Start-Sleep -m 30
    [void][Win]::PostMessage($h, 0x205, [IntPtr]0, (Param2 $x $y)); Start-Sleep -m 170
}
function DragFrom($h, $x0, $y0, $x1, $y1) {
    MouseAt $h $x0 $y0
    [void][Win]::PostMessage($h, 0x201, [IntPtr]1, (Param2 $x0 $y0)); Start-Sleep -m 30
    foreach ($i in 1..6) { MouseAt $h ($x0 + ($x1 - $x0) * $i / 6) ($y0 + ($y1 - $y0) * $i / 6) 1 }
    [void][Win]::PostMessage($h, 0x202, [IntPtr]0, (Param2 $x1 $y1)); Start-Sleep -m 170
}
function ClientSize($h) { $r = New-Object Win+R; [void][Win]::GetClientRect($h, [ref]$r); return @($r.Rt, $r.B) }
function WindowSize($h) { $r = New-Object Win+R; [void][Win]::GetWindowRect($h, [ref]$r); return @(($r.Rt - $r.L), ($r.B - $r.T)) }
function LoadPng($file) {
    for ($i = 0; $i -lt 40 -and -not (Test-Path $file); $i++) { Start-Sleep -m 50 }
    $bytes = [IO.File]::ReadAllBytes($file)
    return New-Object Drawing.Bitmap (New-Object IO.MemoryStream (, $bytes))
}

$out = Join-Path ([IO.Path]::GetTempPath()) ("snip-ui-test-" + [Guid]::NewGuid().ToString('N') + ".png")
$proc = Start-Process -FilePath $Exe -ArgumentList '--snip-try', '110', "`"$out`"" -PassThru
$id = [uint32]$proc.Id
try {
    Start-Sleep -m 1800
    # ---- карточка: три снимка, лента видна
    $card = [Win]::Find($id, 'MoonRadialSnip')
    Check ([Win]::IsWindowVisible($card)) 'card is visible'
    $k = [Win]::GetDpiForWindow($card) / 96.0
    function S($v) { [int]($v * $k) }
    $wh = WindowSize $card
    $bodyX = S 128; $bodyTop = S 28; $bodyBottom = $wh[1] - (S 28); $bodyRight = $bodyX + (S 384)
    $stripY = $bodyBottom - (S 32)
    function StripX($i) { $bodyX + (S (16 + $i * 50 + 22)) }
    $compareX = $bodyRight - (S 64)

    # второй снимок ленты - 1920x1080: с ним и работаем
    ClickAt $card (StripX 1) $stripY; Start-Sleep -m 300

    # ---- сравнение
    ClickAt $card $compareX $stripY; Start-Sleep -m 500
    $cmp = [Win]::Find($id, 'MoonRadialSnipCompare')
    Check ($cmp -ne [IntPtr]::Zero) 'compare button opens the compare window'
    DragFrom $cmp 300 400 700 400
    ClickAt $cmp (S 180) (S 30)
    Start-Sleep -m 200
    [void][Win]::PostMessage($cmp, 0x10, [IntPtr]0, [IntPtr]0); Start-Sleep -m 300
    Check ([Win]::Count($id, 'MoonRadialSnipCompare') -eq 0) 'compare window closes'
    RightClickAt $card (StripX 0) $stripY; Start-Sleep -m 500
    Check ([Win]::Count($id, 'MoonRadialSnipCompare') -eq 1) 'right click on a strip thumbnail compares'
    [void][Win]::PostMessage(([Win]::Find($id, 'MoonRadialSnipCompare')), 0x10, [IntPtr]0, [IntPtr]0); Start-Sleep -m 300

    # ---- закреплённый снимок
    ClickAt $card ($bodyRight - (S 32)) ($stripY - (S 44)); Start-Sleep -m 400
    $pin = [Win]::Find($id, 'MoonRadialPin')
    Check ($pin -ne [IntPtr]::Zero) 'pin button pins the screenshot'
    MouseAt $pin 200 150; Start-Sleep -m 150
    # прозрачность: щелчок у левого края шкалы
    $ps = ClientSize $pin
    $barY = $ps[1] - (S 17); $barLeft = ($ps[0] - [Math]::Min((S 160), $ps[0] - (S 16))) / 2
    ClickAt $pin ($barLeft + (S 8)) $barY
    $key = 0; [byte]$alpha = 0; $flags = 0
    [void][Win]::GetLayeredWindowAttributes($pin, [ref]$key, [ref]$alpha, [ref]$flags)
    Check ($alpha -lt 90) "pin opacity slider lowers opacity (alpha=$alpha)"
    # сквозь окно: стиль включён, ярлычок появился, ярлычок выключает
    ClickAt $pin (S 48) (S 18); Start-Sleep -m 300
    $ex = [long][Win]::GetLong($pin, -20)
    Check (($ex -band 0x20) -ne 0) 'click-through sets WS_EX_TRANSPARENT'
    $tab = [Win]::Find($id, 'MoonRadialPinTab')
    Check ($tab -ne [IntPtr]::Zero) 'click-through shows a way back'
    ClickAt $tab 5 5; Start-Sleep -m 300
    $ex = [long][Win]::GetLong($pin, -20)
    Check ((($ex -band 0x20) -eq 0) -and ([Win]::Count($id, 'MoonRadialPinTab') -eq 0)) 'tab turns click-through off'
    # обрезка: окно стало меньше
    MouseAt $pin 200 150; Start-Sleep -m 150
    $before = WindowSize $pin
    ClickAt $pin (S 18) (S 18); Start-Sleep -m 150
    DragFrom $pin 50 50 250 180; Start-Sleep -m 300
    $after = WindowSize $pin
    Check (($after[0] -eq 200) -and ($after[1] -eq 130) -and ($before[0] -gt 200)) "pin crop resizes the pin ($($before[0])x$($before[1]) -> $($after[0])x$($after[1]))"
    RightClickAt $pin 20 20; Start-Sleep -m 300
    Check ([Win]::Count($id, 'MoonRadialPin') -eq 0) 'right click closes the pin'

    # ---- звезда
    ClickAt $card ($bodyX + (S 32)) ($bodyTop + (S 32)); Start-Sleep -m 500

    # ---- редактор: карточка уходит с его кнопок
    ClickAt $card ($bodyX + (S 100)) ($bodyTop + (S 60)); Start-Sleep -m 1200
    $ed = [Win]::Find($id, 'MoonRadialMarkup')
    Check ($ed -ne [IntPtr]::Zero) 'preview click opens the editor'
    Check (-not [Win]::IsWindowVisible($card)) 'card is hidden while editing'
    [void][Win]::SetForegroundWindow($ed); Start-Sleep -m 300
    $cs = ClientSize $ed; $W = $cs[0]; $H = $cs[1]
    $imgW = 1920; $imgH = 1080
    function Tool($i) {
        $gap = 0; if ($i -ge 1) { $gap += 10 }; if ($i -ge 6) { $gap += 10 }
        ClickAt $ed (S (12 + 88 * $i + 44 + $gap)) (S 30)
    }
    $top = (S 96) + (S 8); $areaH = $H - (S 96) - (S 60) - (S 16); $areaW = $W - (S 24) - (S 16)
    $scale = [Math]::Min([Math]::Min($areaW / $imgW, $areaH / $imgH), 1.0)
    $imgLeft = [Math]::Floor((S 20) + ($areaW - $imgW * $scale) / 2); $imgTop = [Math]::Floor($top + ($areaH - $imgH * $scale) / 2)
    function WX($f) { [int]($imgLeft + $imgW * $scale * $f) }
    function WY($f) { [int]($imgTop + $imgH * $scale * $f) }
    function Copied { ClickAt $ed ($W - (S 12) - (S 96) - (S 8) - (S 104) - (S 8) - (S 56)) ($H - (S 30)); Start-Sleep -m 300; return LoadPng $out }

    # секрет на снимке: распознавание идёт в фоне и предлагает скрыть
    $offered = $false
    for ($i = 0; $i -lt 40 -and -not $offered; $i++) {
        Start-Sleep -m 250
        # кнопка «Скрыть» появляется во втором ряду; проверяем нажатием в конце ожидания
        if ($i -eq 24) { $offered = $true }
    }
    ClickAt $ed ($W - (S 12) - (S 88) - (S 84) + (S 39)) (S 72); Start-Sleep -m 500
    $b = Copied
    $cellsOk = [Win]::CountNear($b, 200, 110, 1000, 170, 240, 240, 245, 30)
    Check ($cellsOk -lt 40) "offered blur hides the token text ($cellsOk bright pixels left)"
    $b.Dispose()

    # рамка красным; перетаскивание «Выбором» двигает её, отмена возвращает
    Tool 2
    DragFrom $ed (WX .55) (WY .35) (WX .75) (WY .55)
    $b = Copied
    Check ([Win]::Near($b, [int]($imgW * .65), [int]($imgH * .35), 229, 72, 77, 40)) 'box is drawn where it was dragged'
    $b.Dispose()
    Tool 0
    DragFrom $ed (WX .65) (WY .35) (WX .65) (WY .65)
    $b = Copied
    Check ([Win]::Near($b, [int]($imgW * .65), [int]($imgH * .65), 229, 72, 77, 40) -and -not [Win]::Near($b, [int]($imgW * .65), [int]($imgH * .35), 229, 72, 77, 40)) 'select tool moves the box'
    $b.Dispose()
    ClickAt $ed ($W - (S 12) - (S 36) - (S 6) - (S 18)) (S 30); Start-Sleep -m 200
    $b = Copied
    Check ([Win]::Near($b, [int]($imgW * .65), [int]($imgH * .35), 229, 72, 77, 40)) 'undo puts the box back'
    $b.Dispose()
    ClickAt $ed ($W - (S 12) - (S 18)) (S 30); Start-Sleep -m 200
    # Delete убирает выбранную
    ClickAt $ed (WX .65) (WY .65); Start-Sleep -m 100
    [void][Win]::PostMessage($ed, 0x100, [IntPtr]0x2E, [IntPtr]0); Start-Sleep -m 300
    $b = Copied
    Check (-not [Win]::Near($b, [int]($imgW * .65), [int]($imgH * .65), 229, 72, 77, 40)) 'Delete removes the selected shape'
    $b.Dispose()

    # цвет и толщина видны только у рисующих инструментов; жёлтая толстая стрелка; почти ровная выпрямляется
    Tool 1
    ClickAt $ed ((S 14) + (S 38)) (S 72)
    ClickAt $ed ((S 248) + (S 170)) (S 72)
    DragFrom $ed (WX .2) (WY .7) (WX .6) (WY .71)
    $b = Copied
    $y = [int]($imgH * .7)
    Check ([Win]::Near($b, [int]($imgW * .4), $y, 255, 210, 63, 40)) 'thick yellow arrow is straightened onto its start line'
    $b.Dispose()

    # цифры: следующий шаг - по порядку; кружок нужного цвета
    Tool 5
    ClickAt $ed (WX .2) (WY .85)
    ClickAt $ed (WX .3) (WY .85)
    $b = Copied
    Check (([Win]::CountNear($b, [int]($imgW * .17), [int]($imgH * .82), [int]($imgW * .23), [int]($imgH * .88), 255, 210, 63, 30)) -gt 200) 'step marker is drawn'
    $b.Dispose()

    # пипетка: цвет снимка становится цветом рамки
    ClickAt $ed ((S 158) + (S 41)) (S 72)
    ClickAt $ed (WX .85) (WY .9); Start-Sleep -m 150
    Tool 2
    DragFrom $ed (WX .4) (WY .8) (WX .5) (WY .88)
    $b = Copied
    $picked = $b.GetPixel([int]($imgW * .85), [int]($imgH * .9))
    Check ([Win]::Near($b, [int]($imgW * .45), [int]($imgH * .8), $picked.R, $picked.G, $picked.B, 25)) 'pipette picks a colour from the screenshot'
    $b.Dispose()

    # текст из куска снимка: в тестовый файл, а не в буфер
    Tool 8
    DragFrom $ed (WX .05) (WY .3) (WX .6) (WY .42); Start-Sleep -m 3000
    Check (Test-Path "$out.txt") 'OCR tool copies text from the selected area'

    # перевод снимка (в проверках подставной, без Python и сети): текст закрыт переводом, кнопка возвращает оригинал
    $translateX = $W - (S 12) - (S 96) - (S 8) - (S 104) - (S 8) - (S 112) - (S 8) - (S 104) - (S 8) - (S 92) - (S 8) - (S 52)
    function CountDiff($a, $b, $x0, $y0, $x1, $y1) { $n = 0; for ($y = $y0; $y -lt $y1; $y += 2) { for ($x = $x0; $x -lt $x1; $x += 2) { if (-not [Win]::Near($b, $x, $y, $a.GetPixel($x, $y).R, $a.GetPixel($x, $y).G, $a.GetPixel($x, $y).B, 12)) { $n++ } } }; return $n }
    $before = Copied
    ClickAt $ed $translateX ($H - (S 30)); Start-Sleep -m 4000
    $after = Copied
    $tx0 = [int]($imgW * .08); $ty0 = [int]($imgH * .3); $tx1 = [int]($imgW * .7); $ty1 = [int]($imgH * .41)
    $changed = CountDiff $before $after $tx0 $ty0 $tx1 $ty1
    Check ($changed -gt 200) "translate covers the text on the screenshot ($changed pixels changed)"
    $untouched = CountDiff $before $after 40 ([int]($imgH * .6)) 500 ([int]($imgH * .66))
    Check ($untouched -lt 5) "translate leaves the rest of the screenshot alone ($untouched pixels changed)"
    ClickAt $ed $translateX ($H - (S 30)); Start-Sleep -m 400
    $back = Copied
    $left = CountDiff $before $back $tx0 $ty0 $tx1 $ty1
    Check ($left -lt 5) "the same button brings the original back ($left pixels differ)"
    $before.Dispose(); $after.Dispose(); $back.Dispose()

    # обрезка меняет размер результата; «Готово» возвращает карточку
    Tool 7
    DragFrom $ed (WX .1) (WY .5) (WX .6) (WY .9); Start-Sleep -m 300
    ClickAt $ed ($W - (S 12) - (S 48)) ($H - (S 30)); Start-Sleep -m 900
    Check ([Win]::Count($id, 'MoonRadialMarkup') -eq 0) 'Done closes the editor'
    Check ([Win]::IsWindowVisible($card)) 'card comes back after the editor'
    $b = LoadPng $out
    $expectW = [Math]::Abs((WX .6) - (WX .1)) / $scale
    Check ([Math]::Abs($b.Width - $expectW) -le 4) "crop sets the result size ($($b.Width) wide, expected about $([int]$expectW))"
    $b.Dispose()
}
catch {
    Write-Host "FAIL   test stopped: $_"
    $script:failures++
}
finally {
    if (-not $proc.HasExited) { Stop-Process -Id $id -Force }
    Remove-Item -Force -ErrorAction SilentlyContinue $out, "$out.txt"
}
Write-Host "UI checks failed: $script:failures"
exit $script:failures
