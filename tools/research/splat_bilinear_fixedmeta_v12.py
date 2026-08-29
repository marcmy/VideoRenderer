from pathlib import Path
import numpy as np, cv2, math, sys, json
from numba import njit
sys.path.insert(0,'/mnt/data/vr_work')
from robust_independent_test import load_rgb,load_flow,consistency_fields,smoothstep
from splat_inverse_prototype import remap,scene_stats
from splat_qrisk_sweep import q_map
from splat_quality_weight_sweep import directional_q, splat_q
from splat_bilinear_v12 import splat_bilinear

@njit
def splat_bilinear_fixed(flow, errq, qq, inb):
    h,w,_=flow.shape
    sx=np.zeros((h,w),np.int64); sy=np.zeros((h,w),np.int64); ws=np.zeros((h,w),np.int64)
    for y in range(h):
      for x in range(w):
        if errq[y,x]>10 or not inb[y,x]: continue
        fx=flow[y,x,0]; fy=flow[y,x,1]; tx=x+.5*fx/4.; ty=y+.5*fy/4.
        x0=int(math.floor(tx)); y0=int(math.floor(ty)); ax=tx-x0; ay=ty-y0
        err=float(errq[y,x]*2); qual=float(qq[y,x]*8)
        conf=math.exp(-min(err,40.)/8.)*math.exp(-min(qual,8000.)/1200.)
        dx=-.5*fx; dy=-.5*fy
        for oy in range(2):
          yy=y0+oy
          if yy<0 or yy>=h: continue
          wy=(1-ay) if oy==0 else ay
          for ox in range(2):
            xx=x0+ox
            if xx<0 or xx>=w: continue
            wx=(1-ax) if ox==0 else ax
            wq=int(round(wx*wy*conf*60.0))
            if wq<=0: continue
            sx[yy,xx]+=int(round(dx*wq)); sy[yy,xx]+=int(round(dy*wq)); ws[yy,xx]+=wq
    out=np.zeros((h,w,2),np.float32)
    for y in range(h):
      for x in range(w):
        if ws[y,x]>0:
          out[y,x,0]=sx[y,x]/ws[y,x]; out[y,x,1]=sy[y,x]/ws[y,x]
    return out,ws

def diff(O,F):
    d=np.mean(np.abs(O-F),2)
    return {'meanLSB':float(d.mean()*255),'p99LSB':float(np.quantile(d,.99)*255),'gt025':float(100*np.mean(d>.25/255)),'gt05':float(100*np.mean(d>.5/255)),'gt1':float(100*np.mean(d>1/255)),'gt4':float(100*np.mean(d>4/255))}
def met(O,G):
    d=np.mean(np.abs(O-G),2);return {'c1':float(100*np.mean(d>1/255)),'c4':float(100*np.mean(d>4/255)),'c8':float(100*np.mean(d>8/255)),'mad':float(d.mean())}

def finish(A,B,G,q,occ,cut,da,wa,db,wb,norm):
    H,W=A.shape[:2];T=.5*(A+B);yy,xx=np.mgrid[0:H,0:W].astype(np.float32)
    CA=cv2.resize(np.clip(wa/norm,0,1).astype(np.float32),(W,H),interpolation=cv2.INTER_LINEAR);CB=cv2.resize(np.clip(wb/norm,0,1).astype(np.float32),(W,H),interpolation=cv2.INTER_LINEAR)
    DA=cv2.resize(da,(W,H),interpolation=cv2.INTER_LINEAR);DB=cv2.resize(db,(W,H),interpolation=cv2.INTER_LINEAR)
    AW=remap(A,xx+DA[...,0],yy+DA[...,1]);BW=remap(B,xx+DB[...,0],yy+DB[...,1]);aa=np.maximum(CA,1e-8)**3;bb=np.maximum(CB,1e-8)**3;den=aa+bb
    S=np.where((den>1e-7)[...,None],(AW*aa[...,None]+BW*bb[...,None])/np.maximum(den[...,None],1e-7),T);M=np.median(np.stack([G,S,T],0),0)
    Q=cv2.resize(q,(W,H),interpolation=cv2.INTER_LINEAR);support=np.maximum(CA,CB);fg=0. if cut else float(smoothstep(15,25,np.array(occ)));a=np.minimum(smoothstep(1200,2400,Q)*smoothstep(.03,.22,support),.6)*fg
    return G*(1-a[...,None])+M*a[...,None]

def process(ds):
    d=Path(ds);A=load_rgb(d,'frame-A.bmp');B=load_rgb(d,'frame-B.bmp');G=load_rgb(d,'midpoint-current.bmp');f=load_flow(d,'flow-forward-B-to-A-s10.5.bin');b=load_flow(d,'flow-backward-A-to-B-s10.5.bin');fe,be=consistency_fields(f,b);q,occ,_=q_map(A,B,f,b);qf,qb=directional_q(A,B,f,b);_,_,cut=scene_stats(A,B)
    H,W=A.shape[:2];fh,fw=f.shape[:2];yy,xx=np.mgrid[0:fh,0:fw].astype(np.float32);px=xx*4.;py=yy*4.;inf=(px+f[...,0]>=0)&(px+f[...,0]<=W-1)&(py+f[...,1]>=0)&(py+f[...,1]<=H-1);inb=(px+b[...,0]>=0)&(px+b[...,0]<=W-1)&(py+b[...,1]>=0)&(py+b[...,1]<=H-1)
    ks=sum(math.exp(-(x*x+y*y)/(2*.75*.75)) for y in range(-1,2) for x in range(-1,2));da,wa=splat_q(b,be,qb,be<=20,.75,1,1200);db,wb=splat_q(f,fe,qf,fe<=20,.75,1,1200);G3=finish(A,B,G,q,occ,cut,da,wa,db,wb,ks)
    da,wa=splat_bilinear(b,be,qb,be<=20);db,wb=splat_bilinear(f,fe,qf,fe<=20);BF=finish(A,B,G,q,occ,cut,da,wa,db,wb,1.)
    qfq=np.minimum(np.rint(np.minimum(qf,8184)/8),1023).astype(np.uint16);qbq=np.minimum(np.rint(np.minimum(qb,8184)/8),1023).astype(np.uint16);efq=np.minimum(np.ceil(np.maximum(fe,0)/2),15).astype(np.uint8);ebq=np.minimum(np.ceil(np.maximum(be,0)/2),15).astype(np.uint8)
    da,wa=splat_bilinear_fixed(b,ebq,qbq,inb);db,wb=splat_bilinear_fixed(f,efq,qfq,inf);qp=.5*((qfq.astype(np.float32)*8)*inf+(qbq.astype(np.float32)*8)*inb);BI=finish(A,B,G,qp,occ,cut,da,wa,db,wb,60.)
    r={'name':d.name,'field':occ,'cut':bool(cut),'gauss':met(G3,G),'bilFloat':met(BF,G),'bilFixed':met(BI,G),'fixedVsBilFloat':diff(BI,BF),'fixedBilVsGauss':diff(BI,G3),'maxW':int(max(wa.max(),wb.max()))}
    print(json.dumps(r),flush=True)
if __name__=='__main__':
    for x in sys.argv[1:]:process(x)
