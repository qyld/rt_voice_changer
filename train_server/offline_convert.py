#!/usr/bin/env python3
"""离线整句转换 (非流式, 用于训练后试听 & 作为 C++ 流式引擎的音质参考)。
   python offline_convert.py --voice output/xxx --common output/common --input in.wav --output out.wav --key 0
"""
import argparse
import json
import os

import librosa
import numpy as np
import onnxruntime as ort
import soundfile as sf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--voice", required=True)
    ap.add_argument("--common", required=True)
    ap.add_argument("--input", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--key", type=float, default=0)
    ap.add_argument("--index-rate", type=float, default=0.5)
    ap.add_argument("--protect", type=float, default=0.33)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--zero-noise", action="store_true", help="调试: 去掉随机噪声")
    a = ap.parse_args()
    meta = json.load(open(os.path.join(a.voice, "voice.json"), encoding="utf8"))
    sr, hop, C = meta["sample_rate"], meta["hop"], meta["inter_channels"]
    so = ort.SessionOptions()
    S = lambda p: ort.InferenceSession(p, so, providers=["CPUExecutionProvider"])
    hubert, rmvpe = S(os.path.join(a.common, "hubert.onnx")), S(os.path.join(a.common, "rmvpe.onnx"))
    enc, dec = S(os.path.join(a.voice, "enc.onnx")), S(os.path.join(a.voice, "dec.onnx"))

    x, _ = librosa.load(a.input, sr=16000, mono=True)
    n = len(x) // 160 * 160
    x = x[:n].astype(np.float32)
    P = n // 160
    feats = hubert.run(None, {"audio": x[None]})[0][0]
    feats0 = feats.copy()
    # f0: 补齐到 5120k-160
    n_f0 = 5120 * ((n + 160 - 1) // 5120 + 1) - 160
    sal = rmvpe.run(None, {"audio": np.pad(x, (0, n_f0 - n))[None]})[0][0]
    cents = 20 * np.arange(360) + 1997.3794084376191
    f0 = np.zeros(sal.shape[0], np.float32)
    for t in range(sal.shape[0]):
        c = int(sal[t].argmax())
        if sal[t, c] <= 0.03:
            continue
        lo, hi = max(0, c - 4), min(360, c + 5)
        f0[t] = 10 * 2 ** ((sal[t, lo:hi] * cents[lo:hi]).sum() / sal[t, lo:hi].sum() / 1200)
    f0 = f0[:P] * 2 ** (a.key / 12)
    # 检索
    ip = os.path.join(a.voice, "index.bin")
    if a.index_rate > 0 and os.path.exists(ip):
        hdr = np.fromfile(ip, np.int32, 4)
        big = np.fromfile(ip, np.float32, offset=16).reshape(hdr[2], hdr[3])
        d = (feats ** 2).sum(1, keepdims=True) + (big ** 2).sum(1)[None] - 2 * feats @ big.T
        d = np.maximum(d, 1e-6)
        ix = np.argsort(d, 1)[:, :8]
        sc = np.take_along_axis(d, ix, 1)
        w = 1 / sc ** 2
        w /= w.sum(1, keepdims=True)
        ret = (big[ix] * w[..., None]).sum(1)
        r = np.full((feats.shape[0], 1), a.index_rate, np.float32)
        if a.protect < 0.5:
            uv = f0[np.minimum(np.arange(feats.shape[0]) * 2, P - 1)] <= 0
            r[uv] *= a.protect
        feats = r * ret + (1 - r) * feats0
    phone = np.repeat(feats, 2, 0)
    phone = np.concatenate([phone, np.repeat(phone[-1:], max(0, P - len(phone)), 0)])[:P]
    mel = 1127 * np.log(1 + f0 / 700)
    lo, hi = 1127 * np.log(1 + 50 / 700), 1127 * np.log(1 + 1100 / 700)
    mel = np.where(mel > 0, (mel - lo) * 254 / (hi - lo) + 1, mel)
    pitch = np.rint(np.clip(mel, 1, 255)).astype(np.int64)
    rng = np.random.default_rng(a.seed)
    sid = np.zeros(1, np.int64)
    nz = 0.0 if a.zero_noise else 1.0
    z = enc.run(None, {"phone": phone[None].astype(np.float32), "pitch": pitch[None],
                       "rnd": nz * rng.standard_normal((1, C, P)).astype(np.float32), "sid": sid})[0]
    y = dec.run(None, {"z": z, "f0": f0[None].astype(np.float32), "phase0": np.zeros(1, np.float32),
                       "noise": nz * rng.standard_normal((1, P * hop)).astype(np.float32), "sid": sid})[0][0]
    sf.write(a.output, y, sr)
    print("写出", a.output, "%.1f 秒" % (len(y) / sr))


if __name__ == "__main__":
    main()
