"""NOMI 部件动画预览。仅输出两份审批总览；不导出正式资源。"""
from pathlib import Path
import math, hashlib
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'005-NOMI表情-v1'
W=(250,250,247); C=(91,229,240); R=(255,99,128); Y=(255,211,99); B=(137,174,239)
STATES=['wake','move_off','accelerate','cruise','turn_left','turn_right','turn_end','reverse','decelerate','hard_brake','bump','wait','arrive','sleep','night','signal_lost','idle_01','idle_02','idle_03']
LABELS=['唤醒','起步','加速','巡航','左转','右转','回正','倒车','减速','急刹','颠簸','等候/校准','停车','休眠','夜间','信号中断','待机 1','待机 2','待机 3']
NOTES=['惺忪睁眼 · 惊喜回应','轻轻蓄力 · 开心出发','认真眯眼 · 得意前探','弯弯笑眼 · 悠闲陪伴','先偷瞄 · 左眼俏皮眨','先偷瞄 · 右眼俏皮眨','视线归中 · 轻眨回应','警觉探看 · 小嘴屏息','慢慢合眼 · 轻轻吐气','惊讶挤眼 · 委屈缓神','错拍弹跳 · 晕乎乎','半眯等候 · 柔和轮转','笑眼鼓腮 · 星光绽开','困困合眼 · 安稳呼吸','低亮慢眨 · 月牙陪伴','困惑叉眼 · 低落恢复','双眨眼 · 小嘴偷笑','左右张望 · 好奇大小眼','单眼眨眨 · 害羞弯眼']
N=120; DT=50; S=2
def ease(v):
    v=max(0,min(1,v)); return v*v*v*(v*(v*6-15)+10)
def ramp(t,a,b): return ease((t-a)/(b-a))
def win(t,a,b,c,d): return ramp(t,a,b)*(1-ramp(t,c,d))
def pulse(t,c,w): return ease(1-abs(t-c)/w)
def mix(a,b,v): return a+(b-a)*v
def ink(c,k): return tuple(round(v*max(0,min(1,k))) for v in c)

class Canvas:
    def __init__(self):
        self.im=Image.new('RGB',(360*S,360*S)); self.d=ImageDraw.Draw(self.im)
    def line(self,p,c=W,w=7):
        p=[(x*S,y*S) for x,y in p]; self.d.line(p,fill=c,width=max(1,round(w*S)),joint='curve')
        r=w*S/2
        for x,y in (p[0],p[-1]): self.d.ellipse((x-r,y-r,x+r,y+r),fill=c)
    def oval(self,x,y,rx,ry,c=W): self.d.ellipse(((x-rx)*S,(y-ry)*S,(x+rx)*S,(y+ry)*S),fill=c)
    def ring(self,x,y,rx,ry,c=W,w=6):
        self.line([(x+rx*math.cos(i*math.tau/64),y+ry*math.sin(i*math.tau/64)) for i in range(65)],c,w)
    def curve(self,x,y,rx,h,c=W,w=7):
        self.line([(x+rx*u,y+h*(1-u*u)) for u in np.linspace(-1,1,35)],c,w)
    def final(self): return self.im.resize((360,360),Image.Resampling.LANCZOS)

def eye(c,x,y,smile=0,closed=0,squeeze=0,cross=0,blink=0,scale=1,tilt=0,color=W):
    # One continuous outline morphs from a capsule to a curved smile/closed lid.
    points=[]
    for a in np.linspace(0,math.tau,81):
        ux=math.cos(a); sy=math.sin(a)
        ex=11*ux; ey=21*(1 if sy>=0 else -1)+8*sy
        ex=mix(ex,24*ux,smile); ey=mix(ey,-15*(1-ux*ux)+sy*1.2,smile)
        ex=mix(ex,25*ux,closed); ey=mix(ey,10*(1-ux*ux)+sy,closed)
        ey=ey*(1-blink)+sy*1.2*blink
        points.append((x+ex*scale,y+(ey+tilt*ux)*scale))
    points.append(points[0])
    if squeeze>0:
        p=[(x-18,y-16),(x+13,y),(x-18,y+16)] if x<180 else [(x+18,y-16),(x-13,y),(x+18,y+16)]
        # Crossfade only local strokes during the shape transition.
        if squeeze<.99: c.line(points,ink(color,1-squeeze),6)
        c.line(p,ink(color,squeeze),7)
    elif cross>0:
        if cross<.99: c.line(points,ink(color,1-cross),6)
        for sign in [-1,1]: c.line([(x-15,y-15*sign),(x+15,y+15*sign)],ink(color,cross),7)
    else: c.line(points,color,6)

def render(state,t):
    c=Canvas(); q=win(t,.55,1.45,3.8,5.55); p=pulse(t,1.05,.5)
    smile=.0; closed=0; sq=0; cross=0; gaze=0; yy=0; scale=1; tilt=0; mouth='smile'; blush=0; accent=0; color=W
    bl=[0.,0.]; sy=[0.,0.]; ex=[0.,0.]
    if state=='wake':
        closed=1-q; scale=1+.13*pulse(t,1.6,.7); mouth='o'; accent=q
    elif state=='move_off':
        smile=.65+.35*q; yy=5*p-7*pulse(t,2,.8); mouth='happy'; blush=q
    elif state=='accelerate':
        smile=.25*(1-q); closed=.45*p; yy=5*p-3*q; scale=1+.08*q; accent=q; tilt=10*q
    elif state=='cruise':
        smile=1; bl=[.75*pulse(t,2.4,.22),.75*pulse(t,2.49,.25)]; yy=1.5*math.sin(t*math.tau/6)
    elif state in ['turn_left','turn_right']:
        direction=-1 if state=='turn_left' else 1; gaze=direction*15*win(t,.3,1.2,4,5.55); accent=q
        bl[0 if direction<0 else 1]=q; tilt=direction*7*q; mouth='dot'
    elif state=='turn_end':
        gaze=-13*(1-ramp(t,.2,1.8))*(1-ramp(t,4.7,5.9))+(-13)*ramp(t,4.7,5.9)
        bl=[pulse(t,2,.3),pulse(t,2.1,.3)]; smile=.25*q
    elif state=='reverse':
        gaze=7*q*math.sin((t-1)*2); tilt=-6*q; mouth='o'; accent=q; scale=1-.12*p
    elif state=='decelerate':
        closed=.35+.65*q; yy=4*q; mouth='dot'; scale=1-.08*q
    elif state=='hard_brake':
        sq=win(t,1,1.65,2.8,3.6); scale=1+.18*pulse(t,.9,.5); mouth='o'; accent=sq
        closed=.65*win(t,3.2,4,4.6,5.8); yy=5*pulse(t,1.9,.7)
    elif state=='bump':
        sq=q; mouth='wave'; accent=q
        env=win(t,.6,1.1,3.3,4.6)
        for i in range(2): sy[i]=9*math.sin((t-1)*9-i*.8)*env; ex[i]=.12*math.sin((t-1)*9-i*.8)*env
        closed=.3*win(t,4,4.4,4.8,5.8)
    elif state=='wait': closed=.85; mouth='dots'; accent=1; bl=[.4*pulse(t,3,.4),.4*pulse(t,3.1,.4)]
    elif state=='arrive': smile=.7+.3*q; blush=q; accent=q; yy=-3*q; mouth='smile'
    elif state=='sleep': closed=1; yy=1.8*math.sin(t*math.tau/6); mouth='o'; scale=.9
    elif state=='night': color=(132,141,160); bl=[pulse(t,2.8,.7),pulse(t,2.86,.7)]; mouth='o'; scale=.85; accent=1
    elif state=='signal_lost': cross=q; mouth='wave'; accent=q; yy=3*q; closed=.4*(1-q)
    elif state=='idle_01':
        bl=[pulse(t,1.7,.22)+pulse(t,2.25,.24),pulse(t,1.76,.23)+pulse(t,2.31,.25)]; smile=.45*win(t,2.7,3.5,4.4,5.7)
    elif state=='idle_02':
        gaze=-14*win(t,.5,1.2,2,2.7)+14*win(t,2.6,3.4,4.3,5.5)
        ex=[.12*q,-.12*q]; tilt=3*q; mouth='dot'
    elif state=='idle_03':
        bl=[pulse(t,1.4,.4),pulse(t,2.25,.4)]; smile=win(t,2.7,3.5,4.4,5.7); blush=smile; mouth='happy'
    for i,x in enumerate([118,242]):
        eye(c,x+gaze,166+yy+sy[i],smile,closed,sq,cross,bl[i],scale+ex[i],tilt*(1 if i==0 else -1),color)
    my=218+yy*.45
    if mouth=='smile': c.curve(180+gaze*.4,my,15,8+3*q,color,6)
    elif mouth=='o': c.ring(180+gaze*.3,my,7+3*q,7+5*q,color,5)
    elif mouth=='dot': c.oval(180+gaze*.4,my,5,5,color)
    elif mouth=='happy':
        c.curve(180,my,13,7+8*q,color,6); c.line([(167,my),(193,my)],color,5)
    elif mouth=='wave': c.line([(159+i,my+3*q*math.sin(i*math.pi/12)) for i in range(43)],color,5)
    elif mouth=='dots':
        for i in range(3): c.oval(160+i*20,my,3.5,3.5,ink(W,.65+.35*math.sin(t*math.tau/3-i)**2))
    if blush>.01:
        for x in [92,258]:
            for off in [-5,5]: c.line([(x+off-2,211),(x+off+2,220)],ink(R,blush),5)
    if accent>.01:
        if state=='wake':
            for a in [0,.7]: c.line([(267+14*a,107+14*a),(273+18*a,94+14*a)],ink(Y,accent),6)
        elif state in ['accelerate','turn_left','turn_right']:
            sides=[-1,1] if state=='accelerate' else [-1 if state=='turn_left' else 1]
            for side in sides:
                for i in range(2):
                    x=180+side*(91+i*12+3*math.sin(t*3)); c.line([(x-side*5,214),(x+side*2,222),(x-side*5,230)],ink(C,accent),5)
        elif state in ['hard_brake','bump','reverse']:
            for side in [-1,1]:
                x=180+side*112
                c.line([(x,204),(x+side*7,214),(x-side*3,224),(x+side*4,236)],ink(Y if state=='bump' else R,accent),6)
        elif state=='arrive':
            for side in [-1,1]:
                x=180+side*108; y=131; r=10*accent
                c.line([(x-r,y),(x+r,y)],ink(Y,accent),4); c.line([(x,y-r),(x,y+r)],ink(Y,accent),4)
        elif state=='wait':
            for i in range(10):
                a=i*math.tau/10; k=.22+.78*((i/10-t/3)%1)**2
                c.oval(180+19*math.cos(a),268+19*math.sin(a),3,3,ink(C,k))
        elif state=='night': c.oval(269,107,14,14,B); c.oval(275,101,13,13,(0,0,0))
        elif state=='signal_lost':
            for r in [12,23,34]: c.line([(250+r*math.sin(a),114-r*math.cos(a)) for a in np.linspace(-.65,.65,20)],ink(R,accent),4)
            c.line([(231,82),(274,116)],ink(R,accent),4)
    return c.final()

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    font=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',23); small=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',13); title=ImageFont.truetype('C:/Windows/Fonts/msyhbd.ttc',32)
    def sheet():
        im=Image.new('RGB',(1080,1620),(12,17,23)); d=ImageDraw.Draw(im)
        d.text((32,20),'NOMI · 轻轻回应每一刻',font=title,fill=W)
        d.text((34,68),'19 个动态表情   /   20 FPS   /   柔和过渡 · 错拍眨眼 · 轻盈回弹',font=small,fill=(151,173,190))
        for k in range(19):
            x=24+(k%4)*264; y=111+(k//4)*299
            d.rounded_rectangle((x,y,x+248,y+284),radius=15,fill=(0,0,0))
            d.text((x+124,y+230),LABELS[k],anchor='mm',font=font,fill=W)
            d.text((x+124,y+262),NOTES[k],anchor='mm',font=small,fill=(150,168,183))
        return im
    base=sheet(); frames=[]; static=base.copy(); key=[2,2,2,1,2,2,2.5,2,2.5,2,2,2,2.5,2,2,2,3.5,3.5,3.8]
    yy,xx=np.mgrid[:360,:360]; unsafe=(xx-180)**2+(yy-180)**2>165**2
    signatures=[set() for _ in STATES]; seams=[]
    for k,state in enumerate(STATES):
        pos=(24+(k%4)*264+14,111+(k//4)*299+2)
        static.paste(render(state,key[k]).resize((220,220),Image.Resampling.LANCZOS),pos)
        a=np.array(render(state,0)).astype(float); b=np.array(render(state,5.95)).astype(float)
        seams.append(round(float(np.abs(a-b).mean()),4))
    for n in range(N):
        im=base.copy()
        for k,state in enumerate(STATES):
            fr=render(state,n*DT/1000); arr=np.array(fr)
            assert arr[unsafe].max()<=3,(state,n,'outside safe radius')
            signatures[k].add(hashlib.sha256(fr.tobytes()).digest())
            im.paste(fr.resize((220,220),Image.Resampling.LANCZOS),(38+(k%4)*264,113+(k//4)*299))
        frames.append(im)
    # Stable palette prevents palette flicker across cells and frames.
    palette=static.quantize(colors=128,method=Image.Quantize.MEDIANCUT)
    frames=[im.quantize(palette=palette,dither=Image.Dither.NONE) for im in frames]
    frames[0].save(OUT/'全部表情预览.gif',save_all=True,append_images=frames[1:],duration=DT,loop=0,optimize=False,disposal=1)
    static.save(OUT/'全部静态表情预览.png')
    gif=Image.open(OUT/'全部表情预览.gif'); total=0
    for i in range(gif.n_frames): gif.seek(i); total+=gif.info['duration']
    assert gif.n_frames==N and total==N*DT
    assert all(len(v)>15 for v in signatures)
    print({'frames':gif.n_frames,'duration_ms':total,'unique_per_state':[len(v) for v in signatures],'seam_mean_255':seams,'safe_radius':165,'sizes':{p.name:p.stat().st_size for p in OUT.iterdir()}})
if __name__=='__main__': main()
