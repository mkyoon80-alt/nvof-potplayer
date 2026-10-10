"""Local quality comparison. Requires NumPy/OpenCV and separately supplied raw fixtures.
Metrics on real 2x synthesis are diagnostics, not an invented ground-truth score.
Only F1 90/130 holdouts have known intermediate source frames for full-reference errors.
"""
import argparse,csv,hashlib,json,subprocess,sys
from pathlib import Path
import numpy as np
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'build/analysis-pylibs'))
import cv2
p=argparse.ArgumentParser();p.add_argument('--baseline',type=Path,required=True);p.add_argument('--candidate',type=Path,required=True);p.add_argument('--output',type=Path,required=True);args=p.parse_args()
outroot=args.output.resolve();assert outroot.is_relative_to(root/'build') and not outroot.exists();outroot.mkdir(parents=True)
executables={'baseline':args.baseline.resolve(),'candidate':args.candidate.resolve()}
cases=[('kokoore-opening','kokoore-opening','nv12',1920,1080,False),('kokoore-80','kokoore-80','nv12',1920,1080,False),('jishou-hair','jishou-hair','nv12',1920,1080,False),('jishou-51','jishou-51','p010',1920,1080,False),('jishou-320','jishou-04-320','nv12',1920,1080,False),('jishou-1404','jishou-04-1404','nv12',1920,1080,False),('f1-title','f1-layers/titles','p010',3840,1604,False),('f1-84','f1-layers/84','p010',3840,1604,False),('f1-90','f1-boundaries','p010',3840,1604,True),('f1-130','f1-occlusion','p010',3840,1604,True)]
summary={}
for name,base,fmt,w,h,held in cases:
 folder=root/'build/native-synthesis'/base;raw=folder/(('holdout' if held else 'input')+'.'+fmt);times=folder/('holdout-times.txt' if held else 'timestamps.txt')
 paths={};arrays={};events={};record={}
 for version in ['baseline','candidate']:
  dest=outroot/name/version;dest.mkdir(parents=True)
  cmd=[str(executables[version]),str(root/'build/quality1/absent-runtime'),str(raw),str(times),str(dest),'native',str(834167 if held else 417083),fmt,str(w),str(h),'1920','0','4','medium']
  with (dest/'run.log').open('w') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
  paths[version]=dest/'output.nv12';arrays[version]=np.memmap(paths[version],np.uint8,mode='r').reshape(-1,h*3//2,w)
  events[version]=list(csv.DictReader((dest/'pairs.csv').open()));dur=[float(r['completed_ms']) for r in events[version][5:]]
  record[version]={'mean_ms':float(np.mean(dur)),'p95_ms':float(np.percentile(dur,95)),'first_ms':float(events[version][0]['completed_ms']),'repeated_pairs':sum(int(r['repeated_mask'])!=0 for r in events[version])}
 a,b=arrays['baseline'],arrays['candidate'];assert a.shape==b.shape
 assert all(np.array_equal(x,y) for x,y in zip(a[::2],b[::2])), 'Original frames changed'
 assert (outroot/name/'baseline/output.csv').read_bytes()==(outroot/name/'candidate/output.csv').read_bytes(),'Timeline changed'
 keys=['previous_pts','current_pts','phase_pts','scene_cut','repeated_mask','identical_skipped','grid','cost_buffers']
 assert [[e[k] for k in keys] for e in events['baseline']]==[[e[k] for k in keys] for e in events['candidate']],'Pair decisions changed'
 assert all(int(e['cost_buffers'])==0 for e in events['candidate'])
 record.update(originals_exact=True,timing_exact=True,decisions_exact=True,pairs=len(events['candidate']),output_frames=len(a))
 if held:
  truth_path=root/'build/native-synthesis'/('f1-boundaries/lab5' if name=='f1-90' else 'f1-occlusion/delivery')/'output.nv12'
  truth=np.memmap(truth_path,np.uint8,mode='r').reshape(-1,h*3//2,w)[::2,:h]
  roi=(slice(80,1550),slice(650,2850)) if name=='f1-90' else (slice(120,1550),slice(1550,3750))
  for version,result in arrays.items():
   values=[]
   for i in range(len(events[version])):
    ref=truth[i*2+1].astype(np.float32);mid=result[i*2+1,:h].astype(np.float32);row=[]
    for sl in [(slice(16,-16),slice(24,-24)),roi]:
     d=mid[sl]-ref[sl];lap=cv2.Laplacian(mid[sl],cv2.CV_32F)-cv2.Laplacian(ref[sl],cv2.CV_32F)
     row.extend([float(np.mean(abs(d))),float(np.mean(d*d)),float(np.mean(lap*lap))])
    values.append(row)
   record[version]['reference_errors']=np.mean(values,0).tolist();record[version]['per_pair_errors']=values
  record['error_columns']=['full_mae','full_mse','full_laplacian_mse','roi_mae','roi_mse','roi_laplacian_mse']
  del truth
 else:
  indices={'jishou-320':[48,60,96,99],'f1-title':[24,36,60,84],'kokoore-80':[0,3,6,12,24],'jishou-hair':[12,24,36]}.get(name,[12,24])
  for i in indices:
   i=min(i,len(a)//2-2);images=[]
   for label,frame in [('source A',a[i*2]),('baseline',a[i*2+1]),('candidate',b[i*2+1]),('source B',a[i*2+2])]:
    im=cv2.resize(cv2.cvtColor(frame,cv2.COLOR_YUV2BGR_NV12),(960,round(h*960/w)));cv2.putText(im,label,(8,25),0,.7,(0,255,255),1);images.append(im)
   cv2.imwrite(str(outroot/name/f'comparison-{i}.jpg'),np.vstack([np.hstack(images[:2]),np.hstack(images[2:])]))
   np.savez_compressed(outroot/name/f'pair-{i}.npz',a=a[i*2],baseline=a[i*2+1],candidate=b[i*2+1],b=a[i*2+2])
 summary[name]=record;(outroot/'summary.json').write_text(json.dumps(summary,indent=2))
 print(name,json.dumps({v:{k:x for k,x in record[v].items() if k!='per_pair_errors'} for v in ['baseline','candidate']}),flush=True)
 # Only disposable outputs within this invocation's new build folder are removed.
 del a,b,arrays
 if held:del result,mid,ref
 else:del frame
 for file in paths.values():
  assert file.resolve().is_relative_to(outroot) and file.name=='output.nv12' and not file.is_symlink();file.unlink()
