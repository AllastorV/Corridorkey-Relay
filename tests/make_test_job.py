import json
import sys
from pathlib import Path

import cv2
import numpy as np

video, folder = sys.argv[1], Path(sys.argv[2])
scale = int(sys.argv[3]) if len(sys.argv) > 3 else 1
hint = sys.argv[4] if len(sys.argv) > 4 else "none"
cap = cv2.VideoCapture(video)
fps = cap.get(cv2.CAP_PROP_FPS) or 24.0
(folder / "in").mkdir(parents=True, exist_ok=True)
count = 0
while True:
    ok, bgr = cap.read()
    if not ok:
        break
    full_h, full_w = bgr.shape[:2]
    w, h = full_w // scale, full_h // scale
    rgb = cv2.cvtColor(cv2.resize(bgr, (w, h), interpolation=cv2.INTER_AREA), cv2.COLOR_BGR2RGB)
    (rgb.astype(np.float32) / 255.0).astype(np.float16).tofile(folder / "in" / f"{count:05d}.raw")
    count += 1
settings = {"linear": False, "scale": scale, "memory": {"auto": True, "chunk": 8, "low_vram": False},
            "hint": {"mode": hint, "color": [0.0, 0.694, 0.251], "similarity": 0.12, "softness": 0.08,
                     "quality": 0.95, "source": "chroma", "reference": int(sys.argv[5]) if len(sys.argv) > 5 else 0},
            "carry_hint": True, "carry_quality": 0.95}
job = {"width": w, "height": h, "full_w": full_w, "full_h": full_h, "x0": 0, "y0": 0, "count": count,
       "start": 0, "fps": fps, **settings, "settings": "test"}
(folder / "job.json").write_text(json.dumps(job), encoding="utf-8")
(folder / "collected").write_text("ok")
print(count, w, h)
