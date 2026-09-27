# Exports COCO detectors to ONNX in the layout the parking app decodes.
#
# The detection head is cut before the box decoding: the KPU runs the network
# up to the last convolutions (plus sigmoid), the CPU decodes only the cells
# above the threshold. Every output is NHWC, one tensor per stride (8, 16, 32):
#
#   family "yolov5"  (anchor based, yolov5 v7.0 repo)
#       [1, H, W, 3 * (5 + nc)]   sigmoid of everything, channel = anchor * (5 + nc) + k
#   family "yolov8"  (anchor free, DFL box: yolov8, yolo11, yolov5u)
#       [1, H, W, 64 + nc]        64 raw DFL logits (4 sides x 16 bins), then sigmoid class scores
#
# Next to <id>.onnx goes <id>.json with what the app needs to decode it.
#
# Runs in the k510-model-export image (torch, ultralytics, yolov5 v7.0 in /opt/yolov5).
import argparse
import json
import os
import sys
import types

import numpy as np
import onnx
import onnxsim
import torch

from decode import decode, letterbox, draw


def export_yolov5(name, size):
    sys.path.insert(0, '/opt/yolov5')
    from models.experimental import attempt_load

    model = attempt_load(name + '.pt', device='cpu', inplace=True, fuse=True).float().eval()
    det = model.model[-1]

    def forward(self, x):
        return [self.m[i](x[i]).sigmoid().permute(0, 2, 3, 1) for i in range(self.nl)]

    det.forward = types.MethodType(forward, det)
    anchors = (det.anchors * det.stride.view(-1, 1, 1)).tolist()
    names = model.names if isinstance(model.names, list) else [model.names[i] for i in sorted(model.names)]
    meta = {'family': 'yolov5', 'anchors': anchors}
    return model, names, meta


def export_yolov8(name, size):
    from ultralytics import YOLO

    model = YOLO(name + '.pt').model.fuse().float().eval()
    det = model.model[-1]
    assert det.reg_max == 16, det.reg_max

    def forward(self, x):
        return [torch.cat([self.cv2[i](x[i]), self.cv3[i](x[i]).sigmoid()], 1).permute(0, 2, 3, 1)
                for i in range(self.nl)]

    det.forward = types.MethodType(forward, det)
    names = [model.names[i] for i in sorted(model.names)]
    meta = {'family': 'yolov8', 'reg_max': 16}
    return model, names, meta


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--name', required=True, help='yolov5n, yolov8s, yolo11n, ...')
    ap.add_argument('--size', type=int, default=320)
    ap.add_argument('--out', default='onnx')
    ap.add_argument('--test', nargs='*', default=[], help='images to run through onnxruntime as a check')
    args = ap.parse_args()

    anchor_based = args.name.startswith('yolov5') and not args.name.endswith('u')
    model, names, meta = (export_yolov5 if anchor_based else export_yolov8)(args.name, args.size)
    for p in model.parameters():
        p.requires_grad_(False)

    mid = f'{args.name}_{args.size}'
    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, mid + '.onnx')
    dummy = torch.zeros(1, 3, args.size, args.size)
    torch.onnx.export(model, dummy, path, opset_version=12, input_names=['images'],
                      output_names=['p3', 'p4', 'p5'], do_constant_folding=True)
    m, ok = onnxsim.simplify(onnx.load(path))
    assert ok
    onnx.save(m, path)

    meta.update({'id': mid, 'input': args.size, 'strides': [8, 16, 32], 'labels': names})
    with open(os.path.join(args.out, mid + '.json'), 'w') as f:
        json.dump(meta, f, ensure_ascii=False, indent=1)

    import onnxruntime as ort
    sess = ort.InferenceSession(path, providers=['CPUExecutionProvider'])
    print(mid, 'outputs', [o.shape for o in sess.get_outputs()])
    for img in args.test:
        x, geom = letterbox(img, args.size)
        outs = sess.run(None, {'images': x[None].astype(np.float32) / 255})
        boxes = decode(outs, meta, geom, 0.3)
        print(' ', os.path.basename(img), [(names[b[5]], round(b[4], 2)) for b in boxes][:12])
        draw(img, boxes, names, geom, os.path.join(args.out, f'{mid}_{os.path.basename(img)}'))


if __name__ == '__main__':
    main()
