from pathlib import Path
import numpy as np,cv2,math,sys,json
sys.path.insert(0,'/mnt/data/vr_work')
from robust_independent_test import load_rgb,load_flow,consistency_fields
from splat_quality_weight_sweep import directional_q

def dir_stats(flow,err,q,inb):
    errq=np.minimum(np.ceil(np.maximum(err,0)/2),15)
    qq=np.minimum(np.rint(np.minimum(q,8184)/8),1023)
    valid=(errq<=10)&inb
    tx=np.indices(err.shape)[1].astype(np.float32)+.5*flow[...,0]/4
    ty=np.indices(err.shape)[0].astype(np.float32)+.5*flow[...,1]/4
    ax=tx-np.floor(tx); ay=ty-np.floor(ty)
    conf=np.exp(-np.minimum(errq*2,40)/8)*np.exp(-np.minimum(qq*8,8000)/1200)
    counts=np.zeros(err.shape,np.uint8)
    for wy in [1-ay,ay]:
      for wx in [1-ax,ax]:
        counts += ((np.rint(wx*wy*conf*60)>0)&valid).astype(np.uint8)
    return {'validPct':float(100*valid.mean()),'targetsPerValid':float(counts[valid].mean()) if valid.any() else 0.,'targetsPerCell':float(counts.mean()),'targetP95':float(np.quantile(counts[valid],.95)) if valid.any() else 0,'targetP99':float(np.quantile(counts[valid],.99)) if valid.any() else 0,'totalTargets':int(counts.sum())}

def one(ds):
 d=Path(ds);A=load_rgb(d,'frame-A.bmp');B=load_rgb(d,'frame-B.bmp');f=load_flow(d,'flow-forward-B-to-A-s10.5.bin');b=load_flow(d,'flow-backward-A-to-B-s10.5.bin');fe,be=consistency_fields(f,b);qf,qb=directional_q(A,B,f,b);H,W=A.shape[:2];fh,fw=f.shape[:2];yy,xx=np.mgrid[0:fh,0:fw].astype(np.float32);px=xx*4;py=yy*4;inf=(px+f[...,0]>=0)&(px+f[...,0]<=W-1)&(py+f[...,1]>=0)&(py+f[...,1]<=H-1);inb=(px+b[...,0]>=0)&(px+b[...,0]<=W-1)&(py+b[...,1]>=0)&(py+b[...,1]<=H-1);sf=dir_stats(f,fe,qf,inf);sb=dir_stats(b,be,qb,inb);cells=fh*fw;targets=sf['totalTargets']+sb['totalTargets'];r={'name':d.name,'frame':[W,H],'flow':[fw,fh],'cells':cells,'forward':sf,'backward':sb,'targetsPerCellBoth':targets/cells,'atomicAddsPerCell':3*targets/cells,'totalAtomicAdds':3*targets};print(json.dumps(r))
if __name__=='__main__':
 for x in sys.argv[1:]:one(x)
