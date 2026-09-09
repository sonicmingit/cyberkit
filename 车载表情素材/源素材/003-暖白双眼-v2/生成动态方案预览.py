"""003 部件重建动画；仅写两份审批总览，不生成正式逐状态资源。"""
from pathlib import Path
import math
import hashlib
from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageChops

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / '003-暖白双眼-v2'
WHITE = (255, 246, 229)
CYAN = (91, 227, 236)
YELLOW = (255, 212, 81)
PINK = (255, 107, 148)
DIM = (155, 150, 135)
STATES = ['wake','move_off','accelerate','cruise','turn_left','turn_right','turn_end','reverse','decelerate','hard_brake','bump','wait','arrive','sleep','night','signal_lost','idle_01','idle_02','idle_03']
LABELS = ['唤醒','起步','加速','巡航','左转','右转','回正','倒车','减速','急刹','颠簸','等候/校准','停车','休眠','夜间','信号中断','待机 1','待机 2','待机 3']
NOTES = ['惺忪睁眼 · 甜甜回应','笑眼起步 · 小嘴欢呼','蓄力眯眼 · 兴奋弯弯','放松半眯 · 错拍眨眼','向左偷瞄 · 单眼微笑','向右偷瞄 · 单眼微笑','轻眨回正 · 满意笑眼','警觉探看 · 大小眼','慢慢眯眼 · 舒一口气','惊讶挤压 · 委屈缓神','错拍弹跳 · 晕乎乎','耐心眯眼 · 三点轮转','开心弯眼 · 腮红鼓起','困困合眼 · 慢慢呼吸','低亮困眼 · 月牙常亮','低垂探看 · 轻轻叹气','双眨眼 · 偷偷微笑','左看右看 · 好奇歪眼','左右单眨 · 害羞笑眼']
N = 100
DT = 50
FONT = 'C:/Windows/Fonts/msyh.ttc'

def smooth(x):
    x = max(0., min(1., x))
    return x*x*(3-2*x)

def ramp(t,a,b):
    return smooth((t-a)/(b-a))

def window(t,a,b,c,d):
    return ramp(t,a,b)*(1-ramp(t,c,d))

def pulse(t,c,w):
    return smooth(1-abs(t-c)/w)

def color(c, k):
    return tuple(round(v*max(0,min(1,k))) for v in c)

class Canvas:
    def __init__(self):
        self.im = Image.new('RGB',(360,360))
    def paint(self, mask, ink, glow=2):
        if glow:
            halo = mask.filter(ImageFilter.GaussianBlur(glow)).point(lambda v: round(v*.20))
            self.im.paste(ink,(0,0),halo)
        self.im.paste(ink,(0,0),mask)
    def poly(self,points,ink=WHITE):
        m=Image.new('L',(360,360)); ImageDraw.Draw(m).polygon(points,fill=255); self.paint(m,ink)
    def ellipse(self,box,ink=WHITE):
        m=Image.new('L',(360,360)); ImageDraw.Draw(m).ellipse(box,fill=255); self.paint(m,ink)
    def line(self,pts,ink=WHITE,width=7):
        m=Image.new('L',(360,360)); d=ImageDraw.Draw(m)
        d.line(pts,fill=255,width=width,joint='curve')
        r=width/2
        for x,y in pts: d.ellipse((x-r,y-r,x+r,y+r),fill=255)
        self.paint(m,ink)

def eye(c,x=124,y=180,rx=32,ry=66,smile=0,lid=0,closed=0,blink=0,tilt=0,ink=WHITE):
    """Morph upper/lower contours independently; no whole-image transforms."""
    top=[]; bottom=[]
    for i in range(49):
        u=-1+2*i/48
        v=math.sqrt(max(0,1-u*u))
        a=-ry*v; b=ry*v
        a=a*(1-smile)+(20-57*v)*smile
        b=b*(1-smile)+(20-29*v)*smile
        a=a*(1-lid)+(-3+tilt*u)*lid
        b=b*(1-lid)+(43*v+tilt*u)*lid
        a=a*(1-closed)+(14*v)*closed
        b=b*(1-closed)+(28*v+5)*closed
        a=a*(1-blink)-3*blink; b=b*(1-blink)+3*blink
        top.append((x+rx*u,y+a)); bottom.append((x+rx*u,y+b))
    c.poly(top+bottom[::-1],ink)

def pair(c,**kw):
    eye(c,124,**kw); eye(c,236,**kw)

def marks(c,amount=1,sides=(-1,1)):
    if amount<.015: return
    for side in sides:
        x=180+side*121
        c.line([(x,144),(x+side*13,132-7*amount)],color(YELLOW,amount),8)
        c.line([(x+side*4,166),(x+side*(17+5*amount),163)],color(YELLOW,amount),8)

def cheeks(c,amount=1):
    if amount<.015:return
    for x in (79,281): c.ellipse((x-14,226-8*amount,x+14,226+8*amount),color(PINK,amount))

def chevrons(c,direction,amount):
    if amount<.015:return
    for j in range(2):
        x=180+direction*(118+19*j+3*amount)
        c.line([(x-direction*7,164),(x+direction*5,177),(x-direction*7,190)],color(CYAN,amount),7)

def render_original(state,t):
    c=Canvas(); phase=t/5
    a=window(t,.25,.85,1.8,2.65)
    if state=='wake':
        opening=ramp(t,.15,.7)
        pair(c,ry=66+7*pulse(t,.85,.3),blink=1-opening)
        marks(c,window(t,.6,.95,1.6,2.15),(1,))
    elif state=='move_off':
        s=window(t,.2,.85,2.1,2.9)
        pair(c,rx=32+5*s,smile=s,y=180-4*s)
        cheeks(c,s)
        if s>.01:
            pts=[(164+i*1.6,216+s*12*math.sin(math.pi*i/20)) for i in range(21)]
            c.line(pts,color(WHITE,s),7)
        marks(c,window(t,.7,1.05,1.6,2.1),(1,))
    elif state=='accelerate':
        anticip=pulse(t,.36,.26)
        pair(c,rx=32+10*a,ry=66-13*anticip,smile=a,y=180+5*anticip-3*a)
        for side in (-1,1):
            for j in range(3):
                k=window(t,.55+j*.09,.85+j*.09,1.7+j*.09,2.4+j*.09)
                x=180+side*(113+4*math.sin(t*6-j)); y=163+j*17
                c.line([(x,y),(x+side*(9+14*k),y+(j-1)*8)],color(CYAN,k),6)
    elif state=='cruise':
        pair(c,blink=pulse(t,3.65,.16))
    elif state in ('turn_left','turn_right'):
        direction=-1 if state=='turn_left' else 1
        s=window(t,.3,.95,3.2,3.85)
        for side in (-1,1):
            eye(c,180+side*56+direction*10*s,rx=32+(8 if side!=direction else -2)*s,smile=s if side!=direction else 0,ry=66+3*s)
        chevrons(c,direction,s)
    elif state=='turn_end':
        s=1-ramp(t,.3,1.1)
        eye(c,124-10*s,ry=66+3*s)
        eye(c,236-10*s,rx=32+8*s,smile=s)
        chevrons(c,-1,s)
        # brief bilateral settling blink after returning to centre
        if t>1.1:
            c=Canvas(); pair(c,blink=pulse(t,1.5,.18))
    elif state=='reverse':
        s=window(t,.3,.8,3.4,4.1)
        pair(c,ry=66+5*s,rx=32-2*s,blink=.45*pulse(t,2.8,.18))
        marks(c,s*(.78+.22*math.cos(t*2*math.pi/2.4)))
    elif state=='decelerate':
        s=window(t,.3,1.15,2.25,3.2)
        pair(c,lid=s,rx=32+7*s,y=180+4*s)
    elif state=='hard_brake':
        shock=pulse(t,.64,.24); squash=pulse(t,.98,.21); recovery=pulse(t,1.31,.28)
        pair(c,rx=32+4*shock+13*squash-3*recovery,ry=66+15*shock-34*squash+8*recovery,y=180+10*squash-4*recovery)
        marks(c,window(t,.42,.6,1.03,1.55))
    elif state=='bump':
        k=window(t,.32,.56,1.5,2.2)
        for side in (-1,1):
            delay=.10 if side>0 else 0
            bounce=math.sin((t-delay-.45)*14)*math.exp(-max(0,t-.65)*1.6)*k
            x=180+side*56; y=180+15*bounce
            oval=Canvas(); eye(oval,x,y,rx=32+8*abs(bounce),ry=66-23*abs(bounce))
            squeeze=Canvas()
            squeeze.line([(x+side*22,y-24),(x-side*16,y),(x+side*22,y+24)],WHITE,10)
            c.im=ImageChops.add(c.im,Image.blend(oval.im,squeeze.im,k))
        if k>.02:
            c.line([(146+i*3.4,239+4*k*math.sin(i*.7+t*8)) for i in range(21)],color(WHITE,k),6)
            for side in (-1,1):
                c.line([(180+side*(128+(j%2)*7),205+j*10) for j in range(4)],color(WHITE,k*.8),5)
    elif state=='wait':
        pair(c,blink=pulse(t,3.8,.18))
        for j,x in enumerate((150,180,210)):
            p=(1+math.cos(2*math.pi*(phase-j/3)))/2
            c.ellipse((x-5,267-5-3*p,x+5,267+5-3*p),color(WHITE,.32+.68*p))
    elif state=='arrive':
        s=window(t,.3,1.1,3.0,3.7)
        pair(c,rx=32+7*s,smile=s,y=180+2*s)
        cheeks(c,s)
    elif state=='sleep':
        breathing=(1-math.cos(phase*2*math.pi))/2
        pair(c,closed=1,rx=37+2*breathing,y=187+3*breathing)
    elif state=='night':
        pair(c,rx=31,ry=48,ink=DIM,blink=pulse(t,3.65,.25))
        m=Image.new('L',(360,360)); d=ImageDraw.Draw(m)
        d.ellipse((252,76,296,124),fill=255); d.ellipse((267,69,304,112),fill=0)
        c.paint(m,YELLOW)
    elif state=='signal_lost':
        s=window(t,.15,.9,3.6,4.3)
        for side in (-1,1):eye(c,180+side*56,190,rx=38,lid=.8+.2*s,tilt=side*13,ink=DIM)
        strength=.8+.2*math.sin(phase*2*math.pi)**2
        for radius in (22,39):
            pts=[(255+radius*math.sin(-.8+i*1.6/24),125-radius*math.cos(-.8+i*1.6/24)) for i in range(25)]
            c.line(pts,color(PINK,strength),6)
        c.line([(248,130),(262,144)],PINK,5);c.line([(262,130),(248,144)],PINK,5)
    elif state=='idle_01':
        blink=max(pulse(t,1.8,.17),pulse(t,2.18,.14))
        pair(c,blink=blink)
    elif state=='idle_02':
        look=-15*window(t,.55,1.15,1.65,2.15)+15*window(t,2.4,3.0,3.45,4.05)
        for side in (-1,1): eye(c,180+side*56+look,ry=66-5*abs(look)/15,rx=32,blink=pulse(t,2.25,.14))
    elif state=='idle_03':
        wink=window(t,1.0,1.25,1.6,1.95)
        eye(c,124,ry=66+4*wink); eye(c,236,smile=wink,rx=32+7*wink)
    else:raise ValueError(state)
    return c.im

def mouth(c,amount=1,kind='smile',y=227):
    if amount<.02:return
    if kind=='o':
        c.ellipse((175,y-6*amount,185,y+7*amount),color(WHITE,amount))
    else:
        c.line([(165+i*1.5,y+9*amount*math.sin(math.pi*i/20)) for i in range(21)],color(WHITE,amount),5)

def render(state,t):
    """Distinct resting silhouettes and anticipation/action/after-reaction phases."""
    c=Canvas()
    a=window(t,.4,1.1,2.1,3.3)
    after=window(t,2.05,2.8,3.65,4.6)
    breathe=(1-math.cos(2*math.pi*t/5))/2
    if state=='wake':
        opened=window(t,.25,.8,1.15,1.75)
        happy=window(t,1.25,2.0,3.45,4.65)
        for side in (-1,1):
            blink=pulse(t,1.05+side*.07,.16)
            eye(c,180+side*56,ry=47+24*opened,lid=.65*(1-opened)*(1-happy),smile=happy,blink=blink)
        marks(c,window(t,.65,.95,1.5,2.1),(1,));cheeks(c,.7*happy);mouth(c,happy)
    elif state=='move_off':
        prep=pulse(t,.5,.35)
        for side in (-1,1):
            eye(c,180+side*56,y=180+6*prep-3*a,rx=37+3*a,ry=47,smile=.8+.2*a,blink=.65*prep)
        cheeks(c,.55+.35*a);mouth(c,.7+.3*a,y=218-3*a)
        marks(c,window(t,.8,1.1,1.7,2.2),(1,))
    elif state=='accelerate':
        prep=pulse(t,.5,.35)
        pair(c,rx=39+4*a,ry=46,smile=.82+.18*a,blink=.7*prep,y=180+5*prep-3*a)
        for side in (-1,1):
            for j in range(3):
                k=window(t,.65+j*.08,1+j*.08,2.0+j*.12,3.2+j*.1)
                x=180+side*(115+5*math.sin(t*5-j));y=163+j*17
                c.line([(x,y),(x+side*(10+10*k),y+(j-1)*8)],color(CYAN,k),6)
        mouth(c,.7*after)
    elif state=='cruise':
        for side in (-1,1):
            eye(c,180+side*56,ry=51,lid=.3+.12*breathe,blink=pulse(t,3.3+side*.06,.19))
    elif state in ('turn_left','turn_right'):
        direction=-1 if state=='turn_left' else 1
        lean=.55+.45*a
        for side in (-1,1):
            outer=side==direction
            eye(c,180+side*56+direction*11*lean,y=180-3*a if outer else 182,rx=30 if outer else 39,ry=56,smile=0 if outer else .9+.1*a,lid=.12 if outer else 0,blink=pulse(t,2.9,.2) if outer else 0)
        chevrons(c,direction,.5+.5*a)
        cheeks(c,.24*after)
    elif state=='turn_end':
        look=-10*window(t,.1,.5,.85,1.6)
        for side in (-1,1):
            eye(c,180+side*56+look,rx=37,ry=48,smile=.65+.35*after,blink=pulse(t,1.55+side*.08,.22))
        mouth(c,.55*after)
    elif state=='reverse':
        for side in (-1,1):
            glance=window(t,.6,1.2,1.7,2.4)-window(t,2.5,3.1,3.5,4.3)
            eye(c,180+side*56+8*glance,ry=53+side*9+side*7*glance,rx=30,lid=.18 if side<0 else 0,blink=pulse(t,3.9+side*.06,.18))
        marks(c,.45+.35*a);mouth(c,.45+.25*a,'o',236)
    elif state=='decelerate':
        pair(c,rx=39,ry=48,lid=.8+.2*a,closed=.3*after,y=182+3*a)
        mouth(c,.6*after,y=240)
    elif state=='hard_brake':
        shock=pulse(t,.75,.3); squash=pulse(t,1.12,.25); recovery=pulse(t,1.5,.3)
        for side in (-1,1):
            eye(c,180+side*56,y=184+10*squash-3*recovery,rx=36+9*squash,ry=51+27*shock-28*squash+7*recovery,lid=.8*(1-shock)*(1-squash)*(1-recovery),tilt=side*10,blink=.65*pulse(t,2.3+side*.1,.23))
        marks(c,window(t,.5,.7,1.2,1.8));mouth(c,.6*shock,'o',244)
        mouth(c,.45*after,y=243)
    elif state=='bump':
        k=window(t,.35,.65,1.65,2.6)
        for side in (-1,1):
            bounce=math.sin((t-.1*side-.45)*12)*math.exp(-max(0,t-.85)*1.4)*k
            x=180+side*56;y=181+15*bounce
            oval=Canvas();eye(oval,x,y,rx=37+5*abs(bounce),ry=48,lid=.65,tilt=side*13,blink=.7*pulse(t,3.1+side*.14,.22))
            squeeze=Canvas();squeeze.line([(x+side*22,y-22),(x-side*16,y),(x+side*22,y+22)],WHITE,10)
            c.im=ImageChops.add(c.im,Image.blend(oval.im,squeeze.im,k))
        c.line([(148+i*3.2,240+3*math.sin(i*.7+t*6*k)) for i in range(21)],WHITE,5)
        for side in (-1,1):
            if k>.01:c.line([(180+side*(128+(j%2)*6),201+j*10) for j in range(4)],color(WHITE,k*.7),5)
    elif state=='wait':
        for side in (-1,1):eye(c,180+side*56,ry=46,rx=35,lid=.48+.13*breathe,blink=pulse(t,3.4+side*.08,.2))
        for j,x in enumerate((150,180,210)):
            p=(1+math.cos(2*math.pi*(t/5-j/3)))/2
            c.ellipse((x-5,263-5-4*p,x+5,263+5-4*p),color(WHITE,.3+.7*p))
    elif state=='arrive':
        for side in (-1,1):eye(c,180+side*56,rx=39+2*a,smile=.94+.06*a,y=181-4*a,blink=.5*pulse(t,2.7+side*.06,.22))
        cheeks(c,.65+.3*a);mouth(c,.6+.3*a,y=219)
    elif state=='sleep':
        pair(c,closed=1,rx=37+2*breathe,y=187+3*breathe)
        mouth(c,.3+.25*breathe,'o',237)
    elif state=='night':
        pair(c,rx=34,ry=39,ink=DIM,lid=.7,closed=.55*breathe,blink=pulse(t,3.6,.3))
        m=Image.new('L',(360,360));d=ImageDraw.Draw(m)
        d.ellipse((252,76,296,124),fill=255);d.ellipse((267,69,304,112),fill=0);c.paint(m,YELLOW)
    elif state=='signal_lost':
        c.im=render_original(state,t)
        mouth(c,.5*after,'o',246)
    elif state=='idle_01':
        sweet=window(t,2.4,3.0,3.6,4.6)
        for side in (-1,1):
            blink=max(pulse(t,1.3+side*.035,.19),pulse(t,1.8+side*.035,.17))
            eye(c,180+side*56,ry=48,rx=35,lid=.25*(1-sweet),smile=.9*sweet,blink=blink)
        cheeks(c,.5*sweet)
    elif state=='idle_02':
        look=-window(t,.4,1.0,1.45,2.0)+window(t,2.4,3.0,3.5,4.3)
        for side in (-1,1):
            eye(c,180+side*56+15*look,y=180-side*5*look,ry=47+side*8*look,rx=32,lid=.18,blink=pulse(t,2.15,.18))
    elif state=='idle_03':
        left=window(t,.65,.9,1.15,1.55);right=window(t,1.65,1.9,2.2,2.6)
        happy=window(t,2.5,3.05,3.6,4.65)
        for side in (-1,1):
            wink=left if side<0 else right
            eye(c,180+side*56,ry=47,rx=37,smile=.72+.28*happy,blink=wink)
        cheeks(c,.25+.55*happy);mouth(c,.6*happy)
    else:raise ValueError(state)
    return c.im

def template():
    im=Image.new('RGB',(1080,1650),(12,16,20)); d=ImageDraw.Draw(im)
    title=ImageFont.truetype(FONT,36); small=ImageFont.truetype(FONT,17)
    d.text((40,25),'暖白双眼 · 动画表情方案',font=title,fill=WHITE)
    d.text((42,80),'16 种设计状态 + 3 种待机动作   /   20 帧每秒   /   视觉预览',font=small,fill=(153,164,174))
    label=ImageFont.truetype(FONT,23); note=ImageFont.truetype(FONT,14)
    for i in range(19):
        x=30+(i%4)*264; y=125+(i//4)*296
        d.text((x+120,y+240),LABELS[i],font=label,fill=WHITE,anchor='mt')
        d.text((x+120,y+273),NOTES[i],font=note,fill=(150,160,171),anchor='mt')
    x=30+3*264;y=125+4*296
    d.text((x+12,y+80),'动作说明',font=label,fill=WHITE)
    for j,s in enumerate(['各状态保留自己的情绪收尾','预备 → 动作 → 余韵 → 轻衔接','三种待机 · 不同微表情','暖白双眼 · 可爱过渡优化']):
        d.text((x+12,y+123+j*26),s,font=note,fill=(150,160,171))
    d.text((42,1621),'黑底圆屏安全区 · 暖白主体 / 青色动势 / 黄色提示 / 粉色情绪与断线',font=small,fill=(153,164,174))
    return im

def main():
    base=template(); static=base.copy(); boards=[base.copy() for _ in range(N)]
    # Static frames correspond exactly to time slots in the dynamic overview.
    representatives=[48,25,26,20,30,30,58,24,38,16,18,25,30,25,40,25,60,26,62]
    mask=Image.new('L',(360,360)); ImageDraw.Draw(mask).ellipse((15,15,345,345),fill=255)
    outside=ImageChops.invert(mask)
    for i,state in enumerate(STATES):
        frames=[render(state,k*DT/1000) for k in range(N)]
        for frame in frames:
            if ImageChops.multiply(frame.convert('L'),outside).getextrema()[1]>3:
                raise ValueError('超出扩展安全区: '+state)
        x=30+(i%4)*264;y=125+(i//4)*296
        for k,f in enumerate(frames):boards[k].paste(f.resize((240,240),Image.Resampling.LANCZOS),(x,y))
        static.paste(frames[representatives[i]].resize((240,240),Image.Resampling.LANCZOS),(x,y))
        unique=len({hashlib.sha256(f.tobytes()).digest() for f in frames})
        assert unique>1,state
        # Report timeline seam; transient restart is an overview replay, not a firmware loop.
        seam=ImageChops.difference(frames[0],frames[-1]).convert('L').getbbox()
        print(state,'unique_frames=',unique,'seam=',seam,flush=True)
    palette_source=Image.new('RGB',(1080,1650*5))
    for i,k in enumerate((0,13,25,40,70)):palette_source.paste(boards[k],(0,i*1650))
    pal=palette_source.quantize(colors=128,method=Image.Quantize.MEDIANCUT)
    quant=[b.quantize(palette=pal,dither=Image.Dither.NONE) for b in boards]
    static.save(OUT/'全部静态表情预览.png')
    quant[0].save(OUT/'全部表情预览.gif',save_all=True,append_images=quant[1:],duration=DT,loop=0,disposal=1,optimize=False)
    for name in ('全部静态表情预览.png','全部表情预览.gif'):
        p=OUT/name;print(name,p.stat().st_size,hashlib.sha256(p.read_bytes()).hexdigest())
    with Image.open(OUT/'全部表情预览.gif') as gif:
        total=0
        for i in range(gif.n_frames):gif.seek(i);total+=gif.info['duration']
        assert total==5000,total
        print('GIF verified',gif.size,gif.n_frames,total,'ms')

if __name__=='__main__':main()
