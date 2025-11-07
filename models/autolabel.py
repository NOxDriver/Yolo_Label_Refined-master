#!/usr/bin/env python3
# Yolo_Label/models/autolabel.py
# Minimal ONNXRuntime YOLOv11 inference to write YOLO txt labels for a single image
import sys, os, json, re
import onnxruntime as ort
import numpy as np
import cv2

from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent

# Usage: autolabel.py <image_path> <label_path> <names_path>
# names_path is a .names or .txt where each line is class name
if len(sys.argv) < 4:
    print("usage: autolabel.py <image> <label_txt> <names_file>", file=sys.stderr)
    sys.exit(2)

img_path, label_path, names_path = sys.argv[1], sys.argv[2], sys.argv[3]
conf_thres = 0.35
iou_thres = 0.60
imgsz = 640  # adjust if your model expects another input size

with open(names_path, "r", encoding="utf-8") as f:
    names = [ln.strip() for ln in f if ln.strip()]

def _pick_latest_onnx(base: Path, pattern: str = "*.onnx") -> Path:
    """
    Choose an ONNX model from `base`:
      - prefer names containing 'best' or 'latest'
      - then prefer dates in the filename like YYYY_MM_DD or YYYY-MM-DD
      - then most-recent mtime
      - then largest size (as a weak tie-breaker)
    """
    cands = list(base.glob(pattern))
    if not cands:
        raise FileNotFoundError(f"No ONNX models found in {base}")

    def score(p: Path):
        name = p.name.lower()
        prefer_best = ("best" in name) or ("latest" in name)
        m = re.search(r'(\d{4})[_-]?(\d{2})[_-]?(\d{2})', name)  # e.g. 2025_08_09
        date_key = (int(m.group(1)), int(m.group(2)), int(m.group(3))) if m else (0,0,0)
        stat = p.stat()
        return (prefer_best, date_key, stat.st_mtime, stat.st_size)

    return max(cands, key=score)


def letterbox(im, new_shape=640):
    h, w = im.shape[:2]
    r = min(new_shape / h, new_shape / w)
    nh, nw = int(round(h * r)), int(round(w * r))
    im_resized = cv2.resize(im, (nw, nh), interpolation=cv2.INTER_LINEAR)
    canvas = np.full((new_shape, new_shape, 3), 114, dtype=np.uint8)
    top = (new_shape - nh) // 2
    left = (new_shape - nw) // 2
    canvas[top:top+nh, left:left+nw] = im_resized
    return canvas, r, left, top

def nms(boxes, scores, iou=0.7):
    if len(boxes)==0: return []
    boxes = np.array(boxes, dtype=np.float32)
    scores = np.array(scores, dtype=np.float32)
    x1,y1,x2,y2 = boxes.T
    areas = (x2-x1+1)*(y2-y1+1)
    order = scores.argsort()[::-1]
    keep=[]
    while order.size>0:
        i=order[0]; keep.append(i)
        xx1=np.maximum(x1[i], x1[order[1:]])
        yy1=np.maximum(y1[i], y1[order[1:]])
        xx2=np.minimum(x2[i], x2[order[1:]])
        yy2=np.minimum(y2[i], y2[order[1:]])
        w=np.maximum(0.0, xx2-xx1+1)
        h=np.maximum(0.0, yy2-yy1+1)
        inter=w*h
        ovr=inter/(areas[i]+areas[order[1:]]-inter)
        inds=np.where(ovr<=iou)[0]
        order=order[inds+1]
    return keep

# --- Resolve MODEL_PATH with overrides and sensible fallbacks ---
# Priority 1: CLI arg (4th arg): autolabel.py <image> <label> <names> <model>
model_override = sys.argv[4] if len(sys.argv) >= 5 else None

# Priority 2: env var
env_override = os.environ.get("YOLO_MODEL_PATH")

MODEL_PATH = None
if model_override:
    MODEL_PATH = str(Path(model_override).expanduser().resolve())
elif env_override:
    MODEL_PATH = str(Path(env_override).expanduser().resolve())
else:
    # Try current folder first (where autolabel.py lives)
    try:
        MODEL_PATH = str(_pick_latest_onnx(SCRIPT_DIR))
    except FileNotFoundError:
        # Then try a nested 'models' folder (works if script sits one level up)
        MODEL_PATH = str(_pick_latest_onnx(SCRIPT_DIR / "models"))

print(f"[autolabel] Using model: {MODEL_PATH}")

# Load & preprocess
img0 = cv2.imread(img_path)
if img0 is None:
    sys.exit(0)
img, r, left, top = letterbox(img0, imgsz)
inp = img[:, :, ::-1].transpose(2,0,1) / 255.0
inp = np.expand_dims(inp.astype(np.float32), 0)

# Infer

if not Path(MODEL_PATH).exists():
    print(f"[autolabel] Model not found: {MODEL_PATH}", file=sys.stderr)
    sys.exit(1)

# Infer
session = ort.InferenceSession(MODEL_PATH, providers=["CPUExecutionProvider"])
inp_name = session.get_inputs()[0].name
out = session.run(None, {inp_name: inp})[0]  # typically (1, F, N) or (1, N, F) or (N, F)

# Normalize to (N, F) where F = 4 + num_classes
o = out
if o.ndim == 3:
    o = o[0]  # drop batch

num_classes = len(names)
feat = 4 + num_classes

if o.ndim != 2:
    raise RuntimeError(f"Unexpected output ndim={o.ndim}, shape={o.shape}")

# If features are first, transpose to (N, F)
if o.shape[0] == feat:
    o = o.transpose(1, 0)
elif o.shape[1] == feat:
    pass  # already (N, F)
else:
    raise RuntimeError(f"Unexpected output shape {o.shape}; can't find feature dim {feat}")

# Split into boxes and class scores
boxes = o[:, :4]          # xywh in pixels of letterboxed input
cls_scores = o[:, 4:]     # per-class scores (shape: 8400, 60)

# Get best score and class per detection
sc = cls_scores.max(axis=1)
cl = cls_scores.argmax(axis=1)

# Center X/Y, width, height in pixels (letterboxed space)
cx, cy, w, h = boxes.T



keep = sc >= conf_thres
cx, cy, w, h, sc, cl = cx[keep], cy[keep], w[keep], h[keep], sc[keep], cl[keep].astype(int)

# Convert to xyxy in padded (letterbox) coordinates then back to original
# Convert to xyxy in *pixels* of the letterboxed input (no extra scaling)
xyxy = np.stack([cx - w/2, cy - h/2, cx + w/2, cy + h/2], axis=1)

# Undo padding/scale back to original image coordinates
xyxy[:, [0, 2]] -= left
xyxy[:, [1, 3]] -= top
xyxy = xyxy / r

# Clip to image size
H, W = img0.shape[:2]
xyxy[:, 0] = np.clip(xyxy[:, 0], 0, W - 1)
xyxy[:, 2] = np.clip(xyxy[:, 2], 0, W - 1)
xyxy[:, 1] = np.clip(xyxy[:, 1], 0, H - 1)
xyxy[:, 3] = np.clip(xyxy[:, 3], 0, H - 1)

# Simple NMS per class (same as before)
final = []
valid_classes = set(range(len(names)))
for c in np.unique(cl):
    if c not in valid_classes:
        continue
    m = cl == c
    keep_idx = nms(xyxy[m], sc[m], iou=iou_thres)
    for k in keep_idx:
        box = xyxy[m][k]
        conf = float(sc[m][k])
        final.append((int(c), box, conf))

# Write YOLO txt (class cx cy w h) normalized to [0,1]
out_lines = []
conf_records = []
for entry in final:
    # entry is (cls, box, conf) OR (cls, box)
    if len(entry) == 3:
        c, (x1, y1, x2, y2), conf = entry
    else:
        c, (x1, y1, x2, y2) = entry
        conf = None

    bw = x2 - x1
    bh = y2 - y1
    cx_abs = x1 + bw / 2.0
    cy_abs = y1 + bh / 2.0

    # write normalized YOLO (cx, cy, w, h)
    out_lines.append(f"{int(c)} {cx_abs/W:.6f} {cy_abs/H:.6f} {bw/W:.6f} {bh/H:.6f}")

    if conf is not None:
        conf_records.append({"cls": int(c), "conf": float(conf)})



if out_lines:
    os.makedirs(os.path.dirname(label_path), exist_ok=True)
    with open(label_path, "w") as f:
        f.write("\n".join(out_lines))
    # optional: write confidences for the UI to show on first load
    if conf_records:
        with open(label_path + ".json", "w") as jf:
            json.dump(conf_records, jf)
