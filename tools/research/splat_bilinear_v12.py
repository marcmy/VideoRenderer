from pathlib import Path
import numpy as np, cv2, math, sys, json
from numba import njit
sys.path.insert(0,'/mnt/data/vr_work')
from robust_independent_test import load_rgb,load_flow,consistency_fields,smoothstep
from splat_inverse_prototype import remap,scene_stats
from splat_qrisk_sweep import q_map
from splat_quality_weight_sweep import directional_q, splat_q

@njit
def splat_bilinear(flow,err,qual,valid):
 h,w,_=flow.shape; sx=np.zeros((h,w),np.float32); sy=np.zeros((h,w),np.float32); ws=np.zeros((h,w),np.float32)
 for y in range(h):
  for x in range(w):
   if not valid[y,x]: continue
   fx=flow[y,x,0]; fy=flow[y,x,1]; tx=x+.5*fx/4.; ty=y+.5*fy/4.
   x0=int(math.floor(tx)); y0=int(math.floor(ty)); ax=tx-x0; ay=ty-y0
   conf=math.exp(-min(err[y,x],40.)/8.)*math.exp(-min(qual[y,x],8000.)/1200.)
   dx=-.5*fx; dy=-.5*fy
   for oy in range(2):
    yy=y0+oy
    if yy<0 or yy>=h: continue
    wy=(1-ay) if oy==0 else ay
    for ox in range(2):
     xx=x0+ox
     if xx<0 or xx>=w: continue
     wx=(1-ax) if ox==0 else ax; k=wx*wy*conf
     sx[yy,xx]+=dx*k; sy[yy,xx]+=dy*k; ws[yy,xx]+=k
 out=np.zeros((h,w,2),np.float32)
 for yy in range(h):
  for xx in range(w):
   if ws[yy,xx]>1e-7:
    out[yy,xx,0]=sx[yy,xx]/ws[yy,xx]; out[yy,xx,1]=sy[yy,xx]/ws[yy,xx]
 return out,ws

def metric(O,G):
 d=np.mean(np.abs(O-G),2);return {'c1':float(100*np.mean(d>1/255)),'c4':float(100*np.mean(d>4/255)),'c8':float(100*np.mean(d>8/255)),'mad':float(d.mean())}
def diff(O,F):
 d=np.mean(np.abs(O-F),2);return {'meanLSB':float(d.mean()*255),'p99LSB':float(np.quantile(d,.99)*255),'gt1':float(100*np.mean(d>1/255)),'gt4':float(100*np.mean(d>4/255))}

def finish(A,B,G,q,occ,cut,da,wa,db,wb,normA=1.,normB=1.):
 H,W=A.shape[:2];T=.5*(A+B);yy,xx=np.mgrid[0:H,0:W].astype(np.float32)
 CA=cv2.resize(np.clip(wa/normA,0,1),(W,H),interpolation=cv2.INTER_LINEAR);CB=cv2.resize(np.clip(wb/normB,0,1),(W,H),interpolation=cv2.INTER_LINEAR)
 DA=cv2.resize(da,(W,H),interpolation=cv2.INTER_LINEAR);DB=cv2.resize(db,(W,H),interpolation=cv2.INTER_LINEAR)
 AW=remap(A,xx+DA[...,0],yy+DA[...,1]);BW=remap(B,xx+DB[...,0],yy+DB[...,1]);aa=np.maximum(CA,1e-8)**3;bb=np.maximum(CB,1e-8)**3;den=aa+bb
 S=np.where((den>1e-7)[...,None],(AW*aa[...,None]+BW*bb[...,None])/np.maximum(den[...,None],1e-7),T);M=np.median(np.stack([G,S,T],0),0)
 Q=cv2.resize(q,(W,H),interpolation=cv2.INTER_LINEAR);support=np.maximum(CA,CB);fg=0 if cut else float(smoothstep(15,25,np.array(occ)))
 alpha=np.minimum(smoothstep(1200,2400,Q)*smoothstep(.03,.22,support),.6)*fg
 O=G*(1-alpha[...,None])+M*alpha[...,None]
 both=np.minimum(CA,CB);agr=np.mean(np.abs(AW-BW),2);mask=both>.1
 return O,S,alpha,float(support.mean()),float(both.mean()),float(agr[mask].mean()) if np.any(mask) else 1.

def process(ds):
 d=Path(ds);A=load_rgb(d,'frame-A.bmp');B=load_rgb(d,'frame-B.bmp');G=load_rgb(d,'midpoint-current.bmp');f=load_flow(d,'flow-forward-B-to-A-s10.5.bin');b=load_flow(d,'flow-backward-A-to-B-s10.5.bin');fe,be=consistency_fields(f,b);q,occ,_=q_map(A,B,f,b);qf,qb=directional_q(A,B,f,b);_,_,cut=scene_stats(A,B)
 ks=sum(math.exp(-(x*x+y*y)/(2*.75*.75)) for y in range(-1,2) for x in range(-1,2));da,wa=splat_q(b,be,qb,be<=20,.75,1,1200);db,wb=splat_q(f,fe,qf,fe<=20,.75,1,1200);R=finish(A,B,G,q,occ,cut,da,wa,db,wb,ks,ks)
 da2,wa2=splat_bilinear(b,be,qb,be<=20);db2,wb2=splat_bilinear(f,fe,qf,fe<=20);BIL=finish(A,B,G,q,occ,cut,da2,wa2,db2,wb2,1.,1.)
 r={'name':d.name,'field':occ,'cut':bool(cut),'gauss':{**metric(R[0],G),'support':R[3],'both':R[4],'agree':R[5]},'bilinear':{**metric(BIL[0],G),'support':BIL[3],'both':BIL[4],'agree':BIL[5]},'bilVsGauss':diff(BIL[0],R[0])}
 print(json.dumps(r),flush=True)

if __name__=='__main__':
 for x in sys.argv[1:]: process(x)
