#!/usr/bin/env python3
"""变声质量客观评估:
  - 音色相似度: WavLM 说话人向量, 输出 vs 目标声音训练样本 (越高越像目标), 同时给出 原声 vs 目标 作对照
  - 清晰度: Whisper 识别 输出 与 原声 的文字, 计算字错误率 CER (越低越好)
  - 语气/情绪保留: 输出与原声的 F0 曲线相关系数 (去掉变调后), 及音量包络相关系数
用法: python eval_quality.py --target 目标样本.wav --source 原声.wav --outputs a.wav b.wav ...
"""
import argparse
import os

import librosa
import numpy as np
import torch

HERE = os.path.dirname(os.path.abspath(__file__))
dev = "cuda" if torch.cuda.is_available() else "cpu"


def load(p, sr=16000):
    return librosa.load(p, sr=sr, mono=True)[0]


class Spk:
    def __init__(self):
        from transformers import AutoFeatureExtractor, WavLMForXVector
        p = os.path.join(HERE, "eval_models", "wavlm-base-plus-sv")
        self.fe = AutoFeatureExtractor.from_pretrained(p)
        self.m = WavLMForXVector.from_pretrained(p).to(dev).eval()

    @torch.no_grad()
    def emb(self, y):
        es = []
        for s in range(0, max(1, len(y) - 16000 * 3), 16000 * 4):  # 4 秒片段平均
            seg = y[s:s + 16000 * 6]
            x = self.fe(seg, sampling_rate=16000, return_tensors="pt").to(dev)
            e = self.m(**x).embeddings[0]
            es.append(torch.nn.functional.normalize(e, dim=-1))
        return torch.nn.functional.normalize(torch.stack(es).mean(0), dim=-1)


class ASR:
    def __init__(self):
        from transformers import WhisperForConditionalGeneration, WhisperProcessor
        p = os.path.join(HERE, "eval_models", "whisper-small")
        self.pr = WhisperProcessor.from_pretrained(p)
        self.m = WhisperForConditionalGeneration.from_pretrained(p).to(dev).eval()

    @torch.no_grad()
    def text(self, y):
        out = []
        for s in range(0, len(y), 16000 * 25):
            x = self.pr(y[s:s + 16000 * 25], sampling_rate=16000, return_tensors="pt").input_features.to(dev)
            ids = self.m.generate(x, language="zh", task="transcribe")
            out.append(self.pr.batch_decode(ids, skip_special_tokens=True)[0])
        return "".join(out)


def cer(ref, hyp):
    import re
    norm = lambda s: re.sub(r"[\s，。！？、,.!?;；:：\"'“”‘’()（）-]", "", s)
    r, h = norm(ref), norm(hyp)
    d = np.arange(len(h) + 1)
    for i in range(1, len(r) + 1):
        prev, d[0] = d.copy(), i
        for j in range(1, len(h) + 1):
            d[j] = min(prev[j] + 1, d[j - 1] + 1, prev[j - 1] + (r[i - 1] != h[j - 1]))
    return d[len(h)] / max(1, len(r))


def f0(y):
    f, _, _ = librosa.pyin(y, fmin=60, fmax=900, sr=16000, frame_length=1024, hop_length=160)
    return f


def rms(y):
    return librosa.feature.rms(y=y, frame_length=640, hop_length=160)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", required=True)
    ap.add_argument("--source", required=True)
    ap.add_argument("--outputs", nargs="+", required=True)
    a = ap.parse_args()
    spk, asr = Spk(), ASR()
    tgt = spk.emb(load(a.target))
    src = load(a.source)
    src_text = asr.text(src)
    fs, rs = f0(src), rms(src)
    print("原声识别: %s" % src_text)
    print("原声 vs 目标 音色相似度: %.3f   (参照线: 不变声时的相似度)" % float(spk.emb(src) @ tgt))
    print("%-40s %8s %8s %8s %8s" % ("输出", "音色相似", "CER", "F0相关", "音量相关"))
    for p in a.outputs:
        y = load(p)
        n = min(len(y), len(src))
        sim = float(spk.emb(y) @ tgt)
        c = cer(src_text, asr.text(y))
        fo, ro = f0(y[:n]), rms(y[:n])
        m = min(len(fo), len(fs))
        ok = ~np.isnan(fo[:m]) & ~np.isnan(fs[:m])
        fc = np.corrcoef(np.log(fo[:m][ok]), np.log(fs[:m][ok]))[0, 1] if ok.sum() > 10 else float("nan")
        m = min(len(ro), len(rs))
        rc = np.corrcoef(ro[:m], rs[:m])[0, 1]
        print("%-40s %8.3f %8.3f %8.3f %8.3f" % (os.path.basename(p), sim, c, fc, rc))


if __name__ == "__main__":
    main()
