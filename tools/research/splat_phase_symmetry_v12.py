from pathlib import Path
import numpy as np, cv2, math, sys, json
from numba import njit
sys.path.insert(0,'/mnt/data/vr_work')
from robust_independent_test import load_rgb, load_flow, consistency_fields
from splat_quality_weight_sweep import directional_q
from splat_inverse_prototype import remap
from splat_grid4_sampling_v12 import up_grid4

@njit
def splat_phase_fixed(flow, errq, qq, inb, phase, is_a_side):
    h,w,_=flow.shape
    sx=np.zeros((h,w),np.int64); sy=np.zeros((h,w),np.int64); ws=np.zeros((h,w),np.int64)
    s = phase if is_a_side else (1.0-phase)
    for y in range(h):
      for x in range(w):
        if errq[y,x]>10 or not inb[y,x]: continue
        fx=flow[y,x,0]; fy=flow[y,x,1]
        tx=x+s*fx/4.; ty=y+s*fy/4.
        x0=int(math.floor(tx)); y0=int(math.floor(ty)); ax=tx-x0; ay=ty-y0
        err=float(errq[y,x]*2); qual=float(qq[y,x]*8)
        conf=math.exp(-min(err,40.)/8.)*math.exp(-min(qual,8000.)/1200.)
        dx=-s*fx; dy=-s*fy
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

def build_inputs(d):
    A=load_rgb(d,'frame-A.bmp'); B=load_rgb(d,'frame-B.bmp')
    f=load_flow(d,'flow-forward-B-to-A-s10.5.bin'); b=load_flow(d,'flow-backward-A-to-B-s10.5.bin')
    fe,be=consistency_fields(f,b); qf,qb=directional_q(A,B,f,b)
    H,W=A.shape[:2]; fh,fw=f.shape[:2]; yy,xx=np.mgrid[0:fh,0:fw].astype(np.float32); px=xx*4.;py=yy*4.
    inf=(px+f[...,0]>=0)&(px+f[...,0]<=W-1)&(py+f[...,1]>=0)&(py+f[...,1]<=H-1)
    inb=(px+b[...,0]>=0)&(px+b[...,0]<=W-1)&(py+b[...,1]>=0)&(py+b[...,1]<=H-1)
    qfq=np.minimum(np.rint(np.minimum(qf,8184)/8),1023).astype(np.uint16); qbq=np.minimum(np.rint(np.minimum(qb,8184)/8),1023).astype(np.uint16)
    efq=np.minimum(np.ceil(np.maximum(fe,0)/2),15).astype(np.uint8); ebq=np.minimum(np.ceil(np.maximum(be,0)/2),15).astype(np.uint8)
    return A,B,f,b,efq,ebq,qfq,qbq,inf,inb

def alt(A,B,ma,mb,t):
    H,W=A.shape[:2]; yy,xx=np.mgrid[0:H,0:W].astype(np.float32)
    A4=up_grid4(ma,H,W); B4=up_grid4(mb,H,W)
    CA=np.clip(A4[...,2],0,1); CB=np.clip(B4[...,2],0,1)
    AW=remap(A,xx+A4[...,0],yy+A4[...,1]); BW=remap(B,xx+B4[...,0],yy+B4[...,1])
    aa=(1.0-t)*np.maximum(CA,1e-8)**3; bb=t*np.maximum(CB,1e-8)**3; den=aa+bb
    T=(1.0-t)*A+t*B
    return np.where((den>1e-7)[...,None],(AW*aa[...,None]+BW*bb[...,None])/np.maximum(den[...,None],1e-7),T)

def maps(A,B,f,b,efq,ebq,qfq,qbq,inf,inb,t):
    da,wa=splat_phase_fixed(b,ebq,qbq,inb,t,True)
    db,wb=splat_phase_fixed(f,efq,qfq,inf,t,False)
    qp=.5*((qfq.astype(np.float32)*8)*inf+(qbq.astype(np.float32)*8)*inb)
    ma=np.dstack([da,np.clip(wa.astype(np.float32)/60,0,1),qp]).astype(np.float32)
    mb=np.dstack([db,np.clip(wb.astype(np.float32)/60,0,1),np.zeros_like(qp)]).astype(np.float32)
    return ma,mb

def arrdiff(a,b):
    d=np.abs(a.astype(np.float64)-b.astype(np.float64))
    return {'max':float(d.max()),'mean':float(d.mean()),'gt1e6':int(np.sum(d>1e-6))}

def imgdiff(a,b):
    d=np.mean(np.abs(a-b),2)
    return {'maxLSB':float(d.max()*255),'meanLSB':float(d.mean()*255),'gt001LSB':float(100*np.mean(d>.01/255)),'gt01LSB':float(100*np.mean(d>.1/255))}

def process(ds):
    d=Path(ds); A,B,f,b,efq,ebq,qfq,qbq,inf,inb=build_inputs(d)
    rows=[]
    for t in [0.0,.1,.25,.4,.5,.6,.75,.9,1.0]:
        ma,mb=maps(A,B,f,b,efq,ebq,qfq,qbq,inf,inb,t)
        ts=1.0-t
        dsa,wsa=splat_phase_fixed(f,efq,qfq,inf,ts,True)
        dsb,wsb=splat_phase_fixed(b,ebq,qbq,inb,ts,False)
        qps=.5*((qbq.astype(np.float32)*8)*inb+(qfq.astype(np.float32)*8)*inf)
        msa=np.dstack([dsa,np.clip(wsa.astype(np.float32)/60,0,1),qps]).astype(np.float32)
        msb=np.dstack([dsb,np.clip(wsb.astype(np.float32)/60,0,1),np.zeros_like(qps)]).astype(np.float32)
        S=alt(A,B,ma,mb,t); SS=alt(B,A,msa,msb,ts)
        rows.append({'t':t,'phaseEnvelope':4*t*(1-t),'mapA_vs_swappedB':arrdiff(ma[...,:3],msb[...,:3]),'mapB_vs_swappedA':arrdiff(mb[...,:3],msa[...,:3]),'alternateSwap':imgdiff(S,SS),'endpointA':imgdiff(S,A) if t==0 else None,'endpointB':imgdiff(S,B) if t==1 else None})
    print(json.dumps({'name':d.name,'rows':rows}),flush=True)

if __name__=='__main__':
    for p in sys.argv[1:]: process(p)
