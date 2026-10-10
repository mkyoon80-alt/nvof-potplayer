from pathlib import Path
import sys,json,csv,numpy as np
sys.path.insert(0,str(Path('build/analysis-pylibs').resolve()))
import cv2
label=sys.argv[1];folder=Path(sys.argv[2]).resolve()
assert folder.is_relative_to(Path('build/flow1').resolve())
w,h=map(int,sys.argv[3:5])
a=np.memmap(folder/'output.nv12',np.uint8,mode='r').reshape(-1,h*3//2,w)
base=Path('build/cost1/clips')/('final-'+label)
b=np.memmap(base/'output.nv12',np.uint8,mode='r').reshape(a.shape)
assert (folder/'output.csv').read_bytes()==(base/'output.csv').read_bytes(),'Timeline changed'
assert all(np.array_equal(x,y) for x,y in zip(a[::2],b[::2])),'Originals changed'
def rows(p): return list(csv.DictReader(p.open()))
events=rows(folder/'pairs.csv');old=rows(base/'pairs.csv')
keys=['scene_cut','repeated_mask','identical_skipped']
changed={k:sum(x[k]!=y[k] for x,y in zip(events,old)) for k in keys}
assert all(int(e['cost_buffers'])==0 for e in events),'Cost generation still active'
live=[e for e in events if int(e['grid'])]
configs=sorted(set((int(e['analysis_width']),int(e['analysis_height']),int(e['grid'])) for e in live))
times=[float(e['completed_ms']) for e in events[1:] if e['scene_cut']=='0' and e['identical_skipped']=='0']
report={'label':label,'frames':len(a),'originals_timing_equal':True,'decisions_changed_vs_cost1':changed,'actual_analysis':configs,'cost_buffers':0,
 'completed_ms_warm':dict(zip(['mean','p50','p95','max'],[float(np.mean(times)),*map(float,np.percentile(times,[50,95])),float(max(times))])),
 'first_pair_completed_ms':float(events[0]['completed_ms'])}
if 'heldout' in label:
 name=label.split('-')[-1]
 ref=Path('build/native-synthesis')/('f1-boundaries/lab5' if name=='90' else 'f1-occlusion/delivery')/'output.nv12'
 truth=np.memmap(ref,np.uint8,mode='r').reshape(-1,2406,3840)[::2,:1604]
 roi=(slice(80,1550),slice(650,2850)) if name=='90' else (slice(120,1550),slice(1550,3750))
 values=[]
 for i in range(31):
  aa=a[2*i+1,:h].astype(np.float32);bb=truth[2*i+1].astype(np.float32);v=[]
  for sl in [(slice(16,-16),slice(24,-24)),roi]:
   diff=aa[sl]-bb[sl];lap=cv2.Laplacian(aa[sl],cv2.CV_32F)-cv2.Laplacian(bb[sl],cv2.CV_32F)
   v.extend([float(np.mean(abs(diff))),float(np.mean(diff*diff)),float(np.mean(lap*lap))])
  values.append(v)
 report.update(columns=['full_mae','full_mse','full_lap_mse','roi_mae','roi_mse','roi_lap_mse'],mean=np.mean(values,0).tolist(),per_pair=values)
else:
 indices={'jishou-320':[24,72,99,120],'title':[24,48,72],'kokoore':[24],'hair':[24]}.get(label,[24])
 for i in indices:
  i=min(i,len(a)//2-2)
  np.savez_compressed(folder/f'pair-{i}.npz',a=a[2*i],mid=a[2*i+1],b=a[2*i+2])
  ims=[]
  for pixels,title in [(a[2*i],'Source A'),(b[2*i+1],'Cost.1'),(a[2*i+1],folder.parent.name),(a[2*i+2],'Source B')]:
   im=cv2.resize(cv2.cvtColor(pixels,cv2.COLOR_YUV2BGR_NV12),(960,int(960*h/w)))
   cv2.putText(im,title,(8,25),0,.65,(0,255,255),1);ims.append(im)
  cv2.imwrite(str(folder/f'comparison-{i}.jpg'),np.vstack([np.hstack(ims[:2]),np.hstack(ims[2:])]))
(folder/'metrics.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(label,{k:v for k,v in report.items() if k!='per_pair'},flush=True)
del a,b

