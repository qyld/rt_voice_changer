#!/usr/bin/env python3
"""声码器 INT8 静态量化 (用真实语音特征校准) + 质量评估。
   python quantize_dec.py --voice output/xxx --common output/common --calib samples/xxx.wav
生成 dec_int8.onnx, 并打印与 FP32 的误差 (SNR / 对数梅尔谱距离)。"""
import argparse
import os

import librosa
import numpy as np
import onnxruntime as ort
from onnxruntime.quantization import CalibrationDataReader, QuantFormat, QuantType, quantize_static
from onnxruntime.quantization.shape_inference import quant_pre_process


def make_feeds(voice, common, wav, n=48, L=39):
    import json
    meta = json.load(open(os.path.join(voice, "voice.json")))
    hop, C = meta["hop"], meta["inter_channels"]
    S = lambda p: ort.InferenceSession(p, providers=["CPUExecutionProvider"])
    hub, rm, enc = S(os.path.join(common, "hubert.onnx")), S(os.path.join(common, "rmvpe.onnx")), S(os.path.join(voice, "enc.onnx"))
    x, _ = librosa.load(wav, sr=16000, duration=40)
    n16 = len(x) // 160 * 160
    x = x[:n16].astype(np.float32)
    P = n16 // 160
    feats = hub.run(None, {"audio": x[None]})[0][0]
    nf = 5120 * ((n16 + 159) // 5120 + 1) - 160
    sal = rm.run(None, {"audio": np.pad(x, (0, nf - n16))[None]})[0][0]
    cents = 20 * np.arange(360) + 1997.3794084376191
    f0 = np.zeros(P, np.float32)
    for t in range(P):
        c = int(sal[t].argmax())
        if sal[t, c] > 0.03:
            lo, hi = max(0, c - 4), min(360, c + 5)
            f0[t] = 10 * 2 ** ((sal[t, lo:hi] * cents[lo:hi]).sum() / sal[t, lo:hi].sum() / 1200)
    phone = np.repeat(feats, 2, 0)
    phone = np.concatenate([phone, np.repeat(phone[-1:], max(0, P - len(phone)), 0)])[:P]
    mel = 1127 * np.log(1 + f0 / 700)
    a, b = 1127 * np.log(1 + 50 / 700), 1127 * np.log(1 + 1100 / 700)
    mel = np.where(mel > 0, (mel - a) * 254 / (b - a) + 1, mel)
    pitch = np.rint(np.clip(mel, 1, 255)).astype(np.int64)
    rng = np.random.default_rng(0)
    z = enc.run(None, {"phone": phone[None], "pitch": pitch[None], "rnd": rng.standard_normal((1, C, P)).astype(np.float32),
                       "sid": np.zeros(1, np.int64)})[0]
    feeds = []
    for s in np.linspace(0, P - L, n).astype(int):
        feeds.append({"z": z[:, :, s:s + L].copy(), "f0": f0[None, s:s + L].copy(), "phase0": np.zeros(1, np.float32),
                      "noise": rng.standard_normal((1, L * hop)).astype(np.float32), "sid": np.zeros(1, np.int64)})
    return feeds, meta["sample_rate"]


class Reader(CalibrationDataReader):
    def __init__(self, feeds):
        self.it = iter(feeds)

    def get_next(self):
        return next(self.it, None)


def logmel(y, sr):
    m = librosa.feature.melspectrogram(y=y, sr=sr, n_fft=2048, hop_length=400, n_mels=80)
    return np.log10(np.maximum(m, 1e-5))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--voice", required=True)
    ap.add_argument("--common", required=True)
    ap.add_argument("--calib", required=True)
    a = ap.parse_args()
    feeds, sr = make_feeds(a.voice, a.common, a.calib)
    src = os.path.join(a.voice, "dec.onnx")
    pre = os.path.join(a.voice, "dec_pre.onnx")
    dst = os.path.join(a.voice, "dec_int8.onnx")
    quant_pre_process(src, pre, skip_symbolic_shape=True)
    quantize_static(pre, dst, Reader(feeds[::2]), quant_format=QuantFormat.QDQ, per_channel=True,
                    activation_type=QuantType.QUInt8, weight_type=QuantType.QInt8,
                    op_types_to_quantize=["Conv", "ConvTranspose"],
                    extra_options={"ActivationSymmetric": False, "WeightSymmetric": True})
    os.remove(pre)
    f32 = ort.InferenceSession(src, providers=["CPUExecutionProvider"])
    i8 = ort.InferenceSession(dst, providers=["CPUExecutionProvider"])
    snr, md = [], []
    for fd in feeds[1::2]:
        y0 = f32.run(None, fd)[0][0]
        y1 = i8.run(None, fd)[0][0]
        snr.append(10 * np.log10((y0 ** 2).sum() / max(((y0 - y1) ** 2).sum(), 1e-12)))
        md.append(np.abs(logmel(y0, sr) - logmel(y1, sr)).mean())
    print("INT8 vs FP32: SNR %.1f dB, 对数梅尔谱平均差 %.3f (越小越好, <0.05 基本听不出)" % (np.mean(snr), np.mean(md)))


if __name__ == "__main__":
    main()
