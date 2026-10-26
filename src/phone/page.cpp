// Страница для телефона: скачать брошенное (по одному или архивом), отправить фото и файлы (по 3 сразу, кусками по
// 8 МБ с докачкой), сканер, общий буфер (текст и картинки), папки ПК с просмотром видео и музыки, тачпад и клавиатура,
// пульт, «открыть на ПК», запомнить телефон. Одна страница без внешних файлов; запросы - относительные.
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
nav{display:flex;gap:6px;overflow-x:auto;margin:0 0 12px;padding-bottom:2px}nav button{flex:0 0 auto;min-height:38px;padding:6px 12px;font-size:14px}
nav button.on{background:var(--ac);color:#fff}
section{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:14px;margin:0 0 12px}
h2{font-size:15px;margin:0 0 10px;color:var(--mut);font-weight:600}
button,.btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;border:0;border-radius:12px;
background:#2f2836;color:var(--fg);font:inherit;padding:12px 14px;min-height:46px;text-decoration:none;cursor:pointer}
button.p,.btn.p{background:var(--ac);color:#fff}button:active,.btn:active{filter:brightness(1.25)}
.row{display:flex;gap:8px;flex-wrap:wrap}.row>*{flex:1}
.file{display:flex;align-items:center;gap:10px;padding:8px 0;border-top:1px solid var(--line)}
.file:first-child{border-top:0}.file .n{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.file small{color:var(--mut);display:block}.file .btn,.file button{flex:0 0 auto;min-height:40px;padding:8px 14px}
.done{color:var(--ok)}textarea{width:100%;min-height:70px;border-radius:12px;border:1px solid var(--line);
background:#1b171f;color:var(--fg);font:inherit;padding:10px;resize:vertical}
#pc{white-space:pre-wrap;word-break:break-word;background:#1b171f;border-radius:12px;padding:10px;min-height:44px;
max-height:180px;overflow:auto;margin:0 0 8px}#pc.empty{color:var(--mut)}#pcimg{max-width:100%;border-radius:12px;margin:0 0 8px}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}.grid button{min-height:56px}
.bar{height:6px;background:#1b171f;border-radius:3px;overflow:hidden;margin-top:6px}.bar i{display:block;height:100%;
width:0;background:var(--ac)}#up{margin-top:10px;font-size:14px;color:var(--mut)}input[type=file]{display:none}
.hint{font-size:13px;color:var(--mut);margin:8px 2px 0}
#pad{height:46vh;min-height:240px;border-radius:16px;background:#1b171f;border:1px solid var(--line);touch-action:none;
display:flex;align-items:center;justify-content:center;color:var(--mut);font-size:13px;text-align:center;padding:12px;user-select:none}
.crumbs{font-size:14px;color:var(--mut);margin:0 0 8px;word-break:break-all}.crumbs a{color:var(--fg)}
.ov{position:fixed;inset:0;background:#000d;display:flex;flex-direction:column;align-items:center;justify-content:center;z-index:9;padding:12px}
.ov video,.ov img,.ov audio{max-width:100%;max-height:80vh;border-radius:12px}.ov .row{width:100%;margin-top:10px}
.hide{display:none}
</style></head><body>
<h1>Компьютер рядом <span id="st">на связи</span></h1>
<nav id="tabs"><button data-t="files" class="on">Файлы</button><button data-t="clip">Буфер</button>
<button data-t="pc">Папки ПК</button><button data-t="pad">Тачпад</button><button data-t="remote">Пульт</button></nav>

<div data-p="files">
<section id="get" hidden><h2>С компьютера</h2><div id="files"></div>
<div class="row" style="margin-top:8px"><button class="p" id="all">Скачать всё</button></div></section>
<section><h2>На компьютер</h2><div class="row">
<button class="p" onclick="pick('photo')">Фото и видео</button><button onclick="pick('file')">Файлы</button></div>
<div class="row" style="margin-top:8px"><button onclick="pick('scan')">Сканировать документ</button></div>
<input type="file" id="fphoto" accept="image/*,video/*" multiple><input type="file" id="ffile" multiple>
<input type="file" id="fscan" accept="image/*" capture="environment">
<div id="up"></div><p class="hint">Фото и видео - в «Изображения\Телефон\дата снимка», файлы - в Загрузки, скан -
в «Изображения\Телефон\Сканы», а распознанный текст - в буфер ПК. Оборвался Wi-Fi - продолжит с места обрыва.</p></section>
<section id="pairbox"><h2>Без QR в следующий раз</h2><button id="pair">Запомнить этот телефон</button>
<p class="hint" id="pairhint">Страница получит постоянный адрес - добавьте её в закладки или на главный экран.</p></section>
</div>

<div data-p="clip" class="hide"><section><h2>Буфер компьютера</h2><pre id="pc" class="empty">пока пусто</pre>
<img id="pcimg" class="hide" alt=""><div class="row"><button onclick="copyPc()">Скопировать текст</button>
<a class="btn hide" id="pcsave" download="Буфер ПК.png">Сохранить картинку</a></div></section>
<section><h2>С телефона на ПК</h2><textarea id="tx" placeholder="текст или ссылка"></textarea>
<div class="row" style="margin-top:8px"><button onclick="pasteHere()">Вставить сюда</button>
<button class="p" onclick="sendText()">В буфер ПК</button></div>
<div class="row" style="margin-top:8px"><button onclick="openPc()">Открыть ссылку на ПК</button>
<button onclick="pick('clipimg')">Картинку в буфер ПК</button></div>
<input type="file" id="fclipimg" accept="image/*"></section></div>

<div data-p="pc" class="hide"><section><h2>Папки компьютера</h2><div class="crumbs" id="crumbs"></div><div id="ls"></div>
<div class="row" style="margin-top:8px"><button id="zipdir" class="hide">Эту папку архивом</button></div></section></div>

<div data-p="pad" class="hide"><section><div id="pad">Палец - курсор. Касание - щелчок, двумя пальцами - правый щелчок,
двумя пальцами вверх-вниз - прокрутка, задержать палец - перетаскивание.</div>
<div class="row" style="margin-top:8px"><button onclick="inp('c&b=l')">Левая</button><button onclick="inp('c&b=r')">Правая</button></div>
<div class="row" style="margin-top:8px"><input id="kb" style="flex:3;min-height:46px;border-radius:12px;border:1px solid var(--line);background:#1b171f;color:var(--fg);font:inherit;padding:0 10px" placeholder="печатать на ПК">
<button class="p" onclick="typeKb()">Ввод</button></div>
<div class="grid" style="margin-top:8px"><button onclick="key('back')">Стереть</button><button onclick="key('enter')">Enter</button><button onclick="key('esc')">Esc</button>
<button onclick="key('tab')">Tab</button><button onclick="key('up')">Вверх</button><button onclick="key('space')">Пробел</button>
<button onclick="key('left')">Влево</button><button onclick="key('down')">Вниз</button><button onclick="key('right')">Вправо</button></div>
<p class="hint" id="padoff"></p></section></div>

<div data-p="remote" class="hide"><section><h2>Пульт</h2><div class="grid">
<button onclick="cmd('vol-')">Тише</button><button onclick="cmd('mute')">Без звука</button><button onclick="cmd('vol+')">Громче</button>
<button onclick="cmd('prev')">Назад</button><button class="p" onclick="cmd('play')">Пауза</button><button onclick="cmd('next')">Дальше</button>
<button onclick="cmd('screen')">Экран</button><button onclick="cmd('lock')">Блок</button><button onclick="sleepPc()">Сон</button>
</div></section></div>
<p class="hint" id="vol"></p>
<div class="ov hide" id="ov"><div id="ovbox"></div><div class="row"><a class="btn" id="ovdl">Скачать</a><button class="p" onclick="closeOv()">Закрыть</button></div></div>
<script>
var seq=-1,files=[],state={},q=[],active=0,CH=8<<20,upBytes=0,upT0=0,upTotal=0,upDone=0;
function $(i){return document.getElementById(i)}
function size(n){if(n<1024)return n+' Б';if(n<1048576)return (n/1024).toFixed(0)+' КБ';
if(n<1073741824)return (n/1048576).toFixed(1).replace('.',',')+' МБ';return (n/1073741824).toFixed(2).replace('.',',')+' ГБ'}
function esc(s){return String(s).replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
function sleep(ms){return new Promise(function(r){setTimeout(r,ms)})}
var tt;function toast(s){var e=$('vol');e.textContent=s;clearTimeout(tt);tt=setTimeout(function(){e.textContent=''},2500)}
document.querySelectorAll('#tabs button').forEach(function(b){b.onclick=function(){
document.querySelectorAll('#tabs button').forEach(function(x){x.className=x==b?'on':''});
document.querySelectorAll('[data-p]').forEach(function(p){p.classList.toggle('hide',p.dataset.p!=b.dataset.t)});
if(b.dataset.t=='pc'&&!state.lsInit){state.lsInit=1;roots()}}});
function stateTxt(ok){var s=$('st');s.textContent=ok?'на связи':'нет связи';s.className=ok?'':'off'}
function draw(){var h='';files.forEach(function(f,i){h+='<div class="file"><div class="n">'+esc(f.name)+
'<small>'+size(f.size)+(f.done?' <span class="done">скачан</span>':'')+'</small></div><a class="btn" href="f/'+i+
'" download>Скачать</a></div>'});$('files').innerHTML=h;$('get').hidden=!files.length;
$('all').textContent=state.zip?'Скачать всё одним архивом':'Скачать всё'}
$('all').onclick=function(){if(state.zip){location.href='zip';return}
files.forEach(function(f,i){setTimeout(function(){var a=document.createElement('a');a.href='f/'+i;
a.download=f.name.split('\\').pop();document.body.appendChild(a);a.click();a.remove()},i*700)})};
function poll(){fetch('poll?seq='+seq,{cache:'no-store'}).then(function(r){if(!r.ok)throw 0;return r.json()})
.then(function(j){stateTxt(true);seq=j.seq;files=j.files||[];state.zip=j.zip;state.shares=j.shares;draw();
var p=$('pc');if(j.clip){p.textContent=j.clip;p.className=''}else{p.textContent='пока пусто';p.className='empty'}
var im=$('pcimg'),sv=$('pcsave');if(j.img){var u='clipimg?n='+j.img;if(im.dataset.u!=u){im.src=u;im.dataset.u=u}
im.classList.remove('hide');sv.href=u;sv.classList.remove('hide')}else{im.classList.add('hide');sv.classList.add('hide')}
$('padoff').textContent=j.input?'':'Тачпад выключен в настройках RadialMenu (phone_input).';
if(location.pathname.indexOf('/d/')==0){$('pair').classList.add('hide');$('pairhint').textContent=
'Телефон запомнен - эта страница открывается из закладки, пока ноутбук в своей сети и в RadialMenu включено «ждать всегда».'}
setTimeout(poll,50)}).catch(function(){stateTxt(false);setTimeout(poll,3000)})}
function copyPc(){var t=$('pc').className?'':$('pc').textContent;if(!t)return;
if(navigator.clipboard&&window.isSecureContext){navigator.clipboard.writeText(t).then(ok,fb)}else fb();
function ok(){toast('Скопировано')}function fb(){var a=$('tx'),was=a.value;a.value=t;a.select();
try{document.execCommand('copy');toast('Скопировано')}catch(e){toast('Выделите и скопируйте вручную')}a.value=was}}
function pasteHere(){if(navigator.clipboard&&navigator.clipboard.readText)navigator.clipboard.readText()
.then(function(t){$('tx').value=t},function(){toast('Долгое нажатие в поле - Вставить')});else toast('Долгое нажатие в поле - Вставить')}
function post(u,body){return fetch(u,{method:'POST',body:body})}
function sendText(){var t=$('tx').value;if(!t)return;post('clip',t).then(function(r){
if(r.ok){toast('В буфере ПК');$('tx').value=''}else toast('Не дошло')}).catch(function(){toast('Нет связи')})}
function openPc(){var t=$('tx').value.trim();if(!/^https?:\/\//i.test(t)){toast('Нужна ссылка http(s)');return}
post('open',t).then(function(r){toast(r.ok?'Открыто на ПК':'Не открылось')})}
function cmd(c){post('cmd?c='+encodeURIComponent(c)).then(function(r){return r.json()})
.then(function(j){if(j.vol>=0)toast('Громкость: '+Math.round(j.vol*100)+'%')}).catch(function(){toast('Нет связи')})}
function sleepPc(){if(confirm('Усыпить компьютер?'))cmd('sleep')}
function pick(k){$('f'+k).click()}
function rid(){var a=new Uint8Array(12);crypto.getRandomValues(a);return Array.from(a,function(b){return('0'+b.toString(16)).slice(-2)}).join('')}
['photo','file','scan'].forEach(function(k){$('f'+k).onchange=function(){for(var i=0;i<this.files.length;i++){
var f=this.files[i];q.push({f:f,k:k,id:rid(),off:0});upTotal+=f.size}this.value='';if(!upT0)upT0=Date.now();pump();showUp()}});
$('fclipimg').onchange=function(){var f=this.files[0];this.value='';if(!f)return;toast('Отправляю картинку...');
post('clipimg',f).then(function(r){toast(r.ok?'Картинка в буфере ПК':'Не дошла')}).catch(function(){toast('Нет связи')})};
function pump(){while(active<3&&q.length){var it=q.shift();active++;send(it).then(function(){active--;upDone++;pump();showUp()})}}
async function send(it){var tries=0;for(;;){var end=Math.min(it.off+CH,it.f.size);
var u='up?id='+it.id+'&off='+it.off+'&total='+it.f.size+'&kind='+it.k+'&name='+encodeURIComponent(it.f.name)+'&mt='+(it.f.lastModified||0);
try{var r=await fetch(u,{method:'PUT',body:it.f.slice(it.off,end)});var j=await r.json().catch(function(){return{}});
if(r.ok||r.status==409){upBytes+=Math.max(0,(j.have||0)-it.off);it.off=j.have||0;tries=0;if(j.done)return;showUp();continue}
throw r.status}catch(e){if(++tries>8){it.err=1;toast('Не дошло: '+it.f.name);return}await sleep(1500*tries);
try{var g=await (await fetch('up?id='+it.id)).json();it.off=g.have||0}catch(e2){}}}}
function showUp(){var left=q.length+active;if(!upTotal){$('up').textContent='';return}
var sec=(Date.now()-upT0)/1000,sp=upBytes/Math.max(sec,0.5),rest=Math.max(0,upTotal-upBytes);
$('up').innerHTML=(left?'Отправляю: '+left+' в работе, ':'Отправлено. ')+size(upBytes)+' из '+size(upTotal)+
(left&&sp>1024?', '+size(sp)+'/с, ещё '+Math.ceil(rest/sp)+' с':'')+'<div class="bar"><i style="width:'+(100*upBytes/Math.max(1,upTotal))+'%"></i></div>';
if(!left){upBytes=upTotal=upDone=0;upT0=0}}
// папки ПК
var cur={r:-1,p:[]};
function roots(){cur={r:-1,p:[]};$('zipdir').classList.add('hide');$('crumbs').textContent='';var h='';
(state.shares||[]).forEach(function(n,i){h+='<div class="file"><div class="n">'+esc(n)+'</div><button onclick="openDir('+i+',[])">Открыть</button></div>'});
$('ls').innerHTML=h||'<p class="hint">Компьютер не открыл телефону ни одной папки (phone_share).</p>'}
function rel(){return cur.p.map(encodeURIComponent).join('/')}
function openDir(r,p){cur={r:r,p:p};fetch('ls?r='+r+'&p='+rel()).then(function(x){if(!x.ok)throw 0;return x.json()}).then(function(j){
var c='<a href="#" onclick="roots();return false">Папки</a> / <a href="#" onclick="openDir('+r+',[]);return false">'+esc(j.root)+'</a>';
cur.p.forEach(function(s,i){c+=' / <a href="#" onclick="openDir('+r+',cur.p.slice(0,'+(i+1)+'));return false">'+esc(s)+'</a>'});
$('crumbs').innerHTML=c;var h='';state.items=j.items;
j.items.forEach(function(it,i){h+='<div class="file"><div class="n">'+esc(it.n)+'<small>'+(it.d?'папка':size(it.s))+
'</small></div><button onclick="tapItem('+i+')">'+(it.d?'Открыть':kind(it.n)?'Смотреть':'Скачать')+'</button></div>'});
$('ls').innerHTML=h||'<p class="hint">Пусто</p>';$('zipdir').classList.remove('hide')}).catch(function(){toast('Нет доступа')})}
function kind(n){n=n.toLowerCase();if(/\.(mp4|webm|mkv|mov|m4v)$/.test(n))return'v';if(/\.(mp3|m4a|aac|ogg|opus|flac|wav)$/.test(n))return'a';
if(/\.(jpg|jpeg|png|gif|webp|bmp)$/.test(n))return'i';return''}
function tapItem(i){var it=state.items[i];if(it.d){openDir(cur.r,cur.p.concat([it.n]));return}
var u='get?r='+cur.r+'&p='+rel()+(cur.p.length?'/':'')+encodeURIComponent(it.n),k=kind(it.n);
if(!k){location.href=u+'&dl=1';return}var box=$('ovbox');
box.innerHTML=k=='v'?'<video controls autoplay playsinline src="'+u+'"></video>':k=='a'?'<p>'+esc(it.n)+'</p><audio controls autoplay src="'+u+'"></audio>':'<img src="'+u+'">';
$('ovdl').href=u+'&dl=1';$('ov').classList.remove('hide')}
function closeOv(){$('ovbox').innerHTML='';$('ov').classList.add('hide')}
$('zipdir').onclick=function(){location.href='zipdir?r='+cur.r+'&p='+rel()};
// тачпад
var pad=$('pad'),acc={x:0,y:0,w:0},busy=false,lx=0,ly=0,moved=false,t0=0,fingers=0,drag=false,lp=null;
function inp(q){return post('in?t='+q).then(function(r){if(r.status==403)toast('Тачпад выключен на ПК')})}
function key(k){inp('k&k='+k)}
function typeKb(){var t=$('kb').value;if(!t){key('enter');return}post('type',t).then(function(){$('kb').value=''})}
$('kb').addEventListener('keydown',function(e){if(e.key=='Enter'){e.preventDefault();typeKb()}});
function flush(){if(busy)return;var x=Math.round(acc.x),y=Math.round(acc.y),w=Math.round(acc.w);
if(x||y){acc.x-=x;acc.y-=y;busy=true;inp('m&x='+x+'&y='+y).finally(function(){busy=false})}
else if(w){acc.w-=w;busy=true;inp('w&d='+w).finally(function(){busy=false})}}
setInterval(flush,20);
pad.addEventListener('touchstart',function(e){e.preventDefault();fingers=Math.max(fingers,e.touches.length);var t=e.touches[0];lx=t.clientX;ly=t.clientY;
if(e.touches.length==1){fingers=1;moved=false;t0=Date.now();lp=setTimeout(function(){if(!moved){drag=true;inp('dn');if(navigator.vibrate)navigator.vibrate(30)}},450)}},{passive:false});
pad.addEventListener('touchmove',function(e){e.preventDefault();var t=e.touches[0],dx=t.clientX-lx,dy=t.clientY-ly;lx=t.clientX;ly=t.clientY;
if(Math.abs(dx)+Math.abs(dy)>2){moved=true;clearTimeout(lp)}
if(e.touches.length>=2){acc.w+=-dy*6;return}var sp=Math.sqrt(dx*dx+dy*dy),k=1.4+Math.min(sp,40)*0.08;acc.x+=dx*k;acc.y+=dy*k},{passive:false});
pad.addEventListener('touchend',function(e){e.preventDefault();clearTimeout(lp);if(e.touches.length)return;
if(drag){drag=false;inp('up')}else if(!moved&&Date.now()-t0<300)inp('c&b='+(fingers>=2?'r':'l'));fingers=0},{passive:false});
// запомнить телефон
$('pair').onclick=function(){var n=(navigator.userAgentData&&navigator.userAgentData.platform)||'телефон';
var m=/\(([^)]+)\)/.exec(navigator.userAgent);if(m)n=m[1].split(';').pop().trim()||n;
post('pair',n).then(function(r){return r.json()}).then(function(j){if(j.url){toast('Запомнен');location.replace(j.url)}}).catch(function(){toast('Не вышло')})};
poll();
</script></body></html>
)PAGE";
}

}  // namespace phone::detail
