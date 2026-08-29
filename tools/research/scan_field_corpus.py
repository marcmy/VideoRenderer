from pathlib import Path
import sys,json
sys.path.insert(0,'/mnt/data/vr_work')
from robust_independent_test import load_rgb,load_flow
from splat_qrisk_sweep import q_map
from splat_inverse_prototype import scene_stats
roots=[Path('/mnt/data/vr_work/boromir'),Path('/mnt/data/vr_work/older'),Path('/mnt/data/svp-research/captures1'),Path('/mnt/data/svp-research/bilbo')]
rows=[]
for root in roots:
 if not root.exists(): continue
 ds=[root] if (root/'metadata.txt').exists() else [p for p in root.iterdir() if p.is_dir() and (p/'metadata.txt').exists()]
 for d in ds:
  try:
   A=load_rgb(d,'frame-A.bmp');B=load_rgb(d,'frame-B.bmp');f=load_flow(d,'flow-forward-B-to-A-s10.5.bin');b=load_flow(d,'flow-backward-A-to-B-s10.5.bin');q,occ,_=q_map(A,B,f,b);corr,mad,cut=scene_stats(A,B);rows.append(dict(name=d.name,path=str(d),field=occ,corr=corr,mad=mad,cut=cut))
  except Exception as e: rows.append(dict(name=d.name,path=str(d),error=str(e)))
rows.sort(key=lambda x:x.get('field',-1))
print(json.dumps(rows,indent=2))
