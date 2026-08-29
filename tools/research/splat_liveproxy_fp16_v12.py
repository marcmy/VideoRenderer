from pathlib import Path
import numpy as np,cv2,math,sys,json
sys.path.insert(0,'/mnt/data/vr_work')
from robust_independent_test import load_rgb,load_flow,consistency_fields,smoothstep
from splat_inverse_prototype import remap,scene_stats
from splat_qrisk_sweep import q_map
from splat_quality_weight_sweep import directional_q
from splat_bilinear_fixedmeta_v12 import splat_bilinear_fixed
from splat_grid4_sampling_v12 import up_grid4

def met(O,G):
 d=np.mean(np.abs(O-G),2);return {'c1':float(100*np.mean(d>1/255)),'c4':float(100*np.mean(d>4/255)),'c8':float(100*np.mean(d>8/255))}
def dif(O,F):
 d=np.mean(np.abs(O-F),2);return {'gt1':float(100*np.mean(d>1/255)),'gt4':float(100*np.mean(d>4/255)),'p99':float(np.quantile(d,.99)*255),'mean':float(d.mean()*255)}
def finish_maps(A,B,G,occ,cut,ma,mb):
 H,W=A.shape[:2];T=.5*(A+B);yy,xx=np.mgrid[0:H,0:W].astype(np.float32)
 A4=up_grid4(ma,H,W);B4=up_grid4(mb,H,W);DA=A4[...,:2];DB=B4[...,:2];CA=np.clip(A4[...,2],0,1);CB=np.clip(B4[...,2],0,1);Q=A4[...,3]
 AW=remap(A,xx+DA[...,0],yy+DA[...,1]);BW=remap(B,xx+DB[...,0],yy+DB[...,1]);aa=np.maximum(CA,1e-8)**3;bb=np.maximum(CB,1e-8)**3;den=aa+bb;S=np.where((den>1e-7)[...,None],(AW*aa[...,None]+BW*bb[...,None])/np.maximum(den[...,None],1e-7),T);M=np.median(np.stack([G,S,T],0),0);support=np.maximum(CA,CB);fg=0. if cut else float(smoothstep(15,25,np.array(occ)));a=np.minimum(smoothstep(1200,2400,Q)*smoothstep(.03,.22,support),.6)*fg;return G*(1-a[...,None])+M*a[...,None]
def process(ds):
 d=Path(ds);A=load_rgb(d,'frame-A.bmp');B=load_rgb(d,'frame-B.bmp');G=load_rgb(d,'midpoint-current.bmp');f=load_flow(d,'flow-forward-B-to-A-s10.5.bin');b=load_flow(d,'flow-backward-A-to-B-s10.5.bin');fe,be=consistency_fields(f,b);q,occ,_=q_map(A,B,f,b);qf,qb=directional_q(A,B,f,b);_,_,cut=scene_stats(A,B);H,W=A.shape[:2];fh,fw=f.shape[:2];yy,xx=np.mgrid[0:fh,0:fw].astype(np.float32);px=xx*4.;py=yy*4.;inf=(px+f[...,0]>=0)&(px+f[...,0]<=W-1)&(py+f[...,1]>=0)&(py+f[...,1]<=H-1);inb=(px+b[...,0]>=0)&(px+b[...,0]<=W-1)&(py+b[...,1]>=0)&(py+b[...,1]<=H-1)
 qfq=np.minimum(np.rint(np.minimum(qf,8184)/8),1023).astype(np.uint16);qbq=np.minimum(np.rint(np.minimum(qb,8184)/8),1023).astype(np.uint16);efq=np.minimum(np.ceil(np.maximum(fe,0)/2),15).astype(np.uint8);ebq=np.minimum(np.ceil(np.maximum(be,0)/2),15).astype(np.uint8)
 da,wa=splat_bilinear_fixed(b,ebq,qbq,inb);db,wb=splat_bilinear_fixed(f,efq,qfq,inf);qp=.5*((qfq.astype(np.float32)*8)*inf+(qbq.astype(np.float32)*8)*inb)
 ma=np.dstack([da, np.clip(wa.astype(np.float32)/60.,0,1), qp]).astype(np.float32);mb=np.dstack([db, np.clip(wb.astype(np.float32)/60.,0,1), np.zeros_like(qp)]).astype(np.float32)
 F32=finish_maps(A,B,G,occ,cut,ma,mb);ha=ma.astype(np.float16).astype(np.float32);hb=mb.astype(np.float16).astype(np.float32);F16=finish_maps(A,B,G,occ,cut,ha,hb)
 print(json.dumps({'name':d.name,'field':occ,'cut':bool(cut),'fp32':met(F32,G),'fp16':met(F16,G),'fp16VsFp32':dif(F16,F32), 'maxDisp':float(max(np.abs(ma[...,:2]).max(),np.abs(mb[...,:2]).max()))}),flush=True)
if __name__=='__main__':
 for x in sys.argv[1:]:process(x)
