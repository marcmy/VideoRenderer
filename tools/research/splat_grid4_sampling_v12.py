from pathlib import Path
import numpy as np,cv2,math,sys,json
sys.path.insert(0,'/mnt/data/vr_work')
from robust_independent_test import load_rgb,load_flow,consistency_fields,smoothstep
from splat_inverse_prototype import remap,scene_stats
from splat_qrisk_sweep import q_map
from splat_quality_weight_sweep import directional_q,splat_q
from splat_bilinear_v12 import splat_bilinear

def up_grid4(a,H,W):
 yy,xx=np.mgrid[0:H,0:W].astype(np.float32); mx=xx/4.;my=yy/4.
 if a.ndim==2:return cv2.remap(a.astype(np.float32),mx,my,cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
 out=np.empty((H,W,a.shape[2]),np.float32)
 for c in range(a.shape[2]):out[...,c]=cv2.remap(a[...,c].astype(np.float32),mx,my,cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
 return out

def met(O,G):
 d=np.mean(np.abs(O-G),2);return {'c1':float(100*np.mean(d>1/255)),'c4':float(100*np.mean(d>4/255)),'c8':float(100*np.mean(d>8/255))}
def dif(O,F):
 d=np.mean(np.abs(O-F),2);return {'gt1':float(100*np.mean(d>1/255)),'p99':float(np.quantile(d,.99)*255),'mean':float(d.mean()*255)}
def finish(A,B,G,q,occ,cut,da,wa,db,wb,norm,mode):
 H,W=A.shape[:2];T=.5*(A+B);yy,xx=np.mgrid[0:H,0:W].astype(np.float32);up=(lambda a:cv2.resize(a,(W,H),interpolation=cv2.INTER_LINEAR)) if mode=='resize' else (lambda a:up_grid4(a,H,W));CA=up(np.clip(wa/norm,0,1));CB=up(np.clip(wb/norm,0,1));DA=up(da);DB=up(db);AW=remap(A,xx+DA[...,0],yy+DA[...,1]);BW=remap(B,xx+DB[...,0],yy+DB[...,1]);aa=np.maximum(CA,1e-8)**3;bb=np.maximum(CB,1e-8)**3;den=aa+bb;S=np.where((den>1e-7)[...,None],(AW*aa[...,None]+BW*bb[...,None])/np.maximum(den[...,None],1e-7),T);M=np.median(np.stack([G,S,T],0),0);Q=up(q);support=np.maximum(CA,CB);fg=0 if cut else float(smoothstep(15,25,np.array(occ)));a=np.minimum(smoothstep(1200,2400,Q)*smoothstep(.03,.22,support),.6)*fg;return G*(1-a[...,None])+M*a[...,None]
def one(ds):
 d=Path(ds);A=load_rgb(d,'frame-A.bmp');B=load_rgb(d,'frame-B.bmp');G=load_rgb(d,'midpoint-current.bmp');f=load_flow(d,'flow-forward-B-to-A-s10.5.bin');b=load_flow(d,'flow-backward-A-to-B-s10.5.bin');fe,be=consistency_fields(f,b);q,occ,_=q_map(A,B,f,b);qf,qb=directional_q(A,B,f,b);_,_,cut=scene_stats(A,B);ks=sum(math.exp(-(x*x+y*y)/(2*.75*.75)) for y in range(-1,2) for x in range(-1,2));da,wa=splat_q(b,be,qb,be<=20,.75,1,1200);db,wb=splat_q(f,fe,qf,fe<=20,.75,1,1200);R=finish(A,B,G,q,occ,cut,da,wa,db,wb,ks,'resize');G4=finish(A,B,G,q,occ,cut,da,wa,db,wb,ks,'grid4');da2,wa2=splat_bilinear(b,be,qb,be<=20);db2,wb2=splat_bilinear(f,fe,qf,fe<=20);BR=finish(A,B,G,q,occ,cut,da2,wa2,db2,wb2,1,'resize');BG=finish(A,B,G,q,occ,cut,da2,wa2,db2,wb2,1,'grid4');print(json.dumps({'name':d.name,'field':occ,'gauss':{'resize':met(R,G),'grid4':met(G4,G),'diff':dif(G4,R)},'bil':{'resize':met(BR,G),'grid4':met(BG,G),'diff':dif(BG,BR)}}),flush=True)
if __name__=='__main__':
 for x in sys.argv[1:]:one(x)
