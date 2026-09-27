# Compiles an exported ONNX detector into a K510 kmodel with nncase 1.9 (the
# runtime version on the board) and checks it in the nncase simulator.
#
# The model takes the ISP output as is: uint8 planar RGB, n x n; the /255
# normalization runs inside the kmodel. bf16 keeps float accuracy, uint8 is
# post-training quantized on real frames (--calib), not on random noise.
#
# Runs in ghcr.io/kendryte/k510_env with the nncase 1.9 wheels installed.
import argparse
import glob
import json
import os
import random
import time

import numpy as np
import nncase

from decode import decode, letterbox


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--onnx', required=True)
    ap.add_argument('--quant', choices=['bf16', 'uint8'], default='bf16')
    ap.add_argument('--calib', nargs='*', default=[], help='calibration image globs (uint8 only)')
    ap.add_argument('--samples', type=int, default=40)
    ap.add_argument('--out', default='kmodel')
    ap.add_argument('--test', nargs='*', default=[])
    args = ap.parse_args()

    meta = json.load(open(args.onnx[:-5] + '.json'))
    n = meta['input']
    mid = f"{meta['id']}_{args.quant}"

    co = nncase.CompileOptions()
    co.target = 'k510'
    co.input_type = 'uint8'
    co.preprocess = True
    co.input_shape = [1, 3, n, n]
    co.input_layout = 'NCHW'
    co.output_layout = 'NCHW'  # the head already ends in NHWC; keep it as is
    co.mean = [0, 0, 0]
    co.std = [255, 255, 255]
    co.input_range = [0, 255]
    co.swapRB = False
    # nncase 1.9 k510 fails with "bad optional access" unless the dumps are on
    co.dump_ir = True
    co.dump_asm = True
    co.dump_dir = '/tmp/ncc_' + mid
    if args.quant == 'uint8':
        co.quant_type = 'uint8'
    compiler = nncase.Compiler(co)

    if args.quant == 'uint8':
        files = sorted(f for g in args.calib for f in glob.glob(g))
        random.Random(0).shuffle(files)
        files = files[:args.samples]
        assert files, 'no calibration images'
        data = np.stack([letterbox(f, n)[0] for f in files])[:, None]  # [samples, 1, 3, n, n]
        ptq = nncase.PTQTensorOptions()
        ptq.samples_count = len(files)
        ptq.set_tensor_data(data.astype(np.uint8).tobytes())
        compiler.use_ptq(ptq)

    t0 = time.time()
    compiler.import_onnx(open(args.onnx, 'rb').read(), nncase.ImportOptions())
    compiler.compile()
    kmodel = compiler.gencode_tobytes()
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, mid + '.kmodel'), 'wb') as f:
        f.write(kmodel)
    meta.update({'id': mid, 'quant': args.quant, 'file': mid + '.kmodel'})
    with open(os.path.join(args.out, mid + '.json'), 'w') as f:
        json.dump(meta, f, ensure_ascii=False, indent=1)
    print(f'{mid}: {len(kmodel) / 1e6:.1f} MB, compiled in {time.time() - t0:.0f} s')

    if args.test:
        sim = nncase.Simulator()
        sim.load_model(kmodel)
        for img in args.test:
            x, geom = letterbox(img, n)
            sim.set_input_tensor(0, nncase.RuntimeTensor.from_numpy(x[None]))
            sim.run()
            outs = [sim.get_output_tensor(i).to_numpy() for i in range(sim.outputs_size)]
            boxes = decode(outs, meta, geom, 0.3)
            print('  sim', os.path.basename(img), [(meta['labels'][b[5]], round(b[4], 2)) for b in boxes][:12])


if __name__ == '__main__':
    main()
