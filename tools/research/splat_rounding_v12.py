from pathlib import Path
import numpy as np, cv2, math, sys, json
from numba import njit
sys.path.insert(0,'/mnt/data/vr_work')
from robust_independent_test import load_rgb,load_flow,consistency_fields,smoothstep
from splat_inverse_prototype import remap,scene_stats
from splat_qrisk_sweep import q_map
from splat_quality_weight_sweep import directional_q, splat_q

@njit
def round_even(v):
 q=math.floor(v)
 f=v-q
 if f < 0.5: return int(q)
 if f > 0.5: return int(q+1)
 return int(q if (int(q)&1)==0 else q+1)

@njit
def splat_direct(flow,err,qual,valid,wscale,even_round):
 h,w,_=flow.shape;sx=np.zeros((h,w),np.int64);sy=np.zeros((h,w),np.int64);ws=np.zeros((h,w),np.int64);sig=.75;den=2*sig*sig
 for y in range(h):
  for x in range(w):
   if not valid[y,x]: continue
   fx=flow[y,x,0];fy=flow[y,x,1];tx=x+.5*fx/4.;ty=y+.5*fy/4.;cx=int(math.floor(tx));cy=int(math.floor(ty));conf=math.exp(-min(err[y,x],40.)/8.)*math.exp(-min(qual[y,x],8000.)/1200.);dx=-.5*fx;dy=-.5*fy
   for oy in range(-1,2):
    yy=cy+oy
    if yy<0 or yy>=h:continue
    for ox in range(-1,2):
     xx=cx+ox
     if xx<0 or xx>=w:continue
     ddx=xx-tx;ddy=yy-ty;k=math.exp(-(ddx*ddx+ddy*ddy)/den)*conf;wq=round_even(k*wscale) if even_round else int(math.floor(k*wscale+.5))
     if wq<=0:continue
     vx=dx*wq;vy=dy*wq;cxq=round_even(vx) if even_round else int(math.floor(vx+.5));cyq=round_even(vy) if even_round else int(math.floor(vy+.5));sx[yy,xx]+=cxq;sy[yy,xx]+=cyq;ws[yy,xx]+=wq
 out=np.zeros((h,w,2),np.float32)
 for y in range(h):
  for x in range(w):
   if ws[y,x]>0:out[y,x,0]=sx[y,x]/ws[y,x];out[y,x,1]=sy[y,x]/ws[y,x]
 return out,ws

def mass(ws):
 s=0
 for y in range(-1,2):
  for x in range(-1,2):s+=int(math.floor(math.exp(-(x*x+y*y)/(2*.75*.75))*ws+.5))
 return s

def met(O,G):
 d=np.mean(np.abs(O-G),2);return {'c1':float(100*np.mean(d>1/255)),'c4':float(100*np.mean(d>4/255)),'c8':float(100*np.mean(d>8/255)),'mad':float(d.mean())}
def diff(O,F):
 d=np.mean(np.abs(O-F),2);return {'lsbMean':float(d.mean()*255),'c025':float(100*np.mean(d>.25/255)),'c05':float(100*np.mean(d>.5/255)),'c1':float(100*np.mean(d>1/255)),'p99lsb':float(np.quantile(d,.99)*255)}
def process(ds):
 d=Path(ds);A=load_rgb(d,'frame-A.bmp');B=load_rgb(d,'frame-B.bmp');G=load_rgb(d,'midpoint-current.bmp');T=.5*(A+B);f=load_flow(d,'flow-forward-B-to-A-s10.5.bin');b=load_flow(d,'flow-backward-A-to-B-s10.5.bin');fe,be=consistency_fields(f,b);q,occ,_=q_map(A,B,f,b);qf,qb=directional_q(A,B,f,b);_,_,cut=scene_stats(A,B);H,W=A.shape[:2];yy,xx=np.mgrid[0:H,0:W].astype(np.float32);Q=cv2.resize(q,(W,H));fg=0. if cut else float(smoothstep(15,25,np.array(occ)))
 def finish(da,wa,db,wb,norm):
  CA=cv2.resize(np.clip(wa/norm,0,1).astype(np.float32),(W,H));CB=cv2.resize(np.clip(wb/norm,0,1).astype(np.float32),(W,H));DA=cv2.resize(da,(W,H));DB=cv2.resize(db,(W,H));AW=remap(A,xx+DA[...,0],yy+DA[...,1]);BW=remap(B,xx+DB[...,0],yy+DB[...,1]);aa=np.maximum(CA,1e-8)**3;bb=np.maximum(CB,1e-8)**3;den=aa+bb;S=np.where((den>1e-7)[...,None],(AW*aa[...,None]+BW*bb[...,None])/np.maximum(den[...,None],1e-7),T);M=np.median(np.stack([G,S,T],0),0);support=np.maximum(CA,CB);alpha=np.minimum(smoothstep(1200,2400,Q)*smoothstep(.03,.22,support),.6)*fg;return G*(1-alpha[...,None])+M*alpha[...,None],S,alpha
 ks=sum(math.exp(-(x*x+y*y)/(2*.75*.75)) for y in range(-1,2) for x in range(-1,2));da,wa=splat_q(b,be,qb,be<=20,.75,1,1200);db,wb=splat_q(f,fe,qf,fe<=20,.75,1,1200);F,Sf,af=finish(da,wa,db,wb,ks)
 r={'name':d.name,'field':occ,'float':met(F,G),'schemes':{}}
 outs={}
 for label,ev in [('plus',False),('even',True)]:
  da,wa=splat_direct(b,be,qb,be<=20,60,ev);db,wb=splat_direct(f,fe,qf,fe<=20,60,ev);O,S,a=finish(da,wa,db,wb,mass(60));outs[label]=(O,S,a,wa,wb);r['schemes'][label]={'mass':mass(60),'out':met(O,G),'vsFloat':diff(O,F),'splat':diff(S,Sf),'alphaMAD':float(np.mean(np.abs(a-af))),'maxWeightSum':int(max(wa.max(),wb.max()))}
 O0=outs['plus'][0];O1=outs['even'][0];r['evenVsPlus']=diff(O1,O0)
 print(json.dumps(r),flush=True);return r
if __name__=='__main__':
 for x in sys.argv[1:]:process(x)
