#!/usr/bin/env python3
"""把 ONNX 模型转为 FP16 (输入输出保持 FP32), 供显卡 (DirectML) 推理使用, 并在 GPU 上校验与 FP32 的误差。
   python make_fp16.py --dir output/common output/<声音名> [--check]
生成 xxx_fp16.onnx; C++ 端在显卡模式下自动优先加载。"""
import argparse
import glob
import os

import numpy as np
import onnx
from onnxconverter_common import float16

# 数值敏感的算子保留 FP32
BLOCK = ["Pow", "ReduceMean", "Sqrt", "Exp", "Log", "Softplus", "CumSum", "Sin", "Cos", "Range", "Mod",
         "GRU", "RandomNormalLike", "RandomUniformLike", "Div"]


def feeds_for(path, rng):
    n = os.path.basename(path)
    if n.startswith("hubert"):
        return {"audio": (rng.standard_normal((1, 32000)) * 0.1).astype(np.float32)}
    if n.startswith("rmvpe"):
        return {"audio": (rng.standard_normal((1, 5120 * 2 - 160)) * 0.1).astype(np.float32)}
    if n.startswith("enc"):
        T = 300
        return {"phone": rng.standard_normal((1, T, 768)).astype(np.float32), "pitch": rng.integers(1, 255, (1, T)).astype(np.int64),
                "rnd": rng.standard_normal((1, 192, T)).astype(np.float32), "sid": np.zeros(1, np.int64)}
    if n.startswith("dec"):
        L = 25
        f0 = (rng.random((1, L)) * 300 + 100).astype(np.float32)
        return {"z": rng.standard_normal((1, 192, L)).astype(np.float32), "f0": f0, "phase0": np.zeros(1, np.float32),
                "noise": np.zeros((1, L * 400), np.float32), "sid": np.zeros(1, np.int64)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", nargs="+", required=True)
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    for d in a.dir:
        for p in sorted(glob.glob(os.path.join(d, "*.onnx"))):
            if p.endswith("_fp16.onnx"):
                continue
            out = p[:-5] + "_fp16.onnx"
            m = onnx.load(p)
            m16 = float16.convert_float_to_float16(m, keep_io_types=True, op_block_list=BLOCK)
            onnx.save(m16, out)
            msg = "%s -> %s (%.0f MB)" % (os.path.basename(p), os.path.basename(out), os.path.getsize(out) / 1e6)
            if a.check:
                import onnxruntime as ort
                pr = ["CUDAExecutionProvider", "CPUExecutionProvider"]
                rng = np.random.default_rng(0)
                fd = feeds_for(p, rng)
                y32 = ort.InferenceSession(p, providers=pr).run(None, fd)[0]
                try:
                    y16 = ort.InferenceSession(out, providers=pr).run(None, fd)[0]
                    snr = 10 * np.log10((y32 ** 2).sum() / max(((y32 - y16) ** 2).sum(), 1e-20))
                except Exception as e:
                    snr = -1
                    msg += "  转换后无法运行 (%s)" % str(e).splitlines()[0][:60]
                msg += "  FP16 vs FP32 信噪比 %.1f dB %s" % (snr, "OK" if snr > 30 else "偏差大, 建议不用 fp16")
                if snr <= 30:
                    os.remove(out)
                    msg += " -> 已删除, 该模型显卡上用 FP32"
            print(msg, flush=True)


if __name__ == "__main__":
    main()
