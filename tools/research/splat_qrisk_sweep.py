from pathlib import Path
import numpy as np, cv2, math, json, os, gc
from PIL import Image, ImageDraw
from robust_independent_test import load_rgb, load_flow, consistency_fields, to8, smoothstep
from splat_inverse_prototype import splat_inverse, remap, scene_stats

OUT=Path('/mnt/data/vr_work/splat_qrisk'); OUT.mkdir(exist_ok=True)

def q_map(A,B,f,b):
    H,W=A.shape[:2]; fh,fw=f.shape[:2]
    yy,xx=np.mgrid[0:fh,0:fw].astype(np.float32); px=xx*4.;py=yy*4.
    def smp(img,x,y): return cv2.remap(img,x.astype(np.float32),y.astype(np.float32),cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
    w=np.array([.2126,.7152,.0722],np.float32)
    B0=smp(B,px,py); Aw=smp(A,px+f[...,0],py+f[...,1]); A0=smp(A,px,py); Bw=smp(B,px+b[...,0],py+b[...,1])
    yB=B0@w; yAw=Aw@w; yA=A0@w; yBw=Bw@w
    inf=(px+f[...,0]>=0)&(px+f[...,0]<=W-1)&(py+f[...,1]>=0)&(py+f[...,1]<=H-1)
    inb=(px+b[...,0]>=0)&(px+b[...,0]<=W-1)&(py+b[...,1]>=0)&(py+b[...,1]<=H-1)
    qf=16*255*np.abs(yB-yAw)/np.maximum(yB,1/255)
    qb=16*255*np.abs(yA-yBw)/np.maximum(yA,1/255)
    q=.5*(qf*inf+qb*inb)
    mx=max(1,int(fw*.04)); my=max(1,int(fh*.04)); interior=np.zeros((fh,fw),bool);interior[my:fh-my,mx:fw-mx]=1
    occ=100*np.sum(interior&(q>=1600))/interior.sum()
    return q.astype(np.float32),float(occ),interior

def splat_candidate(A,B,f,b,fe,be,sig=1.25,rad=3):
    H,W=A.shape[:2]
    da,wa=splat_inverse(b,be,be<=20,sig,rad); db,wb=splat_inverse(f,fe,fe<=20,sig,rad)
    ksum=sum(math.exp(-(x*x+y*y)/(2*sig*sig)) for y in range(-rad,rad+1) for x in range(-rad,rad+1))
    ca=np.clip(wa/ksum,0,1);cb=np.clip(wb/ksum,0,1)
    DA=cv2.resize(da,(W,H),interpolation=cv2.INTER_LINEAR);DB=cv2.resize(db,(W,H),interpolation=cv2.INTER_LINEAR)
    CA=cv2.resize(ca,(W,H),interpolation=cv2.INTER_LINEAR);CB=cv2.resize(cb,(W,H),interpolation=cv2.INTER_LINEAR)
    yy,xx=np.mgrid[0:H,0:W].astype(np.float32)
    AW=remap(A,xx+DA[...,0],yy+DA[...,1]);BW=remap(B,xx+DB[...,0],yy+DB[...,1]);den=CA+CB
    SPL=np.where((den>1e-6)[...,None],(AW*CA[...,None]+BW*CB[...,None])/np.maximum(den[...,None],1e-6),.5*(A+B))
    return SPL,CA,CB

def met(O,G,a=None):
    d=np.mean(np.abs(O-G),2);r={'mad':float(d.mean()),'chg1':float(100*np.mean(d>1/255)),'chg4':float(100*np.mean(d>4/255)),'chg8':float(100*np.mean(d>8/255)),'p99':float(np.quantile(d,.99))}
    if a is not None:r.update(alphaMean=float(a.mean()),alphaGt05=float(100*np.mean(a>.05)),alphaGt25=float(100*np.mean(a>.25)))
    return r

def process(ds,save=True):
    d=Path(ds);name=d.name;A=load_rgb(d,'frame-A.bmp');B=load_rgb(d,'frame-B.bmp');G=load_rgb(d,'midpoint-current.bmp');T=.5*(A+B);f=load_flow(d,'flow-forward-B-to-A-s10.5.bin');b=load_flow(d,'flow-backward-A-to-B-s10.5.bin');fe,be=consistency_fields(f,b)
    q,occ,interior=q_map(A,B,f,b);corr,mad,cut=scene_stats(A,B);active=occ>=20 and not cut;H,W=A.shape[:2]
    Q=cv2.resize(q,(W,H),interpolation=cv2.INTER_LINEAR);S,CA,CB=splat_candidate(A,B,f,b,fe,be);M=np.median(np.stack([G,S,T],0),axis=0).astype(np.float32);both=np.minimum(CA,CB);anyc=np.maximum(CA,CB)
    # Local image-domain matching error is the artifact confidence signal. Coverage only asks whether the alternate hypothesis exists.
    rules={
      'QLO_ANY':smoothstep(600,1600,Q)*smoothstep(.05,.30,anyc),
      'QM1_ANY':smoothstep(1000,2200,Q)*smoothstep(.05,.30,anyc),
      'QM1_BOTH':smoothstep(1000,2200,Q)*smoothstep(.05,.30,both),
      'QHI_ANY':smoothstep(1600,3200,Q)*smoothstep(.05,.30,anyc),
      'QHI_BOTH':smoothstep(1600,3200,Q)*smoothstep(.05,.30,both),
    }
    outs={};stats={'name':name,'field':occ,'corr':corr,'madAB':mad,'cut':bool(cut),'active':bool(active),'qMean':float(Q.mean()),'qGt800':float(100*np.mean(Q>800)),'qGt1600':float(100*np.mean(Q>1600)),'fullMedian':met(M,G),'variants':{}}
    for rk,r in rules.items():
        r=cv2.GaussianBlur(r.astype(np.float32),(0,0),.8)
        if not active:r*=0
        for cap in (.35,.60,1.0):
            a=np.minimum(r,cap);O=G*(1-a[...,None])+M*a[...,None];k=f'{rk}_{int(cap*100)}';outs[k]=(O,a);stats['variants'][k]=met(O,G,a)
    if save:
        for k in ['QLO_ANY_60','QM1_ANY_60','QM1_BOTH_60','QHI_ANY_60','QHI_BOTH_60','QM1_ANY_100']:
            Image.fromarray(to8(outs[k][0])).save(OUT/(name+'_'+k+'.png'))
        Image.fromarray(to8(G)).save(OUT/(name+'_GOLD.png'));Image.fromarray(to8(M)).save(OUT/(name+'_MEDFULL.png'))
        # q diagnostic
        qv=np.clip(np.log1p(Q)/np.log1p(4000),0,1);Image.fromarray(to8(np.repeat(qv[...,None],3,2))).save(OUT/(name+'_QMAP.png'))
    print(json.dumps(stats),flush=True);return stats

if __name__=='__main__':
 import sys
 rows=[process(c) for c in sys.argv[1:]]
 print(json.dumps(rows,indent=2))
