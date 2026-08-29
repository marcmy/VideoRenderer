from pathlib import Path
import numpy as np,cv2,json,sys
from robust_independent_test import load_rgb,load_flow

def calc(d):
 d=Path(d);A=load_rgb(d,'frame-A.bmp');B=load_rgb(d,'frame-B.bmp');f=load_flow(d,'flow-forward-B-to-A-s10.5.bin');b=load_flow(d,'flow-backward-A-to-B-s10.5.bin');H,W=A.shape[:2];fh,fw=f.shape[:2];yy,xx=np.mgrid[0:fh,0:fw].astype(np.float32);px=xx*4.;py=yy*4.;w=np.array([.2126,.7152,.0722],np.float32)
 def smp(im,x,y):return cv2.remap(im,x.astype(np.float32),y.astype(np.float32),cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
 B0=smp(B,px,py);Aw=smp(A,px+f[...,0],py+f[...,1]);A0=smp(A,px,py);Bw=smp(B,px+b[...,0],py+b[...,1]);yB=B0@w;yAw=Aw@w;yA=A0@w;yBw=Bw@w;qf=16*255*np.abs(yB-yAw)/np.maximum(yB,1/255);qb=16*255*np.abs(yA-yBw)/np.maximum(yA,1/255);inf=(px+f[...,0]>=0)&(px+f[...,0]<=W-1)&(py+f[...,1]>=0)&(py+f[...,1]<=H-1);inb=(px+b[...,0]>=0)&(px+b[...,0]<=W-1)&(py+b[...,1]>=0)&(py+b[...,1]<=H-1);cnt=inf.astype(np.float32)+inb.astype(np.float32);qfixed=.5*(qf*inf+qb*inb);qvalid=(qf*inf+qb*inb)/np.maximum(cnt,1);qmax=np.maximum(np.where(inf,qf,0),np.where(inb,qb,0));qmin=np.where(inf&inb,np.minimum(qf,qb),np.where(inf,qf,np.where(inb,qb,0)));mx=max(1,int(fw*.04));my=max(1,int(fh*.04));m=np.zeros((fh,fw),bool);m[my:fh-my,mx:fw-mx]=1
 def occ(q):return float(100*np.sum(m&(q>=1600))/m.sum())
 return {'name':d.name,'fixed':occ(qfixed),'validavg':occ(qvalid),'max':occ(qmax),'minValid':occ(qmin),'oneOOB':float(100*np.mean(m&(cnt==1))),'bothOOB':float(100*np.mean(m&(cnt==0)))}
if __name__=='__main__':print(json.dumps([calc(x) for x in sys.argv[1:]],indent=2))
