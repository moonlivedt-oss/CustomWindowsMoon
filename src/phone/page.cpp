// Страница для телефона: скачать то, что бросили на ПК, отправить фото и файлы на ПК, общий буфер и пульт.
// Одна страница без внешних файлов; все запросы - относительные (страница живёт по адресу /<ключ>/).
#include <string>

namespace phone::detail {

const char* PageHtml() {
    return R"PAGE(<!doctype html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#16131a"><title>Компьютер рядом</title>
<style>
:root{--bg:#16131a;--card:#221d27;--line:#342c3b;--fg:#f1ecf4;--mut:#a79caf;--ac:#c8323f;--ok:#3fae6a}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.4 system-ui,sans-serif;
padding:14px 14px 40px}h1{font-size:19px;margin:4px 2px 12px;display:flex;justify-content:space-between;align-items:center}
#st{font-size:13px;color:var(--mut);font-weight:400}#st.off{color:var(--ac)}
section{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:14px;margin:0 0 12px}
h2{font-size:15px;margin:0 0 10px;color:var(--mut);font-weight:600}
button,.btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;border:0;border-radius:12px;
background:#2f2836;color:var(--fg);font:inherit;padding:12px 14px;min-height:46px;text-decoration:none;cursor:pointer}
button.p,.btn.p{background:var(--ac);color:#fff}button:active,.btn:active{filter:brightness(1.25)}
.row{display:flex;gap:8px;flex-wrap:wrap}.row>*{flex:1}
.file{display:flex;align-items:center;gap:10px;padding:8px 0;border-top:1px solid var(--line)}
.file:first-of-type{border-top:0}.file .n{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.file small{color:var(--mut);display:block}.file .btn{flex:0 0 auto;min-height:40px;padding:8px 14px}
.done{color:var(--ok)}textarea{width:100%;min-height:80px;border-radius:12px;border:1px solid var(--line);
background:#1b171f;color:var(--fg);font:inherit;padding:10px;resize:vertical}
#pc{white-space:pre-wrap;word-break:break-word;background:#1b171f;border-radius:12px;padding:10px;min-height:44px;
max-height:180px;overflow:auto;color:var(--fg);margin:0 0 8px}#pc.empty{color:var(--mut)}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}.grid button{min-height:56px}
.bar{height:6px;background:#1b171f;border-radius:3px;overflow:hidden;margin-top:6px}.bar i{display:block;height:100%;
width:0;background:var(--ac)}#up{margin-top:10px;font-size:14px;color:var(--mut)}input[type=file]{display:none}
.hint{font-size:13px;color:var(--mut);margin:8px 2px 0}
</style></head><body>
<h1>Компьютер рядом <span id="st">на связи</span></h1>
<section id="get" hidden><h2>С компьютера</h2><div id="files"></div>
<div class="row" style="margin-top:8px"><button class="p" id="all">Скачать всё</button></div></section>
<section><h2>На компьютер</h2><div class="row">
<button class="p" onclick="pick('photo')">Фото и видео</button><button onclick="pick('file')">Файлы</button></div>
<input type="file" id="fphoto" accept="image/*,video/*" multiple><input type="file" id="ffile" multiple>
<div id="up"></div><p class="hint">Фото и видео лягут в «Изображения\Телефон\дата снимка», файлы - в Загрузки.</p></section>
<section><h2>Буфер компьютера</h2><pre id="pc" class="empty">пока пусто</pre>
<div class="row"><button onclick="copyPc()">Скопировать</button></div>
<h2 style="margin-top:14px">С телефона в буфер ПК</h2><textarea id="tx" placeholder="текст или ссылка"></textarea>
<div class="row" style="margin-top:8px"><button onclick="pasteHere()">Вставить сюда</button>
<button class="p" onclick="sendText()">На ПК</button></div></section>
<section><h2>Пульт</h2><div class="grid">
<button onclick="cmd('vol-')">Тише</button><button onclick="cmd('mute')">Без звука</button><button onclick="cmd('vol+')">Громче</button>
<button onclick="cmd('prev')">Назад</button><button class="p" onclick="cmd('play')">Пауза</button><button onclick="cmd('next')">Дальше</button>
<button onclick="cmd('screen')">Экран</button><button onclick="cmd('lock')">Блок</button><button onclick="sleepPc()">Сон</button>
</div><p class="hint" id="vol"></p></section>
<script>
var seq=-1,files=[],busy=false,q=[];
function $(i){return document.getElementById(i)}
function size(n){if(n<1024)return n+' Б';if(n<1048576)return (n/1024).toFixed(0)+' КБ';
if(n<1073741824)return (n/1048576).toFixed(1).replace('.',',')+' МБ';return (n/1073741824).toFixed(2).replace('.',',')+' ГБ'}
function state(ok){var s=$('st');s.textContent=ok?'на связи':'нет связи';s.className=ok?'':'off'}
function draw(){var h='';files.forEach(function(f,i){h+='<div class="file"><div class="n">'+esc(f.name)+
'<small>'+size(f.size)+(f.done?' <span class="done">скачан</span>':'')+'</small></div><a class="btn" href="f/'+i+
'" download>Скачать</a></div>'});$('files').innerHTML=h;$('get').hidden=!files.length}
function esc(s){return s.replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
$('all').onclick=function(){files.forEach(function(f,i){setTimeout(function(){var a=document.createElement('a');
a.href='f/'+i;a.download=f.name;document.body.appendChild(a);a.click();a.remove()},i*700)})};
function poll(){fetch('poll?seq='+seq,{cache:'no-store'}).then(function(r){if(!r.ok)throw 0;return r.json()})
.then(function(j){state(true);seq=j.seq;files=j.files||[];draw();var p=$('pc');
if(j.clip){p.textContent=j.clip;p.className=''}else{p.textContent='пока пусто';p.className='empty'}
setTimeout(poll,50)}).catch(function(){state(false);setTimeout(poll,3000)})}
function copyPc(){var t=$('pc').className?'':$('pc').textContent;if(!t)return;
if(navigator.clipboard&&window.isSecureContext){navigator.clipboard.writeText(t).then(ok,fallback)}else fallback();
function ok(){toast('Скопировано')}function fallback(){var a=$('tx'),was=a.value;a.value=t;a.select();
try{document.execCommand('copy');toast('Скопировано')}catch(e){toast('Выделите и скопируйте вручную')}a.value=was}}
function pasteHere(){if(navigator.clipboard&&navigator.clipboard.readText)navigator.clipboard.readText()
.then(function(t){$('tx').value=t},function(){toast('Долгое нажатие в поле - Вставить')});else toast('Долгое нажатие в поле - Вставить')}
function sendText(){var t=$('tx').value;if(!t)return;fetch('clip',{method:'POST',body:t}).then(function(r){
if(r.ok){toast('В буфере ПК');$('tx').value=''}else toast('Не дошло')}).catch(function(){toast('Нет связи')})}
function cmd(c){fetch('cmd?c='+encodeURIComponent(c),{method:'POST'}).then(function(r){return r.json()})
.then(function(j){if(j.vol>=0)$('vol').textContent='Громкость: '+Math.round(j.vol*100)+'%'}).catch(function(){toast('Нет связи')})}
function sleepPc(){if(confirm('Усыпить компьютер?'))cmd('sleep')}
function pick(k){$('f'+k).click()}
['photo','file'].forEach(function(k){$('f'+k).onchange=function(){for(var i=0;i<this.files.length;i++)
q.push({f:this.files[i],k:k});this.value='';next()}});
function next(){if(busy||!q.length)return;busy=true;var it=q.shift(),f=it.f,x=new XMLHttpRequest();
var u='up?kind='+it.k+'&name='+encodeURIComponent(f.name)+'&mt='+(f.lastModified||0);
$('up').innerHTML=esc(f.name)+' ('+size(f.size)+')'+(q.length?', ещё '+q.length:'')+'<div class="bar"><i id="pb"></i></div>';
x.upload.onprogress=function(e){if(e.lengthComputable)$('pb').style.width=(100*e.loaded/e.total)+'%'};
x.onload=function(){busy=false;$('up').textContent=x.status==200?'Отправлено: '+f.name:'Не принято: '+f.name+' ('+x.responseText+')';next()};
x.onerror=function(){busy=false;$('up').textContent='Обрыв: '+f.name;next()};x.open('PUT',u);x.send(f)}
var tt;function toast(s){var e=$('vol');e.textContent=s;clearTimeout(tt);tt=setTimeout(function(){e.textContent=''},2500)}
poll();
</script></body></html>
)PAGE";
}

}  // namespace phone::detail
