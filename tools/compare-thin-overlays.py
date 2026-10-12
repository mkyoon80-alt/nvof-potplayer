"""NVIDIA hardware regression against independent analytic images.

Requires NumPy/OpenCV and two video_x2_repro executables. Covers stationary,
moving, fading, short and occluded strokes on a moving background. This is
not a quality score for arbitrary real video. Run with PotPlayer closed.
"""
import argparse
import csv
import ctypes
import json
from pathlib import Path
import subprocess
import sys

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'build/analysis-pylibs'))
import cv2

W, H = 960, 540
Y, X = np.mgrid[:H, :W].astype(np.float32)
KINDS = ['vertical', 'horizontal', 'bright', 'faint', 'fast', 'slow',
         'fade', 'short', 'parallel', 'texture', 'occluder', 'smallpan', 'along']


def fixture(t, kind):
    qx, qy = X - 19*t, Y - 3*t
    if kind == 'smallpan':
        qx, qy = X - .5*t, Y - .25*t
    if kind == 'along':
        qx, qy = X, Y - 12*t
    background = (155 + 22*np.sin(qx*.037 + np.sin(qy*.031)*1.4)
                  + 17*np.cos(qy*.055 + qx*.013) + 9*np.sin(qx*.089-qy*.091))
    normal, center, opacity, value = X, 480.3, .55, 35
    if kind == 'horizontal':
        normal, center = Y, 270.3
    if kind == 'bright':
        value = 230
    if kind == 'faint':
        opacity = .18
    if kind == 'fast':
        center += 8*t
    if kind == 'slow':
        center += .25*t
    if kind == 'fade':
        opacity *= max(0, 1-t*.12)
    alpha = opacity*np.clip(1.6-np.abs(normal-center), 0, 1)
    if kind == 'short':
        alpha *= (Y > 230) & (Y < 300)
    if kind == 'texture':
        alpha *= 0
    if kind == 'parallel':
        alpha = opacity*np.clip(1.6-np.abs(np.remainder(X-5*t+12, 24)-12), 0, 1)
    image = background*(1-alpha) + value*alpha
    if kind == 'occluder':
        cover = np.clip(38-np.hypot(X-395-24*t, (Y-270)*.7), 0, 1)
        image = image*(1-cover) + (212+12*np.sin(X*.15+Y*.02))*cover
    raw = np.full((H*3//2, W), 128, np.uint8)
    raw[:H] = np.clip(np.rint(image), 16, 235).astype(np.uint8)
    return raw


def require_idle():
    result = subprocess.run(['powershell', '-NoProfile', '-Command',
        "@(Get-Process -ErrorAction Stop | Where-Object { $_.ProcessName -match "
        "'PotPlayer|repro|native_motion|native_synthesis|gpu_interop' }).Count"],
        capture_output=True, text=True, check=True)
    if result.stdout.strip() != '0':
        raise RuntimeError('Close PotPlayer and other GPU tests before running this test')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    assert output.is_relative_to(ROOT/'build') and not output.exists()
    output.mkdir(parents=True)
    ctypes.windll.kernel32.SetErrorMode(0x8003)
    executables = {'baseline': args.baseline.resolve(), 'candidate': args.candidate.resolve()}
    summary, failures = {}, []
    valid = (Y > 100) & (Y < H-100) & (X > 100) & (X < W-100)
    for kind in KINDS:
        case = output/kind
        case.mkdir()
        raw, times = case/'input.nv12', case/'times.txt'
        with raw.open('wb') as f:
            for i in range(9):
                f.write(fixture(i, kind).tobytes())
        times.write_text('\n'.join(str(i*417083) for i in range(9)))
        records, shots, timelines, decisions = {}, {}, {}, {}
        for label, executable in executables.items():
            require_idle()
            dest = case/label
            dest.mkdir()
            cmd = [executable, ROOT/'build/absent-runtime', raw, times, dest,
                   'native', 417083, 'nv12', W, H, W, 0, 4, 'medium', 'reuse', 120]
            with (dest/'run.log').open('w') as log:
                subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT,
                               check=True, timeout=120)
            result = np.memmap(dest/'output.nv12', np.uint8, mode='r').reshape(-1, H*3//2, W)
            errors = []
            for i in range(8):
                assert np.array_equal(result[i*5], fixture(i, kind)), 'Original changed'
                for phase in range(1, 5):
                    truth = fixture(i+phase/5, kind)[:H].astype(np.float32)
                    delta = result[i*5+phase, :H].astype(np.float32)-truth
                    distance = np.abs(Y-270.3) if kind == 'horizontal' else np.abs(X-480.3)
                    core, band = distance < 2, distance < 20
                    errors.append([float(np.mean(delta[valid]**2)),
                                   float(np.mean(delta[core & valid]**2)),
                                   float(np.mean(delta[band & ~core & valid]**2))])
            records[label] = dict(zip(['full_mse', 'line_mse', 'adjacent_mse'],
                                      np.mean(errors, axis=0).tolist()))
            timelines[label] = (dest/'output.csv').read_bytes()
            keys = ['previous_pts', 'current_pts', 'phase_pts', 'scene_cut', 'repeated_mask']
            with (dest/'pairs.csv').open() as f:
                decisions[label] = [[r[k] for k in keys] for r in csv.DictReader(f)]
            shots[label] = result[22].copy()
            del result
            (dest/'output.nv12').unlink()  # Only this invocation's disposable output.
        assert timelines['baseline'] == timelines['candidate'], 'Timeline changed'
        assert decisions['baseline'] == decisions['candidate'], 'Pair decisions changed'
        base, candidate = records['baseline'], records['candidate']
        for key in base:
            if candidate[key] > base[key]*1.03 + .01:
                failures.append(f'{kind}/{key} regressed')
        if kind in ['vertical', 'bright', 'faint', 'fade']:
            if candidate['line_mse'] > base['line_mse']*.5:
                failures.append(f'{kind}: insufficient stationary-line improvement')
        images = []
        for label, frame in [('truth', fixture(4.4, kind)), *shots.items()]:
            im = cv2.cvtColor(frame, cv2.COLOR_YUV2BGR_NV12)[150:420, 400:560]
            im = cv2.copyMakeBorder(im, 25, 0, 0, 0, cv2.BORDER_CONSTANT, value=(25, 25, 25))
            cv2.putText(im, label, (4, 18), 0, .45, (255, 255, 255), 1)
            images.append(im)
        cv2.imwrite(str(case/'comparison.png'), np.hstack(images))
        summary[kind] = records
        (output/'summary.json').write_text(json.dumps(summary, indent=2))
        print(kind, json.dumps(records), flush=True)
    (output/'result.json').write_text(json.dumps({'passed': not failures, 'failures': failures}, indent=2))
    if failures:
        raise AssertionError('; '.join(failures))
    print('PASS thin overlays: stationary quality, moving/occluded controls, originals and timing')


if __name__ == '__main__':
    main()
