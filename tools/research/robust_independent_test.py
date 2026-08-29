import os, math, argparse, time, json
from pathlib import Path
import numpy as np
import cv2
from PIL import Image, ImageDraw
from numba import njit, prange

INV=np.uint32(0xffffffff)

def load_rgb(d,name):
    return np.array(Image.open(Path(d)/name).convert('RGB'),dtype=np.float32)/255.0

def load_flow(d,name):
    meta={}
    for line in open(Path(d)/'metadata.txt','r',encoding='utf-8'):
        if '=' in line:
            k,v=line.strip().split('=',1); meta[k]=v
    fh=int(meta['flow_height']); fw=int(meta['flow_width'])
    return np.fromfile(Path(d)/name,dtype='<i2').reshape(fh,fw,2).astype(np.float32)/32.0

@njit
def sample_flow_scalar(flow, px, py, grid=4.0):
    h,w,_=flow.shape
    gx=px/grid; gy=py/grid
    bx=math.floor(gx); by=math.floor(gy)
    fx=gx-bx; fy=gy-by
    x0=0 if bx<0 else w-1 if bx>w-1 else bx
    x1=0 if bx+1<0 else w-1 if bx+1>w-1 else bx+1
    y0=0 if by<0 else h-1 if by>h-1 else by
    y1=0 if by+1<0 else h-1 if by+1>h-1 else by+1
    o0=flow[y0,x0,0]*(1-fx)+flow[y0,x1,0]*fx
    o1=flow[y0,x0,1]*(1-fx)+flow[y0,x1,1]*fx
    q0=flow[y1,x0,0]*(1-fx)+flow[y1,x1,0]*fx
    q1=flow[y1,x0,1]*(1-fx)+flow[y1,x1,1]*fx
    return o0*(1-fy)+q0*fy, o1*(1-fy)+q1*fy

@njit(parallel=True)
def consistency_fields(f,b):
    fh,fw,_=f.shape
    fe=np.empty((fh,fw),np.float32); be=np.empty((fh,fw),np.float32)
    for y in prange(fh):
        for x in range(fw):
            px=x*4.0; py=y*4.0
            fx=f[y,x,0]; fy=f[y,x,1]
            bx=b[y,x,0]; by=b[y,x,1]
            sbx,sby=sample_flow_scalar(b,px+fx,py+fy,4.0)
            sfx,sfy=sample_flow_scalar(f,px+bx,py+by,4.0)
            fe[y,x]=math.sqrt((fx+sbx)**2+(fy+sby)**2)
            be[y,x]=math.sqrt((bx+sfx)**2+(by+sfy)**2)
    return fe,be

@njit
def jfa_seed(valid):
    h,w=valid.shape
    a=np.empty((h,w),np.uint32); tmp=np.empty_like(a)
    for y in range(h):
        for x in range(w):
            a[y,x]=np.uint32((y<<16)|(x&0xffff)) if valid[y,x] else INV
    step=1; md=max(w,h)
    while step<md: step<<=1
    step>>=1
    while step>=1:
        for y in range(h):
            for x in range(w):
                best=a[y,x]
                if best==INV: bestd=3.402823466e38
                else:
                    sx=int(best & np.uint32(0xffff)); sy=int((best>>np.uint32(16)) & np.uint32(0x7fff))
                    dx=sx-x; dy=sy-y; bestd=float(dx*dx+dy*dy)
                for oy in range(-1,2):
                    cy=y+oy*step
                    if cy<0: cy=0
                    elif cy>=h: cy=h-1
                    for ox in range(-1,2):
                        cx=x+ox*step
                        if cx<0: cx=0
                        elif cx>=w: cx=w-1
                        cand=a[cy,cx]
                        if cand==INV: cd=3.402823466e38
                        else:
                            sx=int(cand & np.uint32(0xffff)); sy=int((cand>>np.uint32(16)) & np.uint32(0x7fff))
                            dx=sx-x; dy=sy-y; cd=float(dx*dx+dy*dy)
                        if cd<bestd or (cd==bestd and cand<best): bestd=cd; best=cand
                tmp[y,x]=best
        a,tmp=tmp,a
        if step==1: break
        step>>=1
    return a

@njit(parallel=True)
def dense_oneway(seedmap, flow, guide, H, W):
    fh,fw,_=flow.shape
    out=np.empty((H,W,2),np.float32)
    spatialDen=2*1.25*1.25; colorDen=2*0.10*0.10
    for y in prange(H):
        gy=y/4.0; by0=math.floor(gy)
        for x in range(W):
            gx=x/4.0; bx0=math.floor(gx)
            gr0=guide[y,x,0]; gr1=guide[y,x,1]; gr2=guide[y,x,2]
            sxsum=0.0; sysum=0.0; wsum=0.0
            cy0=0 if by0<0 else fh-1 if by0>=fh else by0
            cx0=0 if bx0<0 else fw-1 if bx0>=fw else bx0
            nearest=seedmap[cy0,cx0]
            for oy in range(-2,3):
                cy=by0+oy
                if cy<0: cy=0
                elif cy>=fh: cy=fh-1
                for ox in range(-2,3):
                    cx=bx0+ox
                    if cx<0: cx=0
                    elif cx>=fw: cx=fw-1
                    packed=seedmap[cy,cx]
                    if packed==INV: continue
                    seedx=int(packed & np.uint32(0xffff)); seedy=int((packed>>np.uint32(16)) & np.uint32(0x7fff))
                    ddx=gx-cx; ddy=gy-cy
                    sw=math.exp(-(ddx*ddx+ddy*ddy)/spatialDen)
                    spx=seedx*4; spy=seedy*4
                    if spx>=W: spx=W-1
                    if spy>=H: spy=H-1
                    c0=gr0-guide[spy,spx,0]; c1=gr1-guide[spy,spx,1]; c2=gr2-guide[spy,spx,2]
                    cw=math.exp(-(c0*c0+c1*c1+c2*c2)/colorDen)
                    idx=seedx-cx; idy=seedy-cy
                    iw=math.exp(-math.sqrt(idx*idx+idy*idy)/8.0)
                    wt=sw*cw*iw
                    sxsum+=flow[seedy,seedx,0]*wt; sysum+=flow[seedy,seedx,1]*wt; wsum+=wt
            if wsum>1e-5:
                out[y,x,0]=sxsum/wsum; out[y,x,1]=sysum/wsum
            elif nearest!=INV:
                sx=int(nearest & np.uint32(0xffff)); sy=int((nearest>>np.uint32(16)) & np.uint32(0x7fff))
                out[y,x,0]=flow[sy,sx,0]; out[y,x,1]=flow[sy,sx,1]
            else:
                out[y,x,0]=0.0; out[y,x,1]=0.0
    return out

def remap(img,x,y):
    return cv2.remap(img,x.astype(np.float32),y.astype(np.float32),cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)

def smoothstep(a,b,x):
    t=np.clip((x-a)/(b-a),0,1); return t*t*(3-2*t)

def topo_reject(dense,sx,sy):
    fpx=remap(dense,sx+1,sy); fmx=remap(dense,sx-1,sy)
    fpy=remap(dense,sx,sy+1); fmy=remap(dense,sx,sy-1)
    dx=.5*(fpx-fmx); dy=.5*(fpy-fmy)
    ax=1+.5*dx[...,0]; ay=.5*dx[...,1]; bx=.5*dy[...,0]; by=1+.5*dy[...,1]
    det=ax*by-ay*bx
    frob=ax*ax+ay*ay+bx*bx+by*by
    disc=np.sqrt(np.maximum(frob*frob-4*det*det,0))
    smax=np.sqrt(np.maximum(.5*(frob+disc),0)); smin=np.sqrt(np.maximum(.5*(frob-disc),0))
    return np.clip(np.maximum(1-smoothstep(.10,.35,det),np.maximum(smoothstep(2.25,3.50,smax),1-smoothstep(.20,.45,smin))),0,1)

def seed_dist(seedmap):
    h,w=seedmap.shape; yy,xx=np.mgrid[0:h,0:w]
    sx=(seedmap & np.uint32(0xffff)).astype(np.int32); sy=((seedmap>>np.uint32(16))&np.uint32(0x7fff)).astype(np.int32)
    d=np.sqrt((sx-xx)**2+(sy-yy)**2).astype(np.float32); d[seedmap==INV]=9999
    return d


def repair_motion_dense(f,b,fe,be,H,W):
    from scipy.ndimage import convolve
    choose_b=(be<=fe)
    rm=np.where(choose_b[...,None],b,-f).astype(np.float32)
    conf=np.exp(-np.minimum(np.minimum(fe,be),80.0)/10.0).astype(np.float32)
    yy,xx=np.mgrid[-2:3,-2:3]; k=np.exp(-(xx*xx+yy*yy)/(2*1.25*1.25)).astype(np.float32)
    wc=np.maximum(conf,1e-4); den=convolve(wc,k,mode='nearest')
    out=np.empty_like(rm)
    for c in range(2): out[...,c]=convolve(rm[...,c]*wc,k,mode='nearest')/np.maximum(den,1e-6)
    gy,gx=np.mgrid[0:H,0:W].astype(np.float32); mx=gx/4.0; my=gy/4.0
    return cv2.remap(out,mx,my,cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)

def repair_topology(rm):
    dx=.5*(np.roll(rm,-1,axis=1)-np.roll(rm,1,axis=1)); dy=.5*(np.roll(rm,-1,axis=0)-np.roll(rm,1,axis=0))
    def rej(sign):
        ax=1+sign*.5*dx[...,0]; ay=sign*.5*dx[...,1]; bx=sign*.5*dy[...,0]; by=1+sign*.5*dy[...,1]
        det=ax*by-ay*bx; frob=ax*ax+ay*ay+bx*bx+by*by
        disc=np.sqrt(np.maximum(frob*frob-4*det*det,0)); smax=np.sqrt(np.maximum(.5*(frob+disc),0)); smin=np.sqrt(np.maximum(.5*(frob-disc),0))
        return np.clip(np.maximum(1-smoothstep(.10,.35,det),np.maximum(smoothstep(2.25,3.50,smax),1-smoothstep(.20,.45,smin))),0,1)
    return np.maximum(rej(-1.0),rej(1.0))

def endpoint_candidate(img,opp,dense,distcoarse):
    H,W=img.shape[:2]; yy,xx=np.mgrid[0:H,0:W].astype(np.float32)
    sx=xx.copy(); sy=yy.copy()
    for _ in range(6):
        fl=remap(dense,sx,sy); sx=xx-.5*fl[...,0]; sy=yy-.5*fl[...,1]
    fl=remap(dense,sx,sy); cand=remap(img,sx,sy); match=remap(opp,sx+fl[...,0],sy+fl[...,1])
    photo=np.mean(np.abs(cand-match),axis=2); top=topo_reject(dense,sx,sy)
    dist=cv2.remap(distcoarse,(sx/4).astype(np.float32),(sy/4).astype(np.float32),cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
    return cand,photo,top,dist,fl

def med3(a,b,c): return np.median(np.stack([a,b,c],axis=0),axis=0).astype(np.float32)
def to8(x): return np.clip(np.rint(x*255),0,255).astype(np.uint8)

def make_sheet(gold,outs,temp,outpath):
    names=['GOLD']+list(outs.keys())+['TEMP']; ims=[gold]+list(outs.values())+[temp]
    H,W=gold.shape[:2]; tw=960; th=int(H*tw/W); lab=28; cols=2; rows=(len(ims)+1)//2
    can=Image.new('RGB',(cols*tw,rows*(th+lab)),(255,255,255)); dr=ImageDraw.Draw(can)
    for i,(name,im) in enumerate(zip(names,ims)):
        r=i//cols;c=i%cols; y=r*(th+lab)
        p=Image.fromarray(to8(im)).resize((tw,th),Image.Resampling.LANCZOS)
        can.paste(p,(c*tw,y+lab)); dr.text((c*tw+5,y+5),name,fill=(0,0,0))
    can.save(outpath,quality=92)

def process(d,outdir):
    d=Path(d); t0=time.time(); name=d.name
    A=load_rgb(d,'frame-A.bmp'); B=load_rgb(d,'frame-B.bmp'); gold=load_rgb(d,'midpoint-current.bmp'); temp=.5*(A+B)
    f=load_flow(d,'flow-forward-B-to-A-s10.5.bin'); b=load_flow(d,'flow-backward-A-to-B-s10.5.bin')
    fe,be=consistency_fields(f,b); vf=(fe<=20).astype(np.uint8); vb=(be<=20).astype(np.uint8); unsupported=~((fe<=20)|(be<=20))
    sf=jfa_seed(vf); sb=jfa_seed(vb); H,W=A.shape[:2]
    df=dense_oneway(sf,f,B,H,W); da=dense_oneway(sb,b,A,H,W)
    ca,pa,ta,adist,fa=endpoint_candidate(A,B,da,seed_dist(sb)); cb,pb,tb,bdist,fb=endpoint_candidate(B,A,df,seed_dist(sf))
    pair=np.sqrt(np.sum((fa+fb)**2,axis=2)); med=med3(ca,cb,temp)
    core=cv2.resize(unsupported.astype(np.uint8),(W,H),interpolation=cv2.INTER_NEAREST)>0
    tchange=np.mean(np.abs(A-B),axis=2)
    settings={'STRICT':(.040,.05,4.0,8.0),'MEDIUM':(.055,.10,6.0,16.0),'LOOSE':(.070,.20,8.0,24.0)}
    outs={}; metrics={}
    for key,(pth,tth,dth,qth) in settings.items():
        gate=core&(tchange>.02)&(pa<pth)&(pb<pth)&(ta<tth)&(tb<tth)&(adist<dth)&(bdist<dth)&(pair<qth)
        out=gold.copy(); out[gate]=med[gate]; outs[key]=out
        diff=np.mean(np.abs(out-gold),axis=2); mt=np.mean(np.abs(med-temp),axis=2)
        metrics[key]={'gate_pct':float(100*gate.mean()),'core_accept_pct':float(100*gate.sum()/max(1,core.sum())),
          'change_gt_1lsb_pct':float(100*np.mean(diff>1/255)),'mad_gold':float(diff.mean()),
          'restored_vs_temporal_gated':float(mt[gate].mean()) if gate.any() else 0.0,
          'photoA':float(pa[gate].mean()) if gate.any() else 0.0,'photoB':float(pb[gate].mean()) if gate.any() else 0.0,
          'pairerr':float(pair[gate].mean()) if gate.any() else 0.0,'seedDistA':float(adist[gate].mean()) if gate.any() else 0.0,'seedDistB':float(bdist[gate].mean()) if gate.any() else 0.0}

    # Confidence-weighted variant: no hard pixel replacement.  The median candidate
    # can only pull the established output gradually, and the confidence field is
    # spatially feathered across the both-invalid island boundary.
    core_soft=cv2.GaussianBlur(core.astype(np.float32),(0,0),3.0)
    pconf=(1-smoothstep(.020,.060,pa))*(1-smoothstep(.020,.060,pb))
    tconf=(1-np.maximum(ta,tb))
    dconf=1-smoothstep(1.0,6.0,np.maximum(adist,bdist))
    qconf=1-smoothstep(2.0,16.0,pair)
    mconf=smoothstep(.020,.050,tchange)
    conf=np.clip(core_soft*pconf*tconf*dconf*qconf*mconf,0,1)
    conf=cv2.GaussianBlur(conf.astype(np.float32),(0,0),1.25)
    for key,cap in [('SOFT25',.25),('SOFT50',.50)]:
        alpha=np.minimum(conf,cap)
        out=gold*(1-alpha[...,None])+med*alpha[...,None]; outs[key]=out
        diff=np.mean(np.abs(out-gold),axis=2)
        metrics[key]={'mean_alpha':float(alpha.mean()),'alpha_gt_.05_pct':float(100*np.mean(alpha>.05)),
          'change_gt_1lsb_pct':float(100*np.mean(diff>1/255)),'mad_gold':float(diff.mean()),
          'p99_change':float(np.quantile(diff,.99))}
    # Algorithm-13-style rescue using the renderer's coherent repair motion: two
    # symmetrically compensated endpoint hypotheses + unwarped temporal reference.
    # Unlike the independent-field trial, both endpoint samples share one repaired
    # correspondence, so we can demand direct A/B agreement before relaxing fallback.
    rmd=repair_motion_dense(f,b,fe,be,H,W)
    yy,xx=np.mgrid[0:H,0:W].astype(np.float32)
    rsa=remap(A,xx-.5*rmd[...,0],yy-.5*rmd[...,1]); rsb=remap(B,xx+.5*rmd[...,0],yy+.5*rmd[...,1])
    rphoto=np.mean(np.abs(rsa-rsb),axis=2); rtop=repair_topology(rmd); rmed=med3(rsa,rsb,temp)
    rconf=core.astype(np.float32)*(1-smoothstep(.020,.060,rphoto))*(1-rtop)*smoothstep(.020,.050,tchange)
    rconf=cv2.GaussianBlur(rconf.astype(np.float32),(0,0),1.5)
    for key,cap in [('REPMED25',.25),('REPMED50',.50)]:
        alpha=np.minimum(rconf,cap)
        out=gold*(1-alpha[...,None])+rmed*alpha[...,None]; outs[key]=out
        diff=np.mean(np.abs(out-gold),axis=2)
        metrics[key]={'mean_alpha':float(alpha.mean()),'alpha_gt_.05_pct':float(100*np.mean(alpha>.05)),
          'change_gt_1lsb_pct':float(100*np.mean(diff>1/255)),'mad_gold':float(diff.mean()),
          'p99_change':float(np.quantile(diff,.99)),'mean_repair_photo_active':float(rphoto[alpha>.05].mean()) if np.any(alpha>.05) else 0.0}

    metrics['base']={'unsupported_pct':float(100*unsupported.mean()),'core_pct':float(100*core.mean()),'time_s':float(time.time()-t0)}
    os.makedirs(outdir,exist_ok=True); make_sheet(gold,outs,temp,Path(outdir)/(name+'_independent_median.jpg'))
    with open(Path(outdir)/(name+'_metrics.json'),'w') as fh: json.dump(metrics,fh,indent=2)
    print(json.dumps({'name':name,**metrics}),flush=True)

if __name__=='__main__':
    ap=argparse.ArgumentParser(); ap.add_argument('capture'); ap.add_argument('--outdir',default='/mnt/data/vr_work/robust_out'); a=ap.parse_args(); process(a.capture,a.outdir)
