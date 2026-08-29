from pathlib import Path
import numpy as np, cv2, math, json, sys
from PIL import Image
from robust_independent_test import load_rgb, load_flow, consistency_fields, to8, smoothstep
from splat_inverse_prototype import remap, scene_stats
from splat_qrisk_sweep import q_map
from splat_quality_weight_sweep import splat_q, directional_q

OUT=Path('/mnt/data/vr_work/splat_ownership'); OUT.mkdir(exist_ok=True)

def metric(O,G):
    d=np.mean(np.abs(O-G),2)
    return {'mad':float(d.mean()),'chg1':float(100*np.mean(d>1/255)),
            'chg4':float(100*np.mean(d>4/255)),'chg8':float(100*np.mean(d>8/255)),
            'p99':float(np.quantile(d,.99))}

def process(ds,qscale=1200):
    d=Path(ds); n=d.name
    A=load_rgb(d,'frame-A.bmp'); B=load_rgb(d,'frame-B.bmp'); G=load_rgb(d,'midpoint-current.bmp'); T=.5*(A+B)
    f=load_flow(d,'flow-forward-B-to-A-s10.5.bin'); b=load_flow(d,'flow-backward-A-to-B-s10.5.bin')
    fe,be=consistency_fields(f,b); q,occ,_=q_map(A,B,f,b); qf,qb=directional_q(A,B,f,b)
    corr,mad,cut=scene_stats(A,B); active=occ>=20 and not cut
    H,W=A.shape[:2]; yy,xx=np.mgrid[0:H,0:W].astype(np.float32)
    sig=1.25; rad=3
    ksum=sum(math.exp(-(x*x+y*y)/(2*sig*sig)) for y in range(-rad,rad+1) for x in range(-rad,rad+1))
    da,wa=splat_q(b,be,qb,be<=20,sig,rad,qscale); db,wb=splat_q(f,fe,qf,fe<=20,sig,rad,qscale)
    CA=cv2.resize(np.clip(wa/ksum,0,1),(W,H),interpolation=cv2.INTER_LINEAR)
    CB=cv2.resize(np.clip(wb/ksum,0,1),(W,H),interpolation=cv2.INTER_LINEAR)
    DA=cv2.resize(da,(W,H),interpolation=cv2.INTER_LINEAR); DB=cv2.resize(db,(W,H),interpolation=cv2.INTER_LINEAR)
    AW=remap(A,xx+DA[...,0],yy+DA[...,1]); BW=remap(B,xx+DB[...,0],yy+DB[...,1])
    Q=cv2.resize(q,(W,H),interpolation=cv2.INTER_LINEAR)
    support=np.maximum(CA,CB)
    risk=smoothstep(1000,2200,Q)*smoothstep(.03,.22,support)
    risk=cv2.GaussianBlur(risk.astype(np.float32),(0,0),.8)
    if not active: risk*=0
    alpha=np.minimum(risk,.60)
    result={'name':n,'field':occ,'cut':bool(cut),'active':bool(active),'qscale':qscale,'variants':{}}
    variants={}
    for gamma in [1.0,1.5,2.0,3.0,4.0,8.0]:
        a=np.power(np.maximum(CA,1e-8),gamma); bb=np.power(np.maximum(CB,1e-8),gamma); den=a+bb
        S=np.where((den>1e-7)[...,None],(AW*a[...,None]+BW*bb[...,None])/np.maximum(den[...,None],1e-7),T)
        M=np.median(np.stack([G,S,T],0),axis=0).astype(np.float32)
        O=G*(1-alpha[...,None])+M*alpha[...,None]
        key=f'G{gamma:g}'
        variants[key]=O
        own=np.abs(a-bb)/np.maximum(den,1e-7)
        result['variants'][key]={**metric(O,G),
            'splatVsTemporal':float(np.mean(np.abs(S-T))),
            'ownershipMean':float(own.mean()),
            'ownershipGt50':float(100*np.mean(own>.5)),
            'alphaMean':float(alpha.mean())}
        Image.fromarray(to8(O)).save(OUT/f'{n}_QS{qscale}_{key}.png')
    Image.fromarray(to8(G)).save(OUT/f'{n}_GOLD.png')
    ratio=CA/np.maximum(CA+CB,1e-7)
    Image.fromarray(to8(np.repeat(ratio[...,None],3,2))).save(OUT/f'{n}_OWNRATIO.png')
    print(json.dumps(result),flush=True)
    return result

if __name__=='__main__':
    qs=int(sys.argv[1]) if len(sys.argv)>1 and sys.argv[1].isdigit() else 1200
    caps=sys.argv[2:] if len(sys.argv)>2 else []
    print(json.dumps([process(c,qs) for c in caps],indent=2))
