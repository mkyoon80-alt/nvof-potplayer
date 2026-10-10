import argparse, csv, hashlib, json, statistics, subprocess
from pathlib import Path
parser=argparse.ArgumentParser(description="Compare session reuse against an existing video_x2_repro binary; original local fixtures required.")
parser.add_argument('--baseline',type=Path,required=True)
parser.add_argument('--candidate',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
root=Path(__file__).resolve().parents[1];outroot=args.output.resolve()
assert outroot.is_relative_to(root/'build'),"Comparison output must be under the project build directory"
assert not outroot.exists(),"Use a new directory to preserve previous evidence"
outroot.mkdir(parents=True)
executables={'baseline':args.baseline.resolve(),'candidate':args.candidate.resolve()}
cases=[('kokoore-opening','kokoore-opening','nv12',1920,1080,False),('kokoore-80','kokoore-80','nv12',1920,1080,False),('jishou-hair','jishou-hair','nv12',1920,1080,False),('jishou-51','jishou-51','p010',1920,1080,False),('jishou-320','jishou-04-320','nv12',1920,1080,False),('jishou-1404','jishou-04-1404','nv12',1920,1080,False),('f1-title','f1-layers/titles','p010',3840,1604,False),('f1-84','f1-layers/84','p010',3840,1604,False),('f1-90','f1-boundaries','p010',3840,1604,True),('f1-130','f1-occlusion','p010',3840,1604,True)]
summary={}
for name,base,fmt,w,h,held in cases:
    folder=root/'build/native-synthesis'/base; raw=folder/(('holdout' if held else 'input')+'.'+fmt); times=folder/('holdout-times.txt' if held else 'timestamps.txt')
    assert raw.is_file() and times.is_file(),name
    outputs={};record={}
    for version in ['baseline','candidate']:
        dest=outroot/name/version;dest.mkdir(parents=True,exist_ok=True)
        assert not (dest/'metrics.json').exists(),'Already finished'
        cmd=[str(executables[version]),str(root/'build/opt1/absent-runtime'),str(raw),str(times),str(dest),'native',str(834167 if held else 417083),fmt,str(w),str(h),'1920','0','4','medium']
        with (dest/'run.log').open('w') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
        file=dest/'output.nv12'; digest=hashlib.sha256()
        with file.open('rb') as f:
            for chunk in iter(lambda:f.read(8*1024*1024),b''):digest.update(chunk)
        pairs=list(csv.DictReader((dest/'pairs.csv').open()));dur=[float(r['completed_ms']) for r in pairs[5:]];dur=sorted(dur)
        record[version]={'sha256':digest.hexdigest(),'frames':file.stat().st_size//(w*h*3//2),'mean_ms':statistics.mean(dur),'p95_ms':dur[int((len(dur)-1)*.95)],'max_ms':max(dur),'first_ms':float(pairs[0]['completed_ms']),'pairs':len(pairs)}
        outputs[version]=(file,pairs,(dest/'output.csv').read_bytes())
    record['pixels_equal']=record['baseline']['sha256']==record['candidate']['sha256']
    record['timing_equal']=outputs['baseline'][2]==outputs['candidate'][2]
    keys=['previous_pts','current_pts','phase_pts','scene_cut','repeated_mask','identical_skipped','grid','cost_buffers']
    record['decisions_equal']=[[r[k] for k in keys] for r in outputs['baseline'][1]]==[[r[k] for k in keys] for r in outputs['candidate'][1]]
    (outroot/name/'metrics.json').write_text(json.dumps(record,indent=2))
    summary[name]=record;(outroot/'summary.json').write_text(json.dumps(summary,indent=2))
    print(name,json.dumps(record),flush=True)
    assert record['pixels_equal'] and record['timing_equal'] and record['decisions_equal'],'Video regression; raw evidence retained'
    for file,_,_ in outputs.values():
        assert file.resolve().is_relative_to(outroot) and file.name=='output.nv12' and not file.is_symlink()
        file.unlink()
