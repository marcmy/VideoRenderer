from pathlib import Path
import numpy as np, sys, json
sys.path.insert(0,'/mnt/data/vr_work')
import splat_phase_symmetry_v12 as base

PHASE_SCALE = 65536


def process(ds):
    d=Path(ds)
    A,B,f,b,efq,ebq,qfq,qbq,inf,inb=base.build_inputs(d)
    rows=[]
    for requested in [0.1,0.2,0.3,0.3333333,0.5,0.7,0.9]:
        phase_q=int(round(requested*PHASE_SCALE))
        t=phase_q/PHASE_SCALE
        ts=(PHASE_SCALE-phase_q)/PHASE_SCALE

        ma,mb=base.maps(A,B,f,b,efq,ebq,qfq,qbq,inf,inb,t)

        dsa,wsa=base.splat_phase_fixed(f,efq,qfq,inf,ts,True)
        dsb,wsb=base.splat_phase_fixed(b,ebq,qbq,inb,ts,False)
        qps=.5*((qbq.astype(np.float32)*8)*inb+(qfq.astype(np.float32)*8)*inf)
        msa=np.dstack([dsa,np.clip(wsa.astype(np.float32)/60,0,1),qps]).astype(np.float32)
        msb=np.dstack([dsb,np.clip(wsb.astype(np.float32)/60,0,1),np.zeros_like(qps)]).astype(np.float32)

        S=base.alt(A,B,ma,mb,t)
        SS=base.alt(B,A,msa,msb,ts)
        rows.append({
            'requested':requested,
            'phaseQ':phase_q,
            't':t,
            'complement':ts,
            'alternateSwap':base.imgdiff(S,SS),
            'mapA_vs_swappedB':base.arrdiff(ma[...,:3],msb[...,:3]),
            'mapB_vs_swappedA':base.arrdiff(mb[...,:3],msa[...,:3]),
        })

    print(json.dumps({'name':d.name,'phaseScale':PHASE_SCALE,'rows':rows}),flush=True)


if __name__=='__main__':
    for p in sys.argv[1:]: process(p)
