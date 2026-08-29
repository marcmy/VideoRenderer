from pathlib import Path
import numpy as np, cv2, math, json, sys
from robust_independent_test import load_rgb,load_flow,consistency_fields,smoothstep
from splat_inverse_prototype import remap,scene_stats
from splat_qrisk_sweep import q_map
from splat_quality_weight_sweep import splat_q,directional_q

def metric(O,G):
 d=np.mean(np.abs(O-G),2);return {'mad':float(d.mean()),'c1':float(100*np.mean(d>1/255)),'c4':float(100*np.mean(d>4/255)),'c8':float(100*np.mean(d>8/255)),'p99':float(np.quantile(d,.99))}

def process(ds):
 d=Path(ds);n=d.name;A=load_rgb(d,'frame-A.bmp');B=load_rgb(d,'frame-B.bmp');G=load_rgb(d,'midpoint-current.bmp');T=.5*(A+B);f=load_flow(d,'flow-forward-B-to-A-s10.5.bin');b=load_flow(d,'flow-backward-A-to-B-s10.5.bin');fe,be=consistency_fields(f,b);q,occ,_=q_map(A,B,f,b);qf,qb=directional_q(A,B,f,b);corr,mad,cut=scene_stats(A,B);active=occ>=20 and not cut;H,W=A.shape[:2];yy,xx=np.mgrid[0:H,0:W].astype(np.float32);sig=1.25;rad=3;ks=sum(math.exp(-(x*x+y*y)/(2*sig*sig)) for y in range(-rad,rad+1) for x in range(-rad,rad+1));Q=cv2.resize(q,(W,H),interpolation=cv2.INTER_LINEAR)
 out={'name':n,'field':occ,'active':active,'rows':[]}
 for qs in [600,800,1000,1200,1400,1600,1800,2200,2800]:
  da,wa=splat_q(b,be,qb,be<=20,sig,rad,qs);db,wb=splat_q(f,fe,qf,fe<=20,sig,rad,qs);CA=cv2.resize(np.clip(wa/ks,0,1),(W,H),interpolation=cv2.INTER_LINEAR);CB=cv2.resize(np.clip(wb/ks,0,1),(W,H),interpolation=cv2.INTER_LINEAR);DA=cv2.resize(da,(W,H),interpolation=cv2.INTER_LINEAR);DB=cv2.resize(db,(W,H),interpolation=cv2.INTER_LINEAR);AW=remap(A,xx+DA[...,0],yy+DA[...,1]);BW=remap(B,xx+DB[...,0],yy+DB[...,1]);
  aa=np.maximum(CA,1e-8)**3;bb=np.maximum(CB,1e-8)**3;den=aa+bb;S=np.where((den>1e-7)[...,None],(AW*aa[...,None]+BW*bb[...,None])/np.maximum(den[...,None],1e-7),T);M=np.median(np.stack([G,S,T],0),0).astype(np.float32);support=np.maximum(CA,CB);risk=smoothstep(1000,2200,Q)*smoothstep(.03,.22,support);risk=cv2.GaussianBlur(risk.astype(np.float32),(0,0),.8);risk*=1 if active else 0;alpha=np.minimum(risk,.6);O=G*(1-alpha[...,None])+M*alpha[...,None];both=np.minimum(CA,CB);agree=np.mean(np.abs(AW-BW),2);m=both>.1
  out['rows'].append({'qs':qs,**metric(O,G),'support':float(support.mean()),'both':float(both.mean()),'agree':float(agree[m].mean()) if np.any(m) else 1.,'alpha':float(alpha.mean()),'strength':float(metric(O,G)['c4']/max(metric(O,G)['c1'],1e-6))})
 print(json.dumps(out),flush=True);return out
if __name__=='__main__':
 print(json.dumps([process(x) for x in sys.argv[1:]],indent=2))
