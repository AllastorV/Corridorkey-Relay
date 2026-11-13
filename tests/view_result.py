import json
import sys
from pathlib import Path

import cv2
import numpy as np

folder, index, out = Path(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
job = json.loads((folder / "job.json").read_text(encoding="utf-8"))
w, h = job["width"], job["height"]
rgba = np.fromfile(folder / "out" / f"{index:05d}.rgba", dtype=np.float16).reshape(h, w, 4).astype(np.float32)
fg, a = np.clip(rgba[..., :3], 0, 1), rgba[..., 3:4]
bg = np.zeros_like(fg)
bg[..., 0], bg[..., 2] = 1.0, 1.0
comp = fg * a + bg * (1 - a)
sheet = np.hstack([comp, np.repeat(a, 3, axis=2)])
cv2.imwrite(out, cv2.cvtColor((sheet * 255).astype(np.uint8), cv2.COLOR_RGB2BGR))
print(rgba.shape, float(a.min()), float(a.max()))
