from pathlib import Path
import numpy as np, cv2, math, json, os
from numba import njit
from PIL import Image
from robust_independent_test import load_rgb,load_flow,consistency_fields,to8,smoothstep
from splat_inverse_prototype import remap,scene_stats
from splat_qrisk_sweep import q_map

OUT=Path('/mnt/data/vr_work/splat_qweight');OUT.mkdir(exist_ok=True)

@njit
def splat_q(flow,err,qual,valid,sigma,radius,qscale):
 h,w,_=flow.shape;sx=np.zeros((h,w),np.float32);sy=np.zeros((h,w),np.float32);ws=np.zeros((h,w),np.float32);den=2*sigma*sigma
 for y in range(h):
  for x in range(w):
   if not valid[y,x]:continue
   fx=flow[y,x,0];fy=flow[y,x,1];tx=x+.5*fx/4.;ty=y+.5*fy/4.;cx=int(math.floor(tx));cy=int(math.floor(ty));
   conf=math.exp(-min(err[y,x],40.)/8.)
   if qscale>0:conf*=math.exp(-min(qual[y,x],8000.)/qscale)
   dx=-.5*fx;dy=-.5*fy
   for oy in range(-radius,radius+1):
    yy=cy+oy
    if yy<0 or yy>=h:continue
    for ox in range(-radius,radius+1):
     xx=cx+ox
     if xx<0 or xx>=w:continue
     ddx=xx-tx;ddy=yy-ty;k=math.exp(-(ddx*ddx+ddy*ddy)/den)*conf
     sx[yy,xx]+=dx*k;sy[yy,xx]+=dy*k;ws[yy,xx]+=k
 out=np.zeros((h,w,2),np.float32)
 for y in range(h):
  for x in range(w):
   if ws[y,x]>1e-7:out[y,x,0]=sx[y,x]/ws[y,x];out[y,x,1]=sy[y,x]/ws[y,x]
 return out,ws

def directional_q(A,B,f,b):
 H,W=A.shape[:2];fh,fw=f.shape[:2];yy,xx=np.mgrid[0:fh,0:fw].astype(np.float32);px=xx*4.;py=yy*4.;w=np.array([.2126,.7152,.0722],np.float32)
 def smp(im,x,y):return cv2.remap(im,x.astype(np.float32),y.astype(np.float32),cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
 B0=smp(B,px,py);Aw=smp(A,px+f[...,0],py+f[...,1]);A0=smp(A,px,py);Bw=smp(B,px+b[...,0],py+b[...,1])
 yB=B0@w;yAw=Aw@w;yA=A0@w;yBw=Bw@w
 inf=(px+f[...,0]>=0)&(px+f[...,0]<=W-1)&(py+f[...,1]>=0)&(py+f[...,1]<=H-1);inb=(px+b[...,0]>=0)&(px+b[...,0]<=W-1)&(py+b[...,1]>=0)&(py+b[...,1]<=H-1)
 qf=16*255*np.abs(yB-yAw)/np.maximum(yB,1/255);qb=16*255*np.abs(yA-yBw)/np.maximum(yA,1/255)
 qf=np.where(inf,qf,8000).astype(np.float32);qb=np.where(inb,qb,8000).astype(np.float32)
 return qf,qb

def metric(O,G):
 d=np.mean(np.abs(O-G),2);return {'mad':float(d.mean()),'chg1':float(100*np.mean(d>1/255)),'chg4':float(100*np.mean(d>4/255)),'chg8':float(100*np.mean(d>8/255)),'p99':float(np.quantile(d,.99))}

def process(ds):
 d=Path(ds);n=d.name;A=load_rgb(d,'frame-A.bmp');B=load_rgb(d,'frame-B.bmp');G=load_rgb(d,'midpoint-current.bmp');T=.5*(A+B);f=load_flow(d,'flow-forward-B-to-A-s10.5.bin');b=load_flow(d,'flow-backward-A-to-B-s10.5.bin');fe,be=consistency_fields(f,b);q,occ,_=q_map(A,B,f,b);qf,qb=directional_q(A,B,f,b);corr,mad,cut=scene_stats(A,B);active=occ>=20 and not cut;H,W=A.shape[:2];yy,xx=np.mgrid[0:H,0:W].astype(np.float32)
 res={'name':n,'field':occ,'cut':bool(cut),'active':bool(active),'variants':{}}
 ksum=sum(math.exp(-(x*x+y*y)/(2*1.25*1.25)) for y in range(-3,4) for x in range(-3,4))
 for qs in [0,1200,2000,3200,5000]:
  da,wa=splat_q(b,be,qb,be<=20,1.25,3,qs);db,wb=splat_q(f,fe,qf,fe<=20,1.25,3,qs)
  CA=cv2.resize(np.clip(wa/ksum,0,1),(W,H),interpolation=cv2.INTER_LINEAR);CB=cv2.resize(np.clip(wb/ksum,0,1),(W,H),interpolation=cv2.INTER_LINEAR)
  DA=cv2.resize(da,(W,H),interpolation=cv2.INTER_LINEAR);DB=cv2.resize(db,(W,H),interpolation=cv2.INTER_LINEAR)
  AW=remap(A,xx+DA[...,0],yy+DA[...,1]);BW=remap(B,xx+DB[...,0],yy+DB[...,1]);den=CA+CB;S=np.where((den>1e-6)[...,None],(AW*CA[...,None]+BW*CB[...,None])/np.maximum(den[...,None],1e-6),T);M=np.median(np.stack([G,S,T],0),axis=0).astype(np.float32)
  Q=cv2.resize(q,(W,H),interpolation=cv2.INTER_LINEAR);support=np.maximum(CA,CB);risk=smoothstep(1000,2200,Q)*smoothstep(.03,.22,support);risk=cv2.GaussianBlur(risk.astype(np.float32),(0,0),.8);risk*=1 if active else 0;a=np.minimum(risk,.6);O=G*(1-a[...,None])+M*a[...,None]
  both=np.minimum(CA,CB);agree=np.mean(np.abs(AW-BW),2);mask=both>.1
  key='BASE' if qs==0 else f'QS{qs}';res['variants'][key]={'out':metric(O,G),'full':metric(M,G),'covAny':float(support.mean()),'covBoth':float(both.mean()),'warpAgree':float(agree[mask].mean()) if np.any(mask) else 1.0,'alphaMean':float(a.mean()),'alphaGt05':float(100*np.mean(a>.05))}
  Image.fromarray(to8(O)).save(OUT/(n+'_'+key+'.png'))
 Image.fromarray(to8(G)).save(OUT/(n+'_GOLD.png'))
 print(json.dumps(res),flush=True);return res

if __name__=='__main__':
 import sys
 print(json.dumps([process(c) for c in sys.argv[1:]],indent=2))
