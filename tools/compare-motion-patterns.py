"""Compare moving patterns and textured occlusions against independent truth.

Uses the same guarded runner and regression checks as compare-occlusion.py.
Images are rendered from continuous-time geometry, not from flow or endpoint
warps. Each case separates foreground, boundary, and visible background error.
No private media is needed. Requires NumPy; outputs stay in a NEW build/ folder.
"""
import argparse
import importlib.util
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('analytic', ROOT/'tools/compare-occlusion.py')
analytic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analytic)
np, X, Y, W, H = analytic.np, analytic.X, analytic.Y, analytic.W, analytic.H

VELOCITIES = {
    'slow': (19, 0), 'fast': (57, 0), 'diagonal': (31, 13),
    'mid': (35, 0), 'high': (43, 0), 'max': (73, 0),
    'flat': (43, 0), 'fine': (43, 0),
    'medium-fast': (47, 0), 'faster': (49, 0),
    'flat-fast': (57, 0), 'fine-fast': (57, 0),
}
KINDS = tuple('static-texture-'+kind for kind in VELOCITIES)
KINDS += ('moving-bars-slow', 'moving-bars-fast', 'moving-bars-reverse')


def scene(t, kind):
    background = (132 + 22*np.sin(X*.071+Y*.039) + 18*np.cos(X*.139-Y*.097)
                  + 19*np.sin(X*.39+Y*.28) + 12*np.cos(X*.57-Y*.63))
    if kind.startswith('static-texture'):
        variant = kind[len('static-texture-'):]
        vx, vy = VELOCITIES[variant]
        ox, oy = X-(240+vx*t), Y-(235+vy*t)
        # Independent opaque geometry: torso plus round head and moving cloth.
        distance = np.maximum(np.minimum(62-np.abs(ox), 104-np.abs(oy)),
                              43-np.hypot(ox, oy+125))
        cover = np.clip(distance+.5, 0, 1)
        foreground = 74+38*np.sin(ox*.27+oy*.06)+8*np.cos(oy*.13)
        if variant.startswith('flat'):
            foreground = np.full_like(X, 60.)
        if variant.startswith('fine'):
            foreground = 88+36*np.sin(ox*.2)*np.cos(oy*.31)+21*np.cos(ox*.17+oy*.35)
    else:
        vx = 17 if kind.endswith('slow') else 45 if kind.endswith('fast') else -37
        q = X-vx*t
        distance = np.minimum(Y-110, 430-Y)
        cover = np.clip(distance+.5, 0, 1)
        bar = np.exp(-(np.remainder(q+20, 40)-20)**2/25)
        foreground = 177-117*bar+6*np.sin(Y*.028)
        # End markings disambiguate the repeated panels without using a flow
        # estimator or defining truth in terms of the repair implementation.
        trim = (np.abs(Y-130)<9) | (np.abs(Y-410)<9)
        marks = 60+35*np.sin(q*.017)+29*np.cos(q*.053)+25*np.sin(q*.089)
        foreground = np.where(trim, marks, foreground)
        foreground += 14*np.exp(-((Y-165)/10)**2)*np.sin(q*.011)
    luma = np.clip(np.rint(background*(1-cover)+foreground*cover), 16, 235).astype(np.uint8)
    raw = np.full((H*3//2, W), 128, np.uint8)
    raw[:H] = luma
    masks = {
        'full': analytic.VALID,
        'boundary': analytic.VALID & (np.abs(distance)<18),
        'visible_background': analytic.VALID & (cover==0),
        'detail': analytic.VALID & (cover>0.5),
    }
    return raw, masks


if __name__ == '__main__':
    analytic.scene = scene
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cases', nargs='+', choices=KINDS, default=list(KINDS))
    parser.add_argument('--multiples', nargs='+', type=int, choices=(2, 5), default=[2, 5])
    parser.add_argument('--keep-raw', action='store_true')
    sys.exit(analytic.compare(parser.parse_args()))
