# Reference decoder for the exported heads (same math as the C++ one in the app)
# and the input preparation that mimics what the ISP feeds the KPU.
import numpy as np
from PIL import Image, ImageDraw


def letterbox(path, n, aspect=0.75):
    """Image scaled to n x n*aspect (like ISP ds2) centered in an n x n gray square.
    Returns CHW uint8 RGB and the geometry to map boxes back."""
    img = Image.open(path).convert('RGB')
    vw, vh = n, int(n * aspect)
    small = np.asarray(img.resize((vw, vh), Image.BILINEAR))
    canvas = np.full((n, n, 3), 114, np.uint8)
    px, py = (n - vw) // 2, (n - vh) // 2
    canvas[py:py + vh, px:px + vw] = small
    return canvas.transpose(2, 0, 1).copy(), (vw, vh, px, py, img.width, img.height)


def _nms(boxes, thr):
    boxes = sorted(boxes, key=lambda b: -b[4])
    keep = []
    for b in boxes:
        ok = True
        for k in keep:
            if k[5] != b[5]:
                continue
            ix = max(0, min(b[2], k[2]) - max(b[0], k[0]))
            iy = max(0, min(b[3], k[3]) - max(b[1], k[1]))
            inter = ix * iy
            union = (b[2] - b[0]) * (b[3] - b[1]) + (k[2] - k[0]) * (k[3] - k[1]) - inter
            if union > 0 and inter / union > thr:
                ok = False
                break
        if ok:
            keep.append(b)
    return keep


def decode(outs, meta, geom, thr, nms=0.45):
    """outs: list of NHWC arrays (one per stride). Returns [x1, y1, x2, y2, score, cls]
    in original image pixels."""
    vw, vh, px, py, W, H = geom
    nc = len(meta['labels'])
    boxes = []
    for i, (o, s) in enumerate(zip(outs, meta['strides'])):
        o = np.asarray(o, np.float32).reshape(o.shape[1], o.shape[2], -1)
        gh, gw = o.shape[:2]
        gy, gx = np.mgrid[0:gh, 0:gw]
        if meta['family'] == 'yolov5':
            for a, (aw, ah) in enumerate(meta['anchors'][i]):
                r = o[..., a * (5 + nc):(a + 1) * (5 + nc)]
                sc = r[..., 5:] * r[..., 4:5]
                cls = sc.argmax(-1)
                best = sc.max(-1)
                for y, x in zip(*np.nonzero(best > thr)):
                    v = r[y, x]
                    cx = (v[0] * 2 - 0.5 + x) * s
                    cy = (v[1] * 2 - 0.5 + y) * s
                    w = (v[2] * 2) ** 2 * aw
                    h = (v[3] * 2) ** 2 * ah
                    boxes.append([cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, float(best[y, x]), int(cls[y, x])])
        else:
            rm = meta['reg_max']
            sc = o[..., 4 * rm:]
            cls = sc.argmax(-1)
            best = sc.max(-1)
            for y, x in zip(*np.nonzero(best > thr)):
                d = o[y, x, :4 * rm].reshape(4, rm)
                d = np.exp(d - d.max(-1, keepdims=True))
                d = (d / d.sum(-1, keepdims=True)) @ np.arange(rm)
                cx, cy = (x + 0.5) * s, (y + 0.5) * s
                boxes.append([cx - d[0] * s, cy - d[1] * s, cx + d[2] * s, cy + d[3] * s,
                              float(best[y, x]), int(cls[y, x])])
    out = []
    for b in _nms(boxes, nms):
        x1, y1, x2, y2 = [(b[0] - px) / vw * W, (b[1] - py) / vh * H, (b[2] - px) / vw * W, (b[3] - py) / vh * H]
        out.append([max(0, x1), max(0, y1), min(W, x2), min(H, y2), b[4], b[5]])
    return out


def draw(path, boxes, names, geom, out):
    img = Image.open(path).convert('RGB')
    d = ImageDraw.Draw(img)
    for x1, y1, x2, y2, s, c in boxes:
        d.rectangle([x1, y1, x2, y2], outline=(255, 200, 0), width=3)
        d.text((x1 + 3, y1 + 2), f'{names[c]} {s:.2f}', fill=(255, 255, 0))
    img.save(out, quality=85)
