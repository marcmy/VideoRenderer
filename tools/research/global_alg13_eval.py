from pathlib import Path
import numpy as np, cv2, json, os, gc, math
from PIL import Image, ImageDraw
from robust_independent_test import load_rgb,load_flow,consistency_fields,jfa_seed,dense_oneway,endpoint_candidate,seed_dist,med3,to8

def field_occ(A,B,f,b):
    # top-left coarse-cell sample, Rec.709 luminance, 4x4-equivalent normalized SAD
    H,W=A.shape[:2]; fh,fw=f.shape[:2]
    yy,xx=np.mgrid[0:fh,0:fw].astype(np.float32); px=xx*4.; py=yy*4.
    def smp(img,x,y):
        return cv2.remap(img,x.astype(np.float32),y.astype(np.float32),cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
    w=np.array([.2126,.7152,.0722],np.float32)
    B0=smp(B,px,py); Aw=smp(A,px+f[...,0],py+f[...,1]); A0=smp(A,px,py); Bw=smp(B,px+b[...,0],py+b[...,1])
    yB=B0@w; yAw=Aw@w; yA=A0@w; yBw=Bw@w
    inf=(px+f[...,0]>=0)&(px+f[...,0]<=W-1)&(py+f[...,1]>=0)&(py+f[...,1]<=H-1)
    inb=(px+b[...,0]>=0)&(px+b[...,0]<=W-1)&(py+b[...,1]>=0)&(py+b[...,1]<=H-1)
    qf=16*255*np.abs(yB-yAw)/np.maximum(yB,1/255)
    qb=16*255*np.abs(yA-yBw)/np.maximum(yA,1/255)
    q=.5*(qf*inf+qb*inb)
    mx=max(1,int(fw*.04)); my=max(1,int(fh*.04)); m=np.zeros((fh,fw),bool);m[my:fh-my,mx:fw-mx]=1
    return 100*np.sum(m&(q>=1600))/m.sum()

def process(d,outdir):
    d=Path(d); A=load_rgb(d,'frame-A.bmp');B=load_rgb(d,'frame-B.bmp');G=load_rgb(d,'midpoint-current.bmp');T=.5*(A+B)
    f=load_flow(d,'flow-forward-B-to-A-s10.5.bin');b=load_flow(d,'flow-backward-A-to-B-s10.5.bin')
    fe,be=consistency_fields(f,b); sf=jfa_seed((fe<=20).astype(np.uint8)); sb=jfa_seed((be<=20).astype(np.uint8));H,W=A.shape[:2]
    df=dense_oneway(sf,f,B,H,W); da=dense_oneway(sb,b,A,H,W)
    ca,pa,ta,adist,fa=endpoint_candidate(A,B,da,seed_dist(sb)); cb,pb,tb,bdist,fb=endpoint_candidate(B,A,df,seed_dist(sf))
    M=med3(ca,cb,T); occ=field_occ(A,B,f,b)
    diff=np.mean(np.abs(M-G),axis=2)
    # median confidence: at least one directional hypothesis should be locally plausible; reject places where both are topology-bad.
    pair=np.sqrt(np.sum((fa+fb)**2,axis=2))
    good=(np.minimum(pa,pb)<.06)&(np.minimum(ta,tb)<.20)&(np.minimum(adist,bdist)<8)&(pair<24)
    # confident global force13 variant; feather confidence, but do NOT require old both-invalid core.
    conf=(1-np.clip((np.minimum(pa,pb)-.025)/.05,0,1))*(1-np.clip((np.minimum(ta,tb)-.02)/.30,0,1))*(1-np.clip((np.minimum(adist,bdist)-1)/8,0,1))
    conf*=good.astype(np.float32); conf=cv2.GaussianBlur(conf.astype(np.float32),(0,0),1.25)
    alpha=np.minimum(conf,.5) if occ>=20 else np.zeros_like(conf)
    S=G*(1-alpha[...,None])+M*alpha[...,None]
    os.makedirs(outdir,exist_ok=True)
    for n,im in [('gold',G),('global',M),('softglobal',S),('temp',T),('Acomp',ca),('Bcomp',cb)]: Image.fromarray(to8(im)).save(Path(outdir)/(d.name+'_'+n+'.png'))
    met={'name':d.name,'field_occ':float(occ),'global_mad':float(diff.mean()),'global_change_gt1':float(100*np.mean(diff>1/255)),'global_change_gt4':float(100*np.mean(diff>4/255)),'soft_alpha_mean':float(alpha.mean()),'soft_alpha_gt05':float(100*np.mean(alpha>.05)),'soft_mad':float(np.mean(np.abs(S-G))), 'pair_med':float(np.median(pair))}
    print(json.dumps(met),flush=True); return met

def sheet(outdir,names):
    rows=[]
    for name in names:
        ims=[]
        for suf in ['gold','global','softglobal','temp']:
            p=Path(outdir)/(name+'_'+suf+'.png'); ims.append(Image.open(p).convert('RGB'))
        rows.append((name,ims))
    tw=480; lab=28
    h=int(rows[0][1][0].height*tw/rows[0][1][0].width)
    C=Image.new('RGB',(tw*4,(h+lab)*len(rows)),'white');dr=ImageDraw.Draw(C)
    for r,(name,ims) in enumerate(rows):
        for c,(suf,im) in enumerate(zip(['GOLD','ALG13 GLOBAL','ALG13 SOFT','TEMP'],ims)):
            x=c*tw;y=r*(h+lab);dr.text((x+3,y+3),f'{name[-18:-9]} {suf}',fill='black');C.paste(im.resize((tw,h),Image.Resampling.LANCZOS),(x,y+lab))
    C.save(Path(outdir)/'global_alg13_sheet.jpg',quality=92)

if __name__=='__main__':
    caps=[
'/mnt/data/vr_work/older/capture-20260824-234854-252-pid32508',
'/mnt/data/vr_work/older/capture-20260824-234925-860-pid32508',
'/mnt/data/vr_work/older/capture-20260824-235102-889-pid32508',
'/mnt/data/vr_work/older/capture-20260825-020950-720-pid28564',
'/mnt/data/vr_work/older/capture-20260825-021001-304-pid28564',
'/mnt/data/vr_work/boromir/capture-20260825-022530-672-pid28564',
'/mnt/data/vr_work/boromir/capture-20260825-022539-208-pid28564',
'/mnt/data/vr_work/boromir/capture-20260825-022550-867-pid28564']
    out='/mnt/data/vr_work/global_alg13';mets=[]
    for c in caps: mets.append(process(c,out));gc.collect()
    sheet(out,[Path(c).name for c in caps]);open(Path(out)/'metrics.json','w').write(json.dumps(mets,indent=2))
