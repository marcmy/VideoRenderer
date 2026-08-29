from pathlib import Path
import numpy as np, cv2, math, sys, json
sys.path.insert(0,'/mnt/data/vr_work')
from robust_independent_test import load_rgb,load_flow,consistency_fields,smoothstep
from splat_inverse_prototype import remap,scene_stats
from splat_qrisk_sweep import q_map
from splat_quality_weight_sweep import splat_q,directional_q

def metric(O,G):
 d=np.mean(np.abs(O-G),2); return {'c1':float(100*np.mean(d>1/255)),'c4':float(100*np.mean(d>4/255)),'c8':float(100*np.mean(d>8/255)),'mad':float(d.mean()),'p99':float(np.quantile(d,.99))}

def compare_alpha(a,ref):
 e=np.abs(a-ref); return {'alphaMAD':float(e.mean()),'alphaP99':float(np.quantile(e,.99)),'alphaMax':float(e.max()),'gt01':float(100*np.mean(e>.01)),'gt05':float(100*np.mean(e>.05))}

def one(ds):
 d=Path(ds); A=load_rgb(d,'frame-A.bmp');B=load_rgb(d,'frame-B.bmp');G=load_rgb(d,'midpoint-current.bmp');T=.5*(A+B);f=load_flow(d,'flow-forward-B-to-A-s10.5.bin');b=load_flow(d,'flow-backward-A-to-B-s10.5.bin');fe,be=consistency_fields(f,b);q,occ,_=q_map(A,B,f,b);qf,qb=directional_q(A,B,f,b);_,_,cut=scene_stats(A,B)
 H,W=A.shape[:2];yy,xx=np.mgrid[0:H,0:W].astype(np.float32);rad=1;sig=.75;ks=sum(math.exp(-(x*x+y*y)/(2*sig*sig)) for y in range(-rad,rad+1) for x in range(-rad,rad+1));da,wa=splat_q(b,be,qb,be<=20,sig,rad,1200);db,wb=splat_q(f,fe,qf,fe<=20,sig,rad,1200);CA=cv2.resize(np.clip(wa/ks,0,1),(W,H));CB=cv2.resize(np.clip(wb/ks,0,1),(W,H));DA=cv2.resize(da,(W,H));DB=cv2.resize(db,(W,H));AW=remap(A,xx+DA[...,0],yy+DA[...,1]);BW=remap(B,xx+DB[...,0],yy+DB[...,1]);aa=np.maximum(CA,1e-8)**3;bb=np.maximum(CB,1e-8)**3;den=aa+bb;S=np.where((den>1e-7)[...,None],(AW*aa[...,None]+BW*bb[...,None])/np.maximum(den[...,None],1e-7),T);M=np.median(np.stack([G,S,T],0),0)
 Q=cv2.resize(q,(W,H));support=np.maximum(CA,CB);base=smoothstep(1200,2400,Q)*smoothstep(.03,.22,support);fg=0. if cut else float(smoothstep(15,25,np.array(occ)));ref=np.minimum(cv2.GaussianBlur(base.astype(np.float32),(0,0),.8),.6)*fg
 noblur=np.minimum(base,.6)*fg;p=np.pad(base,((1,1),(1,1)),mode='edge');cross=np.minimum(.5*p[1:-1,1:-1]+.125*(p[1:-1,:-2]+p[1:-1,2:]+p[:-2,1:-1]+p[2:,1:-1]),.6)*fg;g9=np.minimum(cv2.sepFilter2D(base.astype(np.float32),-1,np.array([.25,.5,.25],np.float32),np.array([.25,.5,.25],np.float32),borderType=cv2.BORDER_REPLICATE),.6)*fg
 variants={'GAUSS08':ref,'NOBLUR':noblur,'CROSS1':cross,'G9':g9};out={'name':d.name,'field':occ,'fg':fg,'variants':{}};Oref=G*(1-ref[...,None])+M*ref[...,None]
 for k,a in variants.items():
  O=G*(1-a[...,None])+M*a[...,None];row={**metric(O,G),'alpha':float(a.mean())}
  if k!='GAUSS08':
   row.update(compare_alpha(a,ref));diff=np.mean(np.abs(O-Oref),2);row.update({'vsRefMAD':float(diff.mean()),'vsRefC1':float(100*np.mean(diff>1/255)),'vsRefC4':float(100*np.mean(diff>4/255))})
  out['variants'][k]=row
 print(json.dumps(out),flush=True);return out
if __name__=='__main__':
 for x in sys.argv[1:]: one(x)
