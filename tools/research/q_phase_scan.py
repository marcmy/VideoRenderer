from pathlib import Path
import numpy as np, cv2, json, sys
from robust_independent_test import load_rgb,load_flow,smoothstep

def samp(im,x,y):
    return cv2.remap(im,x.astype(np.float32),y.astype(np.float32),cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)

def qocc(d,ox,oy):
    d=Path(d); A=load_rgb(d,'frame-A.bmp'); B=load_rgb(d,'frame-B.bmp'); f=load_flow(d,'flow-forward-B-to-A-s10.5.bin'); b=load_flow(d,'flow-backward-A-to-B-s10.5.bin')
    H,W=A.shape[:2]; fh,fw=f.shape[:2]; yy,xx=np.mgrid[0:fh,0:fw].astype(np.float32); px=xx*4+ox; py=yy*4+oy; w=np.array([.2126,.7152,.0722],np.float32)
    B0=samp(B,px,py); Aw=samp(A,px+f[...,0],py+f[...,1]); A0=samp(A,px,py); Bw=samp(B,px+b[...,0],py+b[...,1])
    yB=B0@w; yAw=Aw@w; yA=A0@w; yBw=Bw@w
    inf=(px+f[...,0]>=0)&(px+f[...,0]<=W-1)&(py+f[...,1]>=0)&(py+f[...,1]<=H-1)
    inb=(px+b[...,0]>=0)&(px+b[...,0]<=W-1)&(py+b[...,1]>=0)&(py+b[...,1]<=H-1)
    qf=16*255*np.abs(yB-yAw)/np.maximum(yB,1/255); qb=16*255*np.abs(yA-yBw)/np.maximum(yA,1/255)
    q=.5*(qf*inf+qb*inb)
    mx=max(1,int(fw*.04)); my=max(1,int(fh*.04)); m=np.zeros((fh,fw),bool); m[my:fh-my,mx:fw-mx]=1
    return float(100*np.sum(m&(q>=1600))/m.sum())

def one(d):
    vals={}
    for oy in [0,1,2,3]:
      for ox in [0,1,2,3]: vals[f'{ox},{oy}']=qocc(d,ox,oy)
    a=np.array(list(vals.values()))
    return {'name':Path(d).name,'mean':float(a.mean()),'min':float(a.min()),'max':float(a.max()),'span':float(a.max()-a.min()),'authority_min':float(smoothstep(15,25,np.array(a.min()))),'authority_max':float(smoothstep(15,25,np.array(a.max()))),'values':vals}

if __name__=='__main__': print(json.dumps([one(x) for x in sys.argv[1:]],indent=2))
