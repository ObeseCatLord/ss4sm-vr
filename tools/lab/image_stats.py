"""Frame-to-frame stability (mean abs difference) and sharpness (Laplacian variance) of a still-camera capture
series, on the centre region (the HUD and hands stay out)."""
import sys
import numpy as np
from PIL import Image

def load(p):
    a = np.asarray(Image.open(p).convert('L'), dtype=np.float32)
    h, w = a.shape
    return a[h // 8: h // 2, w // 4: 3 * w // 4]

frames = [load(p) for p in sys.argv[1:]]
diff = np.mean([np.abs(a - b).mean() for a, b in zip(frames, frames[1:])])
lap = lambda a: (a[1:-1, 1:-1] * 4 - a[:-2, 1:-1] - a[2:, 1:-1] - a[1:-1, :-2] - a[1:-1, 2:]).var()
print(f"temporal diff {diff:.3f}  sharpness {np.mean([lap(f) for f in frames]):.1f}  ({len(frames)} frames)")
