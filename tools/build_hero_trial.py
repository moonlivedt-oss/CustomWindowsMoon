"""Build a separate hero experiment while leaving docs/index.html intact."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / 'docs'
source = (DOCS / 'index.html').read_text(encoding='utf-8')
source = source.replace('<title>Radial Menu</title>', '<title>Radial Menu — пробный первый экран</title>')
source = source.replace('background:url(img/hero.webp)', 'background:url(img/hero-observatory.webp)')
# More room for the secondary fan; original copy and live ring stay in place.
style = '''
/* Local artwork experiment: original page is index.html. */
.photo{background-size:100% auto;background-position:left center;-webkit-mask:linear-gradient(90deg,#000 39%,transparent 51%);mask:linear-gradient(90deg,#000 39%,transparent 51%)}
.hero>.wrap{width:100%}
.photo::after{content:"";position:absolute;inset:0;background:linear-gradient(180deg,var(--wine) 12%,transparent 28%,transparent 72%,var(--wine) 88%)}
:root[data-art="original"] .photo::after{display:none}
.copy{width:52%;margin-left:48%}
@media(min-width:641px) and (max-aspect-ratio:4/3){.photo{background-size:100% auto;background-position:left center}}
.art-compare{position:fixed;right:16px;bottom:16px;z-index:2000;display:flex;gap:4px;padding:5px;border:1px solid #ffffff26;border-radius:12px;background:#140b0fee;box-shadow:0 4px 24px #0006;font:13px/1.3 var(--body);color:#f6ece8}
.art-compare button{font:inherit;cursor:pointer;border:0;border-radius:8px;padding:10px 12px;color:inherit;background:transparent}
.art-compare button[aria-pressed="true"]{background:#c8323f}
.art-compare button:focus-visible{outline:2px solid #ff7b5c}
:root[data-art="original"] .photo{background-image:url(img/hero.webp);-webkit-mask:linear-gradient(90deg,#000 55%,transparent 92%);mask:linear-gradient(90deg,#000 55%,transparent 92%)}
@media(max-width:640px){.copy{width:100%;margin-left:0}.photo{-webkit-mask:linear-gradient(#000 30%,transparent 52%);mask:linear-gradient(#000 30%,transparent 52%)}.art-compare{bottom:10px;right:10px}.art-compare button{padding:8px 10px}}
'''
source = source.replace('</style>', style + '</style>', 1)
comparison = '''
<nav class="art-compare" aria-label="Сравнение иллюстраций">
<button type="button" data-art-choice="original" aria-pressed="false">Исходный</button>
<button type="button" data-art-choice="trial" aria-pressed="true">Новый</button>
</nav>
<script>
document.querySelectorAll('[data-art-choice]').forEach(function(button){
  button.addEventListener('click',function(){
    document.documentElement.dataset.art=button.dataset.artChoice;
    document.querySelectorAll('[data-art-choice]').forEach(function(other){
      other.setAttribute('aria-pressed',String(other===button));
    });
  });
});
</script>
'''
source = source.replace('</body>', comparison + '</body>')
(DOCS / 'hero-trial.html').write_text(source, encoding='utf-8')
print('Created docs/hero-trial.html with original/new artwork toggle')
