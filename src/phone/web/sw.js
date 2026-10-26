var OFF='<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'+
'<title>ПК рядом</title><body style="margin:0;padding:24px;background:#16131a;color:#f1ecf4;font:16px/1.4 system-ui,sans-serif">'+
'<h1 style="font-size:19px">Компьютер не отвечает</h1><p>Ноутбук выключен, спит или в другой сети, либо в RadialMenu не '+
'включено «Ждать запомненный телефон». Поделитесь ещё раз, когда он будет рядом.</p>'+
'<button onclick="location.reload()" style="border:0;border-radius:12px;background:#c8323f;color:#fff;font:inherit;padding:12px 16px">Повторить</button>';
self.addEventListener('install',function(){self.skipWaiting()});
self.addEventListener('activate',function(e){e.waitUntil(self.clients.claim())});
self.addEventListener('fetch',function(e){if(e.request.mode!='navigate')return;
e.respondWith(fetch(e.request).catch(function(){return new Response(OFF,{headers:{'Content-Type':'text/html; charset=utf-8'}})}))});
