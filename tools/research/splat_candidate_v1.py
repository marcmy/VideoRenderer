from pathlib import Path
import numpy as np,cv2,math,json,sys
from PIL import Image
from robust_independent_test import load_rgb,load_flow,consistency_fields,to8,smoothstep
from splat_inverse_prototype import remap,scene_stats
from splat_qrisk_sweep import q_map
from splat_quality_weight_sweep import splat_q,directional_q

OUT=Path('/mnt/data/vr_work/splat_candidate_v1');OUT.mkdir(exist_ok=True)

def metric(O,G):
 d=np.mean(np.abs(O-G),2);return {'mad':float(d.mean()),'c1':float(100*np.mean(d>1/255)),'c4':float(100*np.mean(d>4/255)),'c8':float(100*np.mean(d>8/255)),'p99':float(np.quantile(d,.99))}

def process(ds,save=False):
 d=Path(ds);n=d.name;A=load_rgb(d,'frame-A.bmp');B=load_rgb(d,'frame-B.bmp');G=load_rgb(d,'midpoint-current.bmp');T=.5*(A+B);f=load_flow(d,'flow-forward-B-to-A-s10.5.bin');b=load_flow(d,'flow-backward-A-to-B-s10.5.bin');fe,be=consistency_fields(f,b);q,occ,_=q_map(A,B,f,b);qf,qb=directional_q(A,B,f,b);corr,mad,cut=scene_stats(A,B);H,W=A.shape[:2];yy,xx=np.mgrid[0:H,0:W].astype(np.float32);sig=1.25;rad=3;ks=sum(math.exp(-(x*x+y*y)/(2*sig*sig)) for y in range(-rad,rad+1) for x in range(-rad,rad+1));
 da,wa=splat_q(b,be,qb,be<=20,sig,rad,1200);db,wb=splat_q(f,fe,qf,fe<=20,sig,rad,1200);CA=cv2.resize(np.clip(wa/ks,0,1),(W,H));CB=cv2.resize(np.clip(wb/ks,0,1),(W,H));DA=cv2.resize(da,(W,H));DB=cv2.resize(db,(W,H));AW=remap(A,xx+DA[...,0],yy+DA[...,1]);BW=remap(B,xx+DB[...,0],yy+DB[...,1]);aa=np.maximum(CA,1e-8)**3;bb=np.maximum(CB,1e-8)**3;den=aa+bb;S=np.where((den>1e-7)[...,None],(AW*aa[...,None]+BW*bb[...,None])/np.maximum(den[...,None],1e-7),T);M=np.median(np.stack([G,S,T],0),0).astype(np.float32);Q=cv2.resize(q,(W,H));support=np.maximum(CA,CB);local=smoothstep(1000,2200,Q)*smoothstep(.03,.22,support);local=cv2.GaussianBlur(local.astype(np.float32),(0,0),.8);local=np.minimum(local,.6);g=0.0 if cut else float(smoothstep(15,25,np.array(occ)));alpha=local*g;O=G*(1-alpha[...,None])+M*alpha[...,None];both=np.minimum(CA,CB);agree=np.mean(np.abs(AW-BW),2);mask=both>.1
 r={'name':n,'field':occ,'global':g,'cut':bool(cut),'corr':corr,'srcMAD':mad,'alphaMean':float(alpha.mean()),'alphaGt05':float(100*np.mean(alpha>.05)),'supportMean':float(support.mean()),'bothMean':float(both.mean()),'warpAgree':float(agree[mask].mean()) if np.any(mask) else 1.0,**metric(O,G)}
 if save:
  Image.fromarray(to8(O)).save(OUT/f'{n}_OUT.png');Image.fromarray(to8(G)).save(OUT/f'{n}_GOLD.png');Image.fromarray(to8(S)).save(OUT/f'{n}_SPLAT.png')
 print(json.dumps(r),flush=True);return r
if __name__=='__main__':print(json.dumps([process(x,True) for x in sys.argv[1:]],indent=2))
