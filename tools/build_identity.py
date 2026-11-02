"""Reproducible vector identity and themed icons. Requires Pillow only.

python tools/build_identity.py          # generate and install
python tools/build_identity.py --check  # verify generated assets
"""
from pathlib import Path
import argparse
import html
import json
import math
import shutil
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT.parent
OUT = ROOT / 'art' / 'identity'
BACKUP = ROOT / 'backups' / 'identity-originals'
S = 4


class Vector:
    """Same geometry for editable SVG and supersampled PNG; no raster tracing."""
    def __init__(self):
        self.im = Image.new('RGBA', (256*S, 256*S))
        self.d = ImageDraw.Draw(self.im)
        self.parts = []

    def line(self, pts, color, w=10):
        self.parts.append(f'<polyline points="{" ".join(f"{x},{y}" for x,y in pts)}" fill="none" stroke="{color}" stroke-width="{w}" stroke-linecap="round" stroke-linejoin="round"/>')
        p = [(round(x*S), round(y*S)) for x,y in pts]
        self.d.line(p, fill=color, width=round(w*S), joint='curve')
        for x,y in p:
            r=w*S/2
            self.d.ellipse((x-r,y-r,x+r,y+r),fill=color)

    def rect(self, box, color, radius=0):
        x,y,r,b=box
        self.parts.append(f'<rect x="{x}" y="{y}" width="{r-x}" height="{b-y}" rx="{radius}" fill="{color}"/>')
        self.d.rounded_rectangle(tuple(round(v*S) for v in box),radius=radius*S,fill=color)

    def circle(self, x,y,r,color):
        self.parts.append(f'<circle cx="{x}" cy="{y}" r="{r}" fill="{color}"/>')
        self.d.ellipse(((x-r)*S,(y-r)*S,(x+r)*S,(y+r)*S),fill=color)

    def poly(self, pts,color):
        self.parts.append(f'<polygon points="{" ".join(f"{x},{y}" for x,y in pts)}" fill="{color}"/>')
        self.d.polygon([(x*S,y*S) for x,y in pts],fill=color)

    def arc(self,x,y,r,start,end,color,w=10):
        self.line([(x+r*math.cos(math.radians(a)), y+r*math.sin(math.radians(a))) for a in range(start,end+1,3)],color,w)

    def save(self, path):
        path.parent.mkdir(parents=True,exist_ok=True)
        self.im.resize((256,256),Image.Resampling.LANCZOS).save(path.with_suffix('.png'))
        path.with_suffix('.svg').write_text('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">'+''.join(self.parts)+'</svg>',encoding='utf-8')


def draw(kind, accent, bg='#171D2D', light='#EFF4FF'):
    v=Vector()
    v.rect((12,12,244,244),bg,62)
    v.rect((27,27,229,229),'#293247' if bg=='#171D2D' else bg,49)
    v.rect((30,30,226,226),bg,47)
    a=accent; f=light
    L=lambda p,c=f,w=10:v.line(p,c,w)
    C=lambda x,y,r,c=a:v.circle(x,y,r,c)
    if kind=='radial':
        # Two levels of sectors, like the actual folder and contents rings.
        def sector(inner, outer, start, end, color):
            angles=list(range(start,end+1,2))+[end]
            pts=[(128+r*math.cos(math.radians(t)),128+r*math.sin(math.radians(t)))
                 for r,aa in [(outer,angles),(inner,list(reversed(angles)))] for t in aa]
            v.poly(pts,color)
        for start in (-175,-115,-55,5,65,125):
            sector(46,68,start,start+48,a if start==-55 else f)
        for start in (-58,-26,6):
            sector(79,96,start,start+23,a)
        C(128,128,25,'#293247')
        C(128,128,12,a)
        # A compact pointer selects the brighter sector, with a dark keyline.
        pointer=[(157,105),(178,164),(187,143),(209,136)]
        L(pointer+[pointer[0]],bg,10)
        v.poly(pointer,f)
    elif kind=='translate':
        v.rect((55,62,139,139),a,18);v.rect((115,113,201,191),f,18)
        L([(74,119),(96,81),(119,119)],bg,7);L([(82,106),(110,106)],bg,6)
        L([(135,135),(181,135)],bg,6);L([(161,131),(155,156),(138,171)],bg,6);L([(143,149),(178,172)],bg,6)
    elif kind in ('code','cpp'):
        L([(94,87),(58,127),(94,167)]);L([(162,87),(198,127),(162,167)])
        L([(143,73),(114,183)],a,12)
    elif kind in ('book','study'):
        v.poly([(57,72),(116,80),(128,94),(141,80),(199,72),(199,170),(141,180),(128,191),(115,180),(57,170)],a)
        L([(128,97),(128,173)],bg,7);L([(73,93),(108,99)],bg,6);L([(149,99),(183,93)],bg,6)
    elif kind=='core':
        for i in range(6):
            t=math.radians(i*60);x=128+66*math.cos(t);y=128+66*math.sin(t)
            L([(128,128),(x,y)],f,8);C(x,y,12,f)
        v.poly([(128,88),(164,108),(164,148),(128,168),(92,148),(92,108)],a)
        C(128,128,12,bg)
    elif kind=='bundle':
        v.poly([(60,96),(128,60),(196,96),(196,166),(128,202),(60,166)],a)
        L([(60,96),(128,132),(196,96)],bg,7);L([(128,132),(128,202)],bg,7);L([(94,78),(161,114),(161,145)],f,8)
    elif kind=='image':
        v.rect((57,62,199,194),a,20);v.rect((68,73,188,183),bg,12)
        v.poly([(72,173),(111,122),(139,153),(160,132),(184,173)],f);C(159,102,14)
    elif kind=='network':
        v.arc(128,144,69,220,320,a,12);v.arc(128,144,43,220,320,f,12);C(128,146,12)
        L([(78,185),(106,185),(118,165),(135,202),(148,180),(179,180)],a,8)
    elif kind=='boost':
        v.poly([(142,52),(76,139),(121,139),(108,205),(183,110),(139,110)],a)
        L([(65,168),(87,168)],f,7);L([(166,76),(187,76)],f,7)
    elif kind in ('contest','career'):
        v.rect((91,65,165,134),a,15);L([(128,135),(128,177)],a,12);L([(98,187),(158,187)],f,10)
        L([(86,82),(64,82),(68,114),(88,128)],f,8);L([(170,82),(192,82),(188,114),(168,128)],f,8)
        C(184,174,22);L([(184,162),(184,175),(193,180)],bg,5)
    elif kind in ('launcher','terminal'):
        v.rect((55,65,201,191),f,20);v.rect((64,74,192,182),bg,13)
        L([(81,106),(104,127),(81,148)],a,10);L([(124,153),(164,153)],a,9)
        if kind=='launcher':v.poly([(145,91),(145,128),(177,109)],a)
    elif kind=='background':
        v.rect((58,75,181,190),f,20);v.rect((78,56,201,171),a,20)
        v.poly([(90,158),(124,111),(149,138),(170,110),(190,158)],bg);C(163,86,12,f)
    elif kind=='game':
        v.rect((56,88,200,171),a,34);L([(78,129),(111,129)],bg,9);L([(94,113),(94,145)],bg,9)
        C(164,119,7,bg);C(180,139,7,bg);L([(113,68),(143,68)],f,8)
    elif kind=='training':
        L([(68,185),(68,148),(100,148),(100,116),(136,116),(136,82),(185,82)],a,14)
        L([(164,61),(185,82),(164,103)],f,10)
    elif kind=='forest':
        # Les contains the Volna festival design: Volga, sunset, three contours.
        C(161,91,28,'#FFC24B')
        v.rect((126,106,198,123),bg)
        for y,color in [(113,a),(143,'#EE5D9A'),(173,f)]:
            pts=[(x,y+12*math.sin((x-59)*math.pi/70)) for x in range(59,199,2)]
            L(pts,color,11)
    elif kind=='backup':
        v.arc(128,125,65,3,285,a,13);L([(147,57),(167,68),(160,90)],a,10)
        L([(128,92),(128,128),(153,144)],f,10);L([(70,184),(184,184)],f,10)
    elif kind=='moon':
        C(122,124,65);C(150,101,56,bg);C(184,166,10,f);C(184,71,6,f)
    elif kind=='projects':
        v.poly([(128,57),(165,115),(152,168),(104,168),(91,115)],a);C(128,115,15,bg)
        L([(88,151),(76,180)],f,9);L([(168,151),(180,180)],f,9);L([(118,188),(128,208),(138,188)],a,9)
    elif kind=='music':
        L([(106,165),(106,82),(178,66),(178,151)],a,12);C(88,170,20,f);C(160,156,20,f)
    elif kind=='apps':
        for x,y,c in [(62,62,a),(137,62,f),(62,137,f),(137,137,a)]:v.rect((x,y,x+57,y+57),c,15)
    elif kind in ('folder','portfolio','files'):
        v.rect((56,71,124,111),a,13);v.rect((56,93,200,187),a,17)
        if kind=='portfolio':L([(101,140),(120,157),(157,120)],bg,10)
        if kind=='files':L([(82,128),(170,128)],bg,8);L([(82,152),(140,152)],bg,8)
    elif kind=='downloads':
        L([(128,61),(128,151)],a,14);L([(98,126),(128,156),(158,126)],a,12);L([(66,157),(66,190),(190,190),(190,157)],f,10)
    elif kind=='desktop':
        v.rect((55,64,201,167),a,16);v.rect((66,75,190,155),bg,8);L([(128,171),(128,192)],f,11);L([(99,194),(157,194)],f,10)
    elif kind=='recycle':
        L([(87,99),(95,188),(162,188),(170,99)],f,10);L([(77,82),(180,82)],a,12);L([(109,66),(148,66)],a,9);L([(116,113),(116,163)],a,7);L([(143,113),(143,163)],a,7)
    elif kind=='search':
        v.arc(112,112,47,0,360,a,12);L([(148,148),(193,193)],f,15)
    elif kind=='back':L([(164,66),(99,128),(164,190)],a,17)
    elif kind=='more':
        for x in [76,128,180]:C(x,128,12)
    elif kind=='web':
        v.arc(128,128,68,0,360,a,10);v.arc(128,128,36,0,360,f,8);L([(62,128),(194,128)],f,8);L([(128,62),(128,194)],a,8)
    elif kind in ('db','table'):
        v.rect((64,65,192,192),a,19)
        for y in (106,148):L([(75,y),(181,y)],bg,8)
        if kind=='table':L([(107,76),(107,180)],bg,8)
    elif kind=='java':
        L([(70,109),(77,170),(155,170),(166,109),(70,109)],a,10);L([(170,119),(191,119),(190,148),(168,148)],f,8)
        L([(104,86),(119,67),(110,49)],f,8);L([(137,85),(151,65)],a,8)
    else:
        raise ValueError(kind)
    return v


# project, symbol, accent, meaning; names used by RadialMenu stay stable.
PROJECTS = {
 'RadialMenu':('radial','#FF637A','Два уровня кольца, выделенный сектор и курсор'),
 'AutoTranslator':('translate','#67DCE8','Две перекрывающиеся карточки разных языков'),
 'ContestReminder':('contest','#FFC66E','Кубок и часы: соревнование с дедлайном'),
 'cpp-docs-panel':('book','#75B5FF','Раскрытый справочник; ясный силуэт вместо миниатюры сцены'),
 'moon-core':('core','#B8A1FF','Ядро с шестью подключениями — модули редактора'),
 'moon-bundle':('bundle','#B8A1FF','Коробка с лентой — установка набора приложений'),
 'NetworkDoctor':('network','#6DE0B4','Сигнал сети и линия диагностики'),
 'Boost':('boost','#C9ED81','Молния — освобождение ресурсов перед игрой'),
 'VSCodeLauncher':('launcher','#75B5FF','Терминал с кнопкой запуска'),
 'vscode-bg':('background','#E9A1D6','Слои оформления и пейзаж'),
 'TestMenuMoon':('game','#B8A1FF','Контроллер и дополнительная панель управления'),
 'training-site':('training','#FFC66E','Лестница прогресса и движение вперёд'),
 'unreal-docs':('book','#A5B9D6','Справочник без имитации стороннего товарного знака'),
 'Les':('forest','#FF6B4A','Фестиваль «Волна»: три линии Волги и закатное солнце'),
 'desktop-backup':('backup','#6DE0B4','Цикл восстановления и история'),
 'github-profile':('moon','#B8A1FF','Полумесяц и звёзды как подпись семейства'),
 'image':('image','#FFC66E','Рамка, горы и солнце — библиотека изображений'),
}
ICONS={'app':'radial','projects':'projects','code':'code','study':'study','career':'career','music':'music','apps':'apps','portfolio':'portfolio','contests':'contest','folder':'folder','cpp':'cpp','java':'java','web':'web','game':'game','db':'db','table':'table','files':'files','downloads':'downloads','desktop':'desktop','recycle':'recycle','search':'search','back':'back','terminal':'terminal','editor':'code','more':'more'}
THEMES={'crimson':('#FF637A','#171D2D','#EFF4FF'),'aurora':('#69E2C5','#12252B','#E9FCF8'),'dusk':('#C3A1FF','#211B34','#F3EDFF'),'sand':('#956022','#F5EFE3','#263345')}


def install(src,dst):
    dst=WORK/dst
    if dst.exists() and src.read_bytes()==dst.read_bytes():return
    if dst.exists():
        backup=BACKUP/dst.relative_to(WORK)
        if not backup.exists():
            backup.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(dst,backup)
    dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dst)


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--check',action='store_true');args=parser.parse_args()
    if args.check:
        manifest=json.loads((OUT/'manifest.json').read_text(encoding='utf-8'))
        import xml.etree.ElementTree as ET
        for name in manifest['assets']:
            p=ROOT/name
            if p.suffix=='.svg':ET.parse(p)
            else:
                with Image.open(p) as im:
                    im.load();assert im.size==(256,256) and im.mode=='RGBA',p
                    assert im.getpixel((0,0))[3]==0,p
        print(f"Verified {len(manifest['assets'])} assets: SVG XML, PNG size, alpha and decoding")
        return
    OUT.mkdir(parents=True,exist_ok=True)
    assets=[]
    for project,(kind,accent,meaning) in PROJECTS.items():
        p=OUT/'logos'/project
        draw(kind,accent).save(p)
        for ext in ('.svg','.png'):assets.append(str(p.with_suffix(ext).relative_to(ROOT)))
        # Library assets are managed by image/_tools, never mutated here.
        if project!='image':
            install(p.with_suffix('.png'),f'{project}/icon.png')
            install(p.with_suffix('.svg'),f'{project}/branding/logo.svg')
        name='proj-'+project.lower()
        if project=='image':name='proj-image'
        install(p.with_suffix('.png'),f'RadialMenu/icons/{name}.png')
        im=Image.open(p.with_suffix('.png'))
        im.save(p.with_suffix('.ico'),sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])
    targets={
      'AutoTranslator':['autotranslator/assets/icon.png','autotranslator/assets/app.ico'],
      'ContestReminder':['contestreminder/assets/bell.png','contestreminder/assets/app.ico'],
      'VSCodeLauncher':['assets/logo.png','assets/app.ico'],
      'cpp-docs-panel':['extension/icon-marketplace.png','extension/icon.svg','docs/screenshots/logo.png'],
      'moon-core':['extension/media/icon.svg'],
      'vscode-bg':['extension/icon.png','docs/screenshots/logo.webp'],
      'RadialMenu':['icons/app.png','src/res/app.ico'],
      'training-site':['reader/src/res/app.ico'],
    }
    for project,paths in targets.items():
        for dest in paths:
            src=(OUT/'logos'/project).with_suffix(Path(dest).suffix)
            if src.suffix=='.webp':Image.open(src.with_suffix('.png')).save(src,lossless=True)
            install(src,f'{project}/{dest}')
    for theme,(accent,bg,fg) in THEMES.items():
        for name,kind in ICONS.items():
            p=ROOT/'icons'/'themes'/theme/name
            draw(kind,accent,bg,fg).save(p)
            assets.extend(str(p.with_suffix(e).relative_to(ROOT)) for e in ('.svg','.png'))
            if theme=='crimson':install(p.with_suffix('.png'),f'RadialMenu/icons/{name}.png')
    (OUT/'manifest.json').write_text(json.dumps({'assets':assets,'projects':PROJECTS,'themes':THEMES},ensure_ascii=False,indent=2),encoding='utf-8')
    preview()
    print(f'Generated {len(PROJECTS)} identities and {len(ICONS)} icons in {len(THEMES)} themes; originals: {BACKUP}')


def preview():
    fontpath='C:/Windows/Fonts/segoeui.ttf'
    font=ImageFont.truetype(fontpath,18);small=ImageFont.truetype(fontpath,14)
    sheet=Image.new('RGB',(1200,1030),'#101522');d=ImageDraw.Draw(sheet)
    d.text((35,22),'MOON / PROJECT IDENTITIES',font=ImageFont.truetype(fontpath,28),fill='#EFF4FF')
    for i,project in enumerate(PROJECTS):
        x=30+i%6*195;y=80+i//6*188
        im=Image.open(OUT/'logos'/f'{project}.png').resize((128,128),Image.Resampling.LANCZOS)
        sheet.paste(im,(x+25,y),im);d.text((x+8,y+136),project,font=small,fill='#D5DDED')
    for j,theme in enumerate(THEMES):
        y=666+j*85;d.text((35,y+23),theme.upper(),font=font,fill='#D5DDED')
        for i,name in enumerate(['projects','code','study','music','apps','folder','downloads','game','search','recycle','web']):
            im=Image.open(ROOT/'icons/themes'/theme/f'{name}.png').resize((67,67),Image.Resampling.LANCZOS)
            sheet.paste(im,(190+i*89,y),im)
    sheet.save(OUT/'preview.png')
    cards=[]
    for project,(_,_,meaning) in PROJECTS.items():
        cards.append(f'<article><img src="logos/{project}.svg" alt=""><h2>{html.escape(project)}</h2><p>{meaning}</p><div class="sizes">'+''.join(f'<img width="{s}" height="{s}" src="logos/{project}.svg" alt="{s}px">' for s in [16,24,32,48])+'</div></article>')
    sections=[]
    for theme in THEMES:
        sections.append(f'<h2>{theme}</h2><section class="icons">'+''.join(f'<figure><img src="../../icons/themes/{theme}/{name}.svg" alt=""><figcaption>{name}</figcaption></figure>' for name in ICONS)+'</section>')
    (OUT/'index.html').write_text('<!doctype html><html lang="ru"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Moon — обновление графики</title><style>body{margin:40px;background:#101522;color:#eaf0ff;font:16px Segoe UI,sans-serif}h1{font-size:40px}p{color:#aebbd1;line-height:1.6}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:20px}article{padding:24px;border:1px solid #303a50;border-radius:24px}article>img{width:120px}h2{font-size:20px}.sizes{display:flex;align-items:center;gap:15px}.icons{display:flex;flex-wrap:wrap;gap:12px}figure{margin:0;width:95px;text-align:center}figure img{width:80px}figcaption{font-size:12px;color:#aebbd1}</style><h1>Moon / новые знаки проектов</h1><p>Одна геометрическая система. Разные смыслы. SVG + PNG + ICO. Ниже проверка в размерах 16, 24, 32 и 48 px.</p><main class="grid">'+''.join(cards)+'</main>'+''.join(sections)+'</html>',encoding='utf-8')


if __name__=='__main__':main()
