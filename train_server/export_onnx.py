#!/usr/bin/env python3
"""
把 RVC v2 模型导出为 C++ 实时引擎使用的 ONNX:

  enc.onnx : phone[1,T,768] f32, pitch[1,T] i64, rnd[1,192,T] f32, sid[1] i64 -> z[1,192,T]
             (TextEncoder + Flow 反向; 随机噪声由 C++ 按绝对帧号确定性生成, 保证分块重叠区域一致)
  dec.onnx : z[1,192,L] f32, f0[1,L] f32(Hz), phase0[1] f32(周期), noise[1,L*upp] f32, sid[1] i64
             -> audio[1,L*upp]
             (NSF-HiFiGAN; 正弦激励的初始相位由 C++ 传入, 实现跨块相位连续 -> 无接缝/嗡嗡声)
  common/hubert.onnx : audio16k[1,N] -> feats[1,T,768]   (ContentVec/HuBERT 第12层)
  common/rmvpe.onnx  : audio16k[1,N] -> salience[1,T,360] (N 必须 = 5120k-160, T = N/160+1)
"""
import argparse
import json
import math
import os
import sys

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

HERE = os.path.dirname(os.path.abspath(__file__))
RVC = os.path.join(HERE, "rvc")
sys.path.insert(0, RVC)

from infer.module.models import SynthesizerTrnMs768NSFsid  # noqa: E402
from infer.module import attentions  # noqa: E402

_orig_attention = attentions.MultiHeadAttention.attention


def _dyn_attention(self, query, key, value, mask=None):
    """与原 RVC 相对位置注意力数学等价, 但全部用动态形状算子实现, 导出的 ONNX 支持任意长度。
    原实现里 int(length) 会在 trace 时被固化成常数。"""
    b, d, t = key.size()
    nh, kc, w = self.n_heads, self.k_channels, self.window_size
    q = query.view(b, nh, kc, t).transpose(2, 3) / math.sqrt(kc)
    k = key.view(b, nh, kc, t).transpose(2, 3)
    v = value.view(b, nh, kc, t).transpose(2, 3)
    scores = torch.matmul(q, k.transpose(-2, -1))
    ar = torch.ones_like(query[0, 0, :]).cumsum(0) - 1           # [t] 0..t-1 (动态)
    rel = ar.unsqueeze(0) - ar.unsqueeze(1)                       # [t,t]  j-i
    m = torch.arange(-w, w + 1, dtype=query.dtype, device=query.device)
    oh = (rel.unsqueeze(-1) == m).to(query.dtype)                 # [t,t,2w+1]
    rk = torch.matmul(q, self.emb_rel_k[0].transpose(0, 1))      # [b,h,t,2w+1]
    scores = scores + torch.matmul(rk.unsqueeze(-2), oh.transpose(-1, -2)).squeeze(-2)
    if mask is not None:
        scores = scores.masked_fill(mask == 0, -1e4)
    p_attn = F.softmax(scores, dim=-1)
    out = torch.matmul(p_attn, v)
    rw = torch.matmul(p_attn.unsqueeze(-2), oh).squeeze(-2)       # [b,h,t,2w+1]
    out = out + torch.matmul(rw, self.emb_rel_v[0])
    return out.transpose(2, 3).contiguous().view(b, d, t), p_attn

OPSET = 17


class EncWrap(nn.Module):
    def __init__(self, net):
        super().__init__()
        self.enc_p, self.flow, self.emb_g = net.enc_p, net.flow, net.emb_g

    def forward(self, phone, pitch, rnd, sid):
        g = self.emb_g(sid).unsqueeze(-1)
        lengths = torch.ones_like(phone[:, :, 0]).sum(1).long()
        m_p, logs_p, x_mask = self.enc_p(phone, pitch, lengths)
        z_p = (m_p + torch.exp(logs_p) * rnd * 0.66666) * x_mask
        return self.flow(z_p, x_mask, g=g, reverse=True)


def conv1d_to_2d(module):
    """把 Conv1d/ConvTranspose1d 换成等价的 (1,k) Conv2d: ONNX Runtime CPU 对 2D 卷积有 NCHWc 优化内核, 快很多"""
    for name, c in list(module.named_children()):
        if isinstance(c, nn.Conv1d):
            n = nn.Conv2d(c.in_channels, c.out_channels, (1, c.kernel_size[0]), (1, c.stride[0]),
                          (0, c.padding[0]), (1, c.dilation[0]), c.groups, c.bias is not None)
            n.weight.data = c.weight.data.unsqueeze(2).clone()
            if c.bias is not None:
                n.bias.data = c.bias.data.clone()
            setattr(module, name, n)
        elif isinstance(c, nn.ConvTranspose1d):
            n = nn.ConvTranspose2d(c.in_channels, c.out_channels, (1, c.kernel_size[0]), (1, c.stride[0]),
                                   (0, c.padding[0]), (0, c.output_padding[0]), c.groups, c.bias is not None,
                                   (1, c.dilation[0]))
            n.weight.data = c.weight.data.unsqueeze(2).clone()
            if c.bias is not None:
                n.bias.data = c.bias.data.clone()
            setattr(module, name, n)
        else:
            conv1d_to_2d(c)
    return module


class DecWrap(nn.Module):
    def __init__(self, net, sr):
        super().__init__()
        import copy
        self.dec, self.emb_g = conv1d_to_2d(copy.deepcopy(net.dec)), net.emb_g
        self.sr = float(sr)
        self.upp = int(self.dec.upp)

    def forward(self, z, f0, phase0, noise, sid):
        d = self.dec
        upp = self.upp
        g = self.emb_g(sid).unsqueeze(-1)
        inc = f0 / self.sr                                  # [1,L] 每采样相位增量(周期)
        frame_inc = torch.fmod(inc * upp, 1.0)
        start = torch.cumsum(frame_inc, dim=1) - frame_inc + phase0.view(1, 1)
        start = torch.fmod(start, 1.0)
        k = torch.arange(1, upp + 1, dtype=f0.dtype, device=f0.device).view(1, 1, upp)
        ph = (start.unsqueeze(-1) + inc.unsqueeze(-1) * k).reshape(1, -1)
        sine = torch.sin(2 * math.pi * ph) * 0.1
        uv = ((f0 > 0).to(f0.dtype).unsqueeze(-1) * torch.ones_like(k)).reshape(1, -1)
        amp = uv * 0.003 + (1 - uv) * (0.1 / 3)
        exc = sine * uv + amp * noise                       # [1,N]
        har = torch.tanh(d.m_source.l_linear(exc.unsqueeze(-1))).transpose(1, 2).unsqueeze(2)  # [1,1,1,N]

        x = d.conv_pre(z.unsqueeze(2)) + d.cond(g.unsqueeze(2))
        for i in range(d.num_upsamples):
            x = F.leaky_relu(x, d.lrelu_slope)
            x = d.ups[i](x)
            x = x + d.noise_convs[i](har)
            xs = None
            for j in range(d.num_kernels):
                r = d.resblocks[i * d.num_kernels + j](x)
                xs = r if xs is None else xs + r
            x = xs / d.num_kernels
        x = F.leaky_relu(x)
        x = torch.tanh(d.conv_post(x))
        return x.squeeze(1).squeeze(1)


def load_net(weight):
    cpt = torch.load(weight, map_location="cpu", weights_only=False)
    cfg = list(cpt["config"])
    cfg[-3] = cpt["weight"]["emb_g.weight"].shape[0]
    net = SynthesizerTrnMs768NSFsid(*cfg, is_half=False)
    net.load_state_dict({k: v.float() for k, v in cpt["weight"].items()}, strict=False)
    net.eval()
    try:
        net.dec.remove_weight_norm()
    except Exception:
        pass
    for m in net.modules():  # flow / enc_p 内的 weight_norm
        try:
            torch.nn.utils.remove_weight_norm(m)
        except Exception:
            pass
    return net, cpt


def check(sess_path, feeds, ref, tag):
    import onnxruntime as ort
    s = ort.InferenceSession(sess_path, providers=["CPUExecutionProvider"])
    out = s.run(None, {k: v.numpy() for k, v in feeds.items()})[0]
    err = np.abs(out - ref.numpy()).max()
    print("  [check] %s max_abs_err=%.2e" % (tag, err))
    if not err < 5e-3:
        raise SystemExit("ONNX 校验失败: " + tag)


@torch.no_grad()
def export_voice(weight, out, name):
    net, cpt = load_net(weight)
    sr = int(cpt["config"][-1])
    inter = net.enc_p.out_channels
    enc, dec = EncWrap(net).eval(), DecWrap(net, sr).eval()
    sid = torch.zeros(1, dtype=torch.long)

    def enc_inputs(T):
        return dict(phone=torch.randn(1, T, 768), pitch=torch.randint(1, 255, (1, T)),
                    rnd=torch.randn(1, inter, T), sid=sid)

    p = os.path.join(out, "enc.onnx")
    tests = []
    for T in (37, 120, 200, 317):  # 先用原始注意力算参考输出
        x = enc_inputs(T)
        tests.append((T, x, enc(**x)))
    attentions.MultiHeadAttention.attention = _dyn_attention
    try:
        torch.onnx.export(enc, tuple(enc_inputs(100).values()), p, opset_version=OPSET,
                          input_names=["phone", "pitch", "rnd", "sid"], output_names=["z"],
                          dynamic_axes={"phone": {1: "T"}, "pitch": {1: "T"}, "rnd": {2: "T"}, "z": {2: "T"}})
    finally:
        attentions.MultiHeadAttention.attention = _orig_attention
    for T, x, ref in tests:  # 动态长度校验 (对比原始实现)
        check(p, x, ref, "enc T=%d" % T)

    def dec_inputs(L):
        f0 = torch.rand(1, L) * 300 + 100
        f0[:, : L // 4] = 0
        return dict(z=torch.randn(1, inter, L), f0=f0, phase0=torch.rand(1),
                    noise=torch.randn(1, L * dec.upp), sid=sid)

    p = os.path.join(out, "dec.onnx")
    torch.onnx.export(dec, tuple(dec_inputs(40).values()), p, opset_version=OPSET,
                      input_names=["z", "f0", "phase0", "noise", "sid"], output_names=["audio"],
                      dynamic_axes={"z": {2: "L"}, "f0": {1: "L"}, "noise": {1: "N"}, "audio": {1: "N"}})
    for L in (13, 40, 77):
        x = dec_inputs(L)
        check(p, x, dec(**x), "dec L=%d" % L)

    meta = dict(name=name, sample_rate=sr, hop=int(dec.upp), inter_channels=int(inter),
                feature_dim=768, version=cpt.get("version", "v2"), f0=int(cpt.get("f0", 1)),
                n_speakers=int(net.emb_g.weight.shape[0]), info=str(cpt.get("info", "")))
    with open(os.path.join(out, "voice.json"), "w", encoding="utf8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)
    print("  voice.json:", meta)


class HubertWrap(nn.Module):
    def __init__(self, model, normalize):
        super().__init__()
        self.m, self.normalize = model, normalize

    def forward(self, audio):
        if self.normalize:
            audio = (audio - audio.mean(-1, keepdim=True)) / torch.sqrt(audio.var(-1, keepdim=True, unbiased=False) + 1e-7)
        return self.m(input_values=audio).last_hidden_state


class STFTMag(nn.Module):
    """center=True, reflect pad, hann(1024), hop 160 -- 以卷积实现, 方便 ONNX 导出"""
    def __init__(self, n_fft=1024, hop=160):
        super().__init__()
        self.n_fft, self.hop = n_fft, hop
        win = torch.hann_window(n_fft)
        n = torch.arange(n_fft).float()
        kf = torch.arange(n_fft // 2 + 1).float()
        ang = 2 * math.pi * kf[:, None] * n[None, :] / n_fft
        self.register_buffer("wr", (torch.cos(ang) * win)[:, None, :])
        self.register_buffer("wi", (-torch.sin(ang) * win)[:, None, :])

    def forward(self, x):  # [1,N]
        x = F.pad(x.unsqueeze(1), (self.n_fft // 2, self.n_fft // 2), mode="reflect")
        re = F.conv1d(x, self.wr, stride=self.hop)
        im = F.conv1d(x, self.wi, stride=self.hop)
        return torch.sqrt(re * re + im * im)


class RmvpeWrap(nn.Module):
    def __init__(self, e2e, mel_basis):
        super().__init__()
        self.stft = STFTMag()
        self.register_buffer("mel_basis", mel_basis)
        self.model = e2e

    def forward(self, audio):
        mag = self.stft(audio)
        mel = torch.log(torch.clamp(torch.matmul(self.mel_basis, mag), min=1e-5))
        return self.model(mel)


@torch.no_grad()
def export_common(common):
    import onnxruntime as ort
    os.makedirs(common, exist_ok=True)
    hp = os.path.join(common, "hubert.onnx")
    if not os.path.exists(hp):
        from transformers import AutoFeatureExtractor, HubertModel
        path = os.path.join(RVC, "assets", "hubert_base")
        normalize = bool(AutoFeatureExtractor.from_pretrained(path).do_normalize)
        m = HubertModel.from_pretrained(path, attn_implementation="eager").eval()
        w = HubertWrap(m, normalize).eval()
        x = torch.randn(1, 16000)
        torch.onnx.export(w, (x,), hp, opset_version=OPSET, input_names=["audio"], output_names=["feats"],
                          dynamic_axes={"audio": {1: "N"}, "feats": {1: "T"}})
        for n in (8000, 24160, 40000):
            x = torch.randn(1, n) * 0.1
            check(hp, {"audio": x}, w(x), "hubert N=%d" % n)
        print("  hubert normalize =", normalize)
    rp = os.path.join(common, "rmvpe.onnx")
    if not os.path.exists(rp):
        from infer.rmvpe import E2E, MelSpectrogram
        e2e = E2E(4, 1, (2, 2))
        e2e.load_state_dict(torch.load(os.path.join(RVC, "assets", "rmvpe", "rmvpe.pt"), map_location="cpu"))
        e2e.eval()
        melx = MelSpectrogram(False, 128, 16000, 1024, 160, None, 30, 8000)
        w = RmvpeWrap(e2e, melx.mel_basis.clone()).eval()
        x = torch.randn(1, 5120 * 2 - 160) * 0.1
        torch.onnx.export(w, (x,), rp, opset_version=OPSET, input_names=["audio"], output_names=["salience"],
                          dynamic_axes={"audio": {1: "N"}, "salience": {1: "T"}})
        for k in (1, 2, 3):
            x = torch.randn(1, 5120 * k - 160) * 0.1
            ref = e2e(melx(x, center=True))
            check(rp, {"audio": x}, ref, "rmvpe N=%d" % x.shape[1])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--weight", default="")
    ap.add_argument("--out", default="")
    ap.add_argument("--name", default="voice")
    ap.add_argument("--common", required=True)
    a = ap.parse_args()
    a.common = os.path.abspath(a.common)
    a.out = os.path.abspath(a.out) if a.out else a.out
    a.weight = os.path.abspath(a.weight) if a.weight else a.weight
    os.chdir(RVC)
    torch.set_num_threads(16)
    export_common(os.path.abspath(a.common))
    if a.weight:
        os.makedirs(a.out, exist_ok=True)
        export_voice(a.weight, os.path.abspath(a.out), a.name)
    print("EXPORT_OK")


if __name__ == "__main__":
    main()
