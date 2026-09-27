# Detection models for the parking app

COCO object detectors (YOLOv5 n/s/m, YOLOv8 n/s; 320 and 640 input)
compiled for the K510 KPU with nncase 1.9 — the runtime version on the board.
They are built on the x86 build server, not by buildroot: the kmodels are
large and the toolchain (PyTorch, ultralytics, nncase) only runs on x86_64.

## What the model looks like

The detection head is cut before the box decoding (see `export.py`):
the KPU runs the network up to the last convolutions plus sigmoid, the app
decodes only the cells above the threshold (`src/kpu.cc`, reference in
`decode.py`). Two families:

| family   | models                    | output per stride (NHWC)                    |
|----------|---------------------------|---------------------------------------------|
| `yolov5` | yolov5 v7.0, SDK yolov5s  | `3 * (5 + nc)`: sigmoid xywh, obj, classes  |
| `yolov8` | yolov8 (and yolov5u)      | `64 + nc`: raw DFL box logits, sigmoid classes |

The input is what ISP ds2 delivers: uint8 planar RGB, `n x n`, image scaled
to `n x 3n/4` and centered in gray (114); `/255` runs inside the kmodel.

Each model is `<id>.kmodel` + `<id>.json` (family, input, strides, anchors,
labels). `bf16` keeps float accuracy; `uint8` is quantized on real frames
(board photos + COCO128), about half the size and 10–20 % faster.

## Building (on the build server)

```
/mnt/nvme0n1/k510/models/scripts/build_all.sh            # everything
/mnt/nvme0n1/k510/models/scripts/build_all.sh yolov8n:416  # one more
```

Work dir `/mnt/nvme0n1/k510/models`: `scripts/` (this directory, rsynced),
`photos/` (calibration frames from the board), `coco128/`, `onnx/`, `kmodel/`.
Docker images: `k510-model-export` (Dockerfile.export: torch, ultralytics,
yolov5 v7.0) and `k510-model-compile` (k510_env + nncase 1.9 wheels from
`dl/nncase_linux_runtime`). Every kmodel is run in the nncase simulator on
two test images; `kmodel/<id>.log` shows the detections next to the ONNX ones.

YOLO11 does not work with nncase 1.9 on the k510 target: bf16 compiles but
finds nothing in the simulator, uint8 crashes the compiler (its C2PSA
attention block: MatMul + Softmax). YOLOv8 is its closest working relative.

nncase 1.9 quirk: the k510 target fails with `bad optional access` unless
`dump_ir` and `dump_asm` are on (compile.py sets them).

## SDK models (package ai)

Besides the COCO detectors the app runs the stock Canaan kmodels from
`/app/ai/kmodel` (catalog in `src/model.cc`, decoders in `src/engine.cc`,
re-implemented from the package ai demos with the same thresholds and crops):

| id | what | stages |
|---|---|---|
| sdk_face_320 / 640 | faces + 5 points | RetinaFace |
| sdk_face_landmarks | faces + 106 points | RetinaFace → PFLD |
| sdk_face_expression | faces + emotion (8) | RetinaFace → FER |
| sdk_head_pose | faces + yaw/pitch/roll | RetinaFace → head pose |
| sdk_person_scrfd | people only, 640×480 | SCRFD |
| sdk_plates_320 / 640 | plates, 4 corners, text | LPD → LPRNet (Chinese plates) |
| sdk_hands | hands + 21 points | tiny YOLOv3 → squeezenet |
| sdk_openpose | skeletons, 18 points, many people | OpenPose + PAF grouping |
| sdk_simple_pose | people + 17 points | YOLOv5s → SimplePose |

Not wired up: face recognition and "self learning" (need enrolling samples),
3D face alignment (dense mesh), the ImageNet classifier of the hand demo.
The ISP AI output follows each model: size, and image centered (YOLO, hands)
or in the top-left corner (RetinaFace family), see `ModelInfo`.

## Installing on the board

Copy `kmodel/*.kmodel` + `*.json` to `/root/data/models/` (on the SD card).
The app also looks in `/app/parking/models` and `/root/sd/*/models`.

```
/app/parking/parking --models                    # what is installed
/app/parking/parking --bench [--image x.jpg] [ids]  # speed + detections, app stopped
```

Pick a model in the web UI (tab «Модели») or on the screen (Настройки
камер → Нейросеть); the app restarts with it. A model that fails to load
falls back to `sdk_yolov5s_320`.
