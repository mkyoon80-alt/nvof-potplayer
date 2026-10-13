"""Compare two native engines against analytic, independently rendered truth.

No source media is required. Measures visible background, occlusion boundaries,
and moving detail separately at x2 and x5. Output is restricted to a new folder
under build/. Run with players and other GPU workloads closed. Requires NumPy.
"""
import argparse
import csv
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'build/analysis-pylibs'))
import numpy as np

W, H, COUNT = 960, 540, 7
Y, X = np.mgrid[:H, :W].astype(np.float32)
KINDS = ('closing-gap', 'opening-gap', 'moving-sliver', 'bright-object',
         'repeated-background', 'curved-occluder', 'translation', 'fixed-line')
VALID = (X >= 64) & (X < W-64) & (Y >= 48) & (Y < H-48)


def scene(t, kind):
    # Images come from geometry at continuous time, never from endpoint warps.
    qx, qy = X-13*t, Y-3*t
    background = (142 + 22*np.sin(qx*.067 + .9*np.sin(qy*.037))
                  + 19*np.cos(qy*.093+qx*.017) + 8*np.sin(qx*.19-qy*.13))
    if kind == 'repeated-background':
        background = 176-100*np.exp(-np.remainder(qx+12, 24)**2/8)
        background += 14*np.sin(qy*.08)+10*np.cos(qx*.029+qy*.017)
    cover = np.zeros_like(X)
    distance = np.full_like(X, -1000)
    detail = np.zeros_like(X, dtype=bool)
    if kind in ('closing-gap', 'opening-gap', 'repeated-background'):
        gap = 90-12*t if kind != 'opening-gap' else 18+12*t
        center = 480+3*t
        distance = np.abs(X-center)-gap*.5
        cover = np.clip(distance+.5, 0, 1)
        # Two opaque faces leave a narrow moving slit, with their own texture.
        local = X-center+np.where(X < center, gap*.5, -gap*.5)
        foreground = 49+7*np.sin(local*.051+Y*.079)
    elif kind == 'curved-occluder':
        ox, oy = X-430-8*t, Y-250-7*t
        distance = 105-np.hypot(ox, oy*.85)
        cover = np.clip(distance+.5, 0, 1)
        foreground = 51+11*np.sin(ox*.071)*np.cos(oy*.053)
    elif kind in ('moving-sliver', 'bright-object'):
        width = 2.5 if kind == 'moving-sliver' else 24
        ox, oy = X-340-19*t, Y-240+2*t
        distance = np.minimum(width/2-np.abs(ox-.16*oy), 80-np.abs(oy))
        cover = np.clip(distance+.5, 0, 1)
        foreground = np.full_like(X, 28 if kind == 'moving-sliver' else 231)
        detail = distance > -3
    elif kind == 'fixed-line':
        distance = 1.4-np.abs(X-480.3)
        cover = .55*np.clip(distance, 0, 1)
        foreground = np.full_like(X, 35)
        detail = distance > -2
    else:
        foreground = np.zeros_like(X)
    luma = np.clip(np.rint(background*(1-cover)+foreground*cover), 16, 235).astype(np.uint8)
    raw = np.full((H*3//2, W), 128, dtype=np.uint8)
    raw[:H] = luma
    masks = {'full': VALID, 'boundary': VALID & (np.abs(distance) < 10),
             'visible_background': VALID & (cover == 0), 'detail': VALID & detail}
    return raw, masks


def require_idle():
    # A failed enumeration is never treated as an idle desktop. No WMI privilege
    # is needed, and no process or user setting is changed.
    query = ("@(Get-Process -ErrorAction Stop | Where-Object { $_.ProcessName -match "
             "'PotPlayer|repro|native_|gpu_|directshow|p010_video|cpu_upload_gpu|cpu10_sequence|ffmpeg|blender|FurMark|3DMark' }).Count")
    p = subprocess.run(['powershell', '-NoProfile', '-Command', query],
                       capture_output=True, text=True, check=True, timeout=20)
    if p.stdout.strip() != '0':
        raise RuntimeError('Player or another GPU test is running; no test started')
    p = subprocess.run(['nvidia-smi', '--query-compute-apps=process_name', '--format=csv,noheader'],
                       capture_output=True, text=True, check=True, timeout=20)
    if any(s in p.stdout.lower() for s in ('python', 'comfy', 'blender', 'ffmpeg', 'benchmark')):
        raise RuntimeError('A compute workload is attached to the GPU; no test started')


def run(executable, source, times, dest, multiple):
    require_idle()
    dest.mkdir()
    env = {k.upper(): v for k, v in os.environ.items()}
    env['LOCALAPPDATA'] = str(dest/'localappdata')
    Path(env['LOCALAPPDATA']).mkdir()
    cmd = [executable, ROOT/'build/absent-runtime', source, times, dest, 'native',
           417083, 'nv12', W, H, W, 0, 4, 'medium', 'reuse', 0 if multiple == 2 else 120]
    with (dest/'run.log').open('w') as log:
        subprocess.run(list(map(str, cmd)), cwd=ROOT, env=env, stdout=log,
                       stderr=subprocess.STDOUT, check=True, timeout=180)


def compare(args):
    output = args.output.resolve()
    if not output.is_relative_to(ROOT/'build') or output.exists():
        raise ValueError('Choose a NEW output directory under build/')
    executables = {'baseline': args.baseline.resolve(), 'candidate': args.candidate.resolve()}
    for exe in executables.values():
        if not exe.is_file():
            raise FileNotFoundError(exe)
    output.mkdir(parents=True)
    ctypes.windll.kernel32.SetErrorMode(0x8003)
    manifest = {k: {'path': str(v), 'sha256': hashlib.sha256(v.read_bytes()).hexdigest()}
                for k, v in executables.items()}
    (output/'manifest.json').write_text(json.dumps(manifest, indent=2))
    summary, failures = {}, []
    for kind in args.cases:
        case = output/kind
        case.mkdir()
        source, times = case/'input.nv12', case/'timestamps.txt'
        with source.open('wb') as f:
            for i in range(COUNT):
                f.write(scene(i, kind)[0].tobytes())
        times.write_text('\n'.join(str(i*417083) for i in range(COUNT)))
        for multiple in args.multiples:
            records, timelines, decisions = {}, {}, {}
            for label, exe in executables.items():
                dest = case/f'{label}-x{multiple}'
                run(exe, source, times, dest, multiple)
                result = np.memmap(dest/'output.nv12', np.uint8, mode='r').reshape(-1, H*3//2, W)
                expected_count = COUNT*multiple
                if len(result) != expected_count:
                    raise AssertionError(f'Unexpected output count: {len(result)} != {expected_count}')
                metrics = {k: [0., 0] for k in ('full', 'boundary', 'visible_background', 'detail')}
                for i in range(COUNT):
                    if not np.array_equal(result[i*multiple], scene(i, kind)[0]):
                        raise AssertionError('Original pixels changed')
                    if i == COUNT-1:
                        continue  # Last padded output group has no second endpoint.
                    for phase in range(1, multiple):
                        truth, masks = scene(i+phase/multiple, kind)
                        delta = result[i*multiple+phase, :H].astype(np.float64)-truth[:H]
                        for key, mask in masks.items():
                            metrics[key][0] += float(np.sum(delta[mask]**2))
                            metrics[key][1] += int(mask.sum())
                records[label] = {k: v[0]/v[1] if v[1] else None for k, v in metrics.items()}
                timelines[label] = (dest/'output.csv').read_bytes()
                keys = ('previous_pts', 'current_pts', 'phase_pts', 'scene_cut', 'repeated_mask', 'identical_skipped')
                with (dest/'pairs.csv').open() as f:
                    rows = list(csv.DictReader(f))
                decisions[label] = [[r[k] for k in keys] for r in rows]
                if any(int(r['scene_cut']) or int(r['repeated_mask']) or int(r['identical_skipped']) for r in rows):
                    failures.append(f'{kind}/x{multiple}/{label}: held or skipped a synthetic pair')
                # Retain a representative exact truth and raw result for inspection.
                np.save(dest/'sample-luma.npy', result[2*multiple+1, :H].copy())
                del result
                if not args.keep_raw:
                    (dest/'output.nv12').unlink()  # Only newly created disposable test output.
            if timelines['baseline'] != timelines['candidate'] or decisions['baseline'] != decisions['candidate']:
                failures.append(f'{kind}/x{multiple}: timing or pair decisions changed')
            for key, value in records['baseline'].items():
                if value is not None and records['candidate'][key] > value*1.03+.01:
                    failures.append(f'{kind}/x{multiple}/{key}: >3%+.01 MSE regression')
            summary[f'{kind}/x{multiple}'] = records
            (output/'summary.json').write_text(json.dumps(summary, indent=2))
            print(kind, multiple, json.dumps(records), flush=True)
    verdict = {'no_material_regression': not failures, 'failures': failures,
               'completed_case_multiples': len(summary), 'all_originals_exact': True,
               'timing_and_decisions_exact': not any('timing or pair decisions' in f for f in failures),
               'note': 'Passing is a regression gate, not evidence of improvement or real-video quality.'}
    (output/'result.json').write_text(json.dumps(verdict, indent=2))
    return 0 if not failures else 1


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', type=Path, required=True)
    p.add_argument('--candidate', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--cases', nargs='+', choices=KINDS, default=list(KINDS))
    p.add_argument('--multiples', nargs='+', type=int, choices=(2, 5), default=[2, 5])
    p.add_argument('--keep-raw', action='store_true')
    sys.exit(compare(p.parse_args()))
