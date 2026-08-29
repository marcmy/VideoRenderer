from pathlib import Path
import numpy as np, cv2, math, json, os, time
from PIL import Image, ImageDraw
from numba import njit
from robust_independent_test import load_rgb,load_flow,consistency_fields,to8,smoothstep
from global_alg13_eval import field_occ

@njit
def splat_inverse(flow, err, valid, sigma=1.25, radius=3):
    h,w,_=flow.shape
    sx=np.zeros((h,w),np.float32); sy=np.zeros((h,w),np.float32); ws=np.zeros((h,w),np.float32)
    den=2*sigma*sigma
    for y in range(h):
      for x in range(w):
        if not valid[y,x]: continue
        fx=flow[y,x,0]; fy=flow[y,x,1]
        # source p -> midpoint m = p + 0.5*flow, coordinates in coarse-cell units
        tx=x+0.5*fx/4.0; ty=y+0.5*fy/4.0
        cx=int(math.floor(tx)); cy=int(math.floor(ty))
        conf=math.exp(-min(err[y,x],40.0)/8.0)
        # target-to-source inverse displacement is -0.5*flow in pixels
        dx=-0.5*fx; dy=-0.5*fy
        for oy in range(-radius,radius+1):
          yy=cy+oy
          if yy<0 or yy>=h: continue
          for ox in range(-radius,radius+1):
            xx=cx+ox
            if xx<0 or xx>=w: continue
            ddx=xx-tx; ddy=yy-ty
            k=math.exp(-(ddx*ddx+ddy*ddy)/den)*conf
            sx[yy,xx]+=dx*k; sy[yy,xx]+=dy*k; ws[yy,xx]+=k
    out=np.zeros((h,w,2),np.float32)
    for y in range(h):
      for x in range(w):
        if ws[y,x]>1e-7:
          out[y,x,0]=sx[y,x]/ws[y,x];out[y,x,1]=sy[y,x]/ws[y,x]
    return out,ws

def remap(img,x,y): return cv2.remap(img,x.astype(np.float32),y.astype(np.float32),cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)


def scene_stats(A,B):
    H,W=A.shape[:2]; sw,sh=32,18
    xs=np.minimum(((2*np.arange(sw)+1)*W)//(2*sw),W-1).astype(np.int32)
    ys=np.minimum(((2*np.arange(sh)+1)*H)//(2*sh),H-1).astype(np.int32)
    aa=A[np.ix_(ys,xs)]; bb=B[np.ix_(ys,xs)]
    ia=np.rint(np.mean(aa,axis=2)*255).astype(np.float64).ravel(); ib=np.rint(np.mean(bb,axis=2)*255).astype(np.float64).ravel()
    mad=float(np.mean(np.abs(ia-ib))/255.0)
    da=ia-ia.mean(); db=ib-ib.mean(); corr=float(np.dot(da,db)/max(np.sqrt(np.dot(da,da)*np.dot(db,db)),1e-12))
    return corr,mad,(corr<.15 and mad>.055)

def process(d,out):
 d=Path(d);name=d.name;t0=time.time();A=load_rgb(d,'frame-A.bmp');B=load_rgb(d,'frame-B.bmp');G=load_rgb(d,'midpoint-current.bmp');T=.5*(A+B);f=load_flow(d,'flow-forward-B-to-A-s10.5.bin');b=load_flow(d,'flow-backward-A-to-B-s10.5.bin');fe,be=consistency_fields(f,b);occ=field_occ(A,B,f,b);corr,mad,cut=scene_stats(A,B);H,W=A.shape[:2]
 # Separate endpoint maps. A->B b originates in A; B->A f originates in B.
 da,wa=splat_inverse(b,be,be<=20); db,wb=splat_inverse(f,fe,fe<=20)
 # Normalize coverage to stationary-field kernel mass. Empirically a static regular grid gives ~9.8 at sigma 1.25/r=3.
 ksum=sum(math.exp(-(x*x+y*y)/(2*1.25*1.25)) for y in range(-3,4) for x in range(-3,4))
 ca=np.clip(wa/ksum,0,1); cb=np.clip(wb/ksum,0,1)
 # Upsample target-midpoint inverse displacement and coverage to frame grid.
 DA=cv2.resize(da,(W,H),interpolation=cv2.INTER_LINEAR); DB=cv2.resize(db,(W,H),interpolation=cv2.INTER_LINEAR); CA=cv2.resize(ca,(W,H),interpolation=cv2.INTER_LINEAR); CB=cv2.resize(cb,(W,H),interpolation=cv2.INTER_LINEAR)
 yy,xx=np.mgrid[0:H,0:W].astype(np.float32)
 AW=remap(A,xx+DA[...,0],yy+DA[...,1]); BW=remap(B,xx+DB[...,0],yy+DB[...,1])
 denom=CA+CB+1e-6; SPL=(AW*CA[...,None]+BW*CB[...,None])/denom[...,None]
 # Fallback candidate when coverage is absent remains golden. Confidence favors genuine two-sided support.
 both=np.minimum(CA,CB); anyc=np.maximum(CA,CB)
 med=np.median(np.stack([G,SPL,T],0),axis=0).astype(np.float32)
 active=(occ>=20 and not cut)
 outs={}
 # direct splat reconstruction only where both sides have solid support; golden elsewhere
 for th in [.10,.20,.30]:
   c=smoothstep(th,min(.85,th+.35),both) if active else np.zeros_like(both)
   outs[f'SPL{int(th*100):02d}']=G*(1-c[...,None])+SPL*c[...,None]
   outs[f'MED{int(th*100):02d}']=G*(1-c[...,None])+med*c[...,None]
 # asymmetric: use one-sided candidate in cover/uncover, but median-anchor it to G/T
 chooseA=CA>CB; one=np.where(chooseA[...,None],AW,BW); asymmed=np.median(np.stack([G,one,T],0),axis=0).astype(np.float32)
 imbalance=np.abs(CA-CB); own=smoothstep(.05,.35,imbalance)*smoothstep(.05,.35,anyc)
 cown=own if active else np.zeros_like(own); outs['OWNMED50']=G*(1-(.5*cown)[...,None])+asymmed*(.5*cown)[...,None]
 os.makedirs(out,exist_ok=True)
 for k,im in [('GOLD',G),('TEMP',T),('AW',AW),('BW',BW),('SPL',SPL),('COVA',np.repeat(CA[...,None],3,2)),('COVB',np.repeat(CB[...,None],3,2)),*outs.items()]: Image.fromarray(to8(im)).save(Path(out)/(name+'_'+k+'.png'))
 def met(O):
  di=np.mean(np.abs(O-G),2); return {'mad':float(di.mean()),'chg1':float(100*np.mean(di>1/255)),'chg4':float(100*np.mean(di>4/255)),'chg8':float(100*np.mean(di>8/255)),'p99':float(np.quantile(di,.99))}
 M={'name':name,'field':float(occ),'corr':corr,'madAB':mad,'cut':bool(cut),'active':bool(active),'covAmean':float(CA.mean()),'covBmean':float(CB.mean()),'bothgt1':float(100*np.mean(both>.1)),'bothgt2':float(100*np.mean(both>.2)),'outs':{k:met(v) for k,v in outs.items()},'time':time.time()-t0};print(json.dumps(M),flush=True);return M

if __name__=='__main__':
 import sys
 for d in sys.argv[1:]: process(d,'/mnt/data/vr_work/splat_inverse')
