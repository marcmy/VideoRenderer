#!/usr/bin/env python3
"""Analyze NativeNvofInputSweep output in full-resolution pixel units."""
from __future__ import annotations
import argparse, json, re
from pathlib import Path
import cv2
import numpy as np

VAR_RE = re.compile(r"grid(\d+)-(bgra|nv12)-flow-forward-B-to-A-s10\.5\.bin$")

def rgb(path: Path):
    x=cv2.imread(str(path),cv2.IMREAD_COLOR)
    if x is None: raise FileNotFoundError(path)
    return x[...,::-1].astype(np.float32)/255.0

def luma(x): return .2126*x[...,0]+.7152*x[...,1]+.0722*x[...,2]

def load_flow(path: Path, gh:int, gw:int, scale:float):
    a=np.fromfile(path,dtype='<i2')
    if a.size != gh*gw*2: raise ValueError(f'{path}: {a.size} values != {gh*gw*2}')
    return a.reshape(gh,gw,2).astype(np.float32)/32.0*scale

def sample_field(flow,x,y):
    return cv2.remap(flow,x.astype(np.float32),y.astype(np.float32),cv2.INTER_LINEAR,
                     borderMode=cv2.BORDER_CONSTANT,borderValue=0)

def pct(a,p): return float(np.percentile(a,p)) if a.size else 0.0

def analyze_pair(A,B,f,b,grid,crop_w,crop_h):
    gh,gw=f.shape[:2]
    yy,xx=np.mgrid[0:gh,0:gw].astype(np.float32)
    px=xx*grid; py=yy*grid
    fx=px+f[...,0]; fy=py+f[...,1]
    bx=px+b[...,0]; by=py+b[...,1]
    fin=(fx>=0)&(fx<=crop_w-1)&(fy>=0)&(fy<=crop_h-1)
    bin=(bx>=0)&(bx<=crop_w-1)&(by>=0)&(by<=crop_h-1)
    br=sample_field(b,fx/grid,fy/grid)
    fr=sample_field(f,bx/grid,by/grid)
    fe=np.linalg.norm(f+br,axis=2)[fin]
    be=np.linalg.norm(b+fr,axis=2)[bin]

    YA,YB=luma(A),luma(B)
    srcB=cv2.remap(YB,px,py,cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
    dstA=cv2.remap(YA,fx,fy,cv2.INTER_LINEAR,borderMode=cv2.BORDER_CONSTANT,borderValue=0)
    srcA=cv2.remap(YA,px,py,cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
    dstB=cv2.remap(YB,bx,by,cv2.INTER_LINEAR,borderMode=cv2.BORDER_CONSTANT,borderValue=0)
    qba=16*255*np.abs(srcB-dstA)/np.maximum(srcB,1/255)
    qab=16*255*np.abs(srcA-dstB)/np.maximum(srcA,1/255)
    q=.5*(qba*fin+qab*bin)
    border_x=max(int(gw*.04),1); border_y=max(int(gh*.04),1)
    interior=np.zeros((gh,gw),bool)
    if gw>2*border_x and gh>2*border_y:
        interior[border_y:gh-border_y,border_x:gw-border_x]=True
    qi=q[interior]

    rough=[]
    if gw>1: rough.append(np.linalg.norm(f[:,1:]-f[:,:-1],axis=2).ravel())
    if gh>1: rough.append(np.linalg.norm(f[1:]-f[:-1],axis=2).ravel())
    rough=np.concatenate(rough) if rough else np.array([],np.float32)
    return {
      'flow_cells':[gw,gh],
      'forward_inbounds_pct':float(fin.mean()*100),'backward_inbounds_pct':float(bin.mean()*100),
      'forward_roundtrip_median_px':pct(fe,50),'forward_roundtrip_p90_px':pct(fe,90),'forward_roundtrip_p99_px':pct(fe,99),
      'backward_roundtrip_median_px':pct(be,50),'backward_roundtrip_p90_px':pct(be,90),'backward_roundtrip_p99_px':pct(be,99),
      'forward_consistent20_pct':float((fe<=20).mean()*100) if fe.size else 0,
      'backward_consistent20_pct':float((be<=20).mean()*100) if be.size else 0,
      'q_m1_occupancy_pct':float((qi>=1600).mean()*100) if qi.size else 0,
      'q_m2_occupancy_pct':float((qi>=2800).mean()*100) if qi.size else 0,
      'q_scene_occupancy_pct':float((qi>=4000).mean()*100) if qi.size else 0,
      'flow_neighbor_delta_median_px':pct(rough,50),'flow_neighbor_delta_p90_px':pct(rough,90),'flow_neighbor_delta_p99_px':pct(rough,99),
    }

def resample_to(flow,grid,out_h,out_w):
    yy,xx=np.mgrid[0:out_h,0:out_w].astype(np.float32)
    return sample_field(flow,(xx*4)/grid,(yy*4)/grid)

def main():
    ap=argparse.ArgumentParser();ap.add_argument('capture',type=Path);ap.add_argument('sweep',type=Path);args=ap.parse_args()
    A=rgb(args.capture/'frame-A.bmp');B=rgb(args.capture/'frame-B.bmp');H,W=A.shape[:2]
    variants={}
    for fp in args.sweep.glob('grid*-flow-forward-B-to-A-s10.5.bin'):
        m=VAR_RE.match(fp.name)
        if not m: continue
        grid=int(m.group(1));fmt=m.group(2);cw=W-W%grid;ch=H-H%grid;tw=(cw//grid)*4;th=(ch//grid)*4;gw=tw//4;gh=th//4;scale=grid/4
        bp=args.sweep/f'grid{grid}-{fmt}-flow-backward-A-to-B-s10.5.bin'
        if not bp.exists(): continue
        f=load_flow(fp,gh,gw,scale);b=load_flow(bp,gh,gw,scale)
        variants[(grid,fmt)]=(f,b,cw,ch)
    report={'source':[W,H],'variants':{},'same_grid_format_delta':{}}
    for (grid,fmt),(f,b,cw,ch) in sorted(variants.items()):
        report['variants'][f'{grid}-{fmt}']=analyze_pair(A,B,f,b,grid,cw,ch)
    for grid in sorted({k[0] for k in variants}):
        if (grid,'bgra') not in variants or (grid,'nv12') not in variants: continue
        fb,bb,_,_=variants[(grid,'bgra')]; fn,bn,_,_=variants[(grid,'nv12')]
        df=np.linalg.norm(fb-fn,axis=2).ravel();db=np.linalg.norm(bb-bn,axis=2).ravel()
        report['same_grid_format_delta'][str(grid)]={
          'forward_mean_px':float(df.mean()),'forward_p90_px':pct(df,90),'forward_p99_px':pct(df,99),
          'backward_mean_px':float(db.mean()),'backward_p90_px':pct(db,90),'backward_p99_px':pct(db,99)}
    capf=args.capture/'flow-forward-B-to-A-s10.5.bin'; capb=args.capture/'flow-backward-A-to-B-s10.5.bin'
    if capf.exists() and capb.exists():
        n=capf.stat().st_size//4
        capgw=(W+3)//4; capgh=n//capgw
        if capgw*capgh==n:
            cf=np.fromfile(capf,'<i2').reshape(capgh,capgw,2).astype(np.float32)/32
            cb=np.fromfile(capb,'<i2').reshape(capgh,capgw,2).astype(np.float32)/32
            report['vs_captured']={}
            for (grid,fmt),(f,b,_,_) in sorted(variants.items()):
                rr_f=resample_to(f,grid,capgh,capgw);rr_b=resample_to(b,grid,capgh,capgw)
                d1=np.linalg.norm(rr_f-cf,axis=2).ravel();d2=np.linalg.norm(rr_b-cb,axis=2).ravel()
                report['vs_captured'][f'{grid}-{fmt}']={'forward_mean_px':float(d1.mean()),'forward_p90_px':pct(d1,90),'forward_p99_px':pct(d1,99),'backward_mean_px':float(d2.mean()),'backward_p90_px':pct(d2,90),'backward_p99_px':pct(d2,99)}
    print(json.dumps(report,indent=2,sort_keys=True))
if __name__=='__main__':main()
