#!/usr/bin/env python3
"""
训练 "轻量版" RVC 底模 (CPU 实时友好): 声码器通道数减半 (512 -> 256), 计算量约为原版 1/4。
除声码器外 (内容编码器/flow/后验编码器/说话人嵌入) 全部沿用官方预训练底模 f0G40k。
声码器以 (官方底模的后验编码器 z, RMVPE 基频) 为输入, 用真实语音 + 原版声码器输出做目标, GAN 训练。

   torchrun --nproc_per_node=2 distill_lite.py --data data --steps 60000      (CUDA_VISIBLE_DEVICES=4,5)
产出: rvc/assets/pretrained_v2/f0G40k_lite.pth, f0D40k_lite.pth, rvc/configs/lite/40k.json
之后训练声音时加 --lite 即可使用该底模。
"""
import argparse
import copy
import glob
import io
import json
import os
import random
import sys
import time

import numpy as np
import torch
import torch.distributed as dist
import torch.nn.functional as F

HERE = os.path.dirname(os.path.abspath(__file__))
RVC = os.path.join(HERE, "rvc")
sys.path.insert(0, RVC)
ORIG_CWD = os.getcwd()
os.chdir(RVC)

from infer.module import commons  # noqa: E402
from infer.module.models import MultiPeriodDiscriminatorV2, SynthesizerTrnMs768NSFsid  # noqa: E402
from train.losses import discriminator_loss, feature_loss, generator_loss  # noqa: E402
from train.mel_processing import mel_spectrogram_torch, spectrogram_torch  # noqa: E402

SR = 40000
HOP = 400


def log(*a):
    if not dist.is_initialized() or dist.get_rank() == 0:
        print(time.strftime("[%H:%M:%S]"), *a, flush=True)


# ---------------------------------------------------------------- 数据
def prepare(data_root, cache):
    """收集语音 -> 40k + RMVPE f0, 缓存为 npz"""
    import librosa
    from scipy import signal
    from infer.rmvpe import RMVPE
    os.makedirs(cache, exist_ok=True)
    rm = RMVPE(os.path.join(RVC, "assets", "rmvpe", "rmvpe.pt"), is_half=False, device="cuda")
    bh, ah = signal.butter(N=5, Wn=48, btype="high", fs=SR)
    items = [("f", p) for p in sorted(glob.glob(os.path.join(data_root, "aishell3", "**", "*.wav"), recursive=True))]
    for pq in sorted(glob.glob(os.path.join(data_root, "libritts_r", "**", "*.parquet"), recursive=True)):
        import pyarrow.parquet as pqm
        t = pqm.read_table(pq, columns=["audio"]).to_pylist()
        items += [("b", r["audio"]["bytes"]) for r in t]
    log("语音条数:", len(items))
    out, n = [], 0
    buf_w, buf_f = [], []
    for i, (kind, src) in enumerate(items):
        try:
            y, _ = librosa.load(src if kind == "f" else io.BytesIO(src), sr=SR, mono=True)
        except Exception:
            continue
        if len(y) < SR:
            continue
        y = signal.lfilter(bh, ah, y)
        peak = np.abs(y).max()
        if peak <= 1e-4:
            continue
        y = (y / peak * 0.9 * 0.75 + 0.25 * y).astype(np.float32)
        y = y[: len(y) // HOP * HOP]
        y16 = librosa.resample(y, orig_sr=SR, target_sr=16000)
        f0 = rm.infer_from_audio(y16, thred=0.03).astype(np.float32)
        T = len(y) // HOP
        f0 = np.pad(f0, (0, max(0, T - len(f0))))[:T]
        buf_w.append(y.astype(np.float16))
        buf_f.append(f0)
        if len(buf_w) >= 500 or i == len(items) - 1:
            np.savez(os.path.join(cache, "part%04d.npz" % n), *buf_w, *buf_f, n=len(buf_w))
            n += 1
            buf_w, buf_f = [], []
            log("prep", i + 1, "/", len(items))
    if buf_w:
        np.savez(os.path.join(cache, "part%04d.npz" % n), *buf_w, *buf_f, n=len(buf_w))


def load_cache(cache):
    W, Fz = [], []
    for p in sorted(glob.glob(os.path.join(cache, "part*.npz"))):
        d = np.load(p)
        k = int(d["n"])
        W += [d["arr_%d" % i] for i in range(k)]
        Fz += [d["arr_%d" % (k + i)] for i in range(k)]
    return W, Fz


def batch(W, Fz, bs, ctx_frames, rng):
    ws, fs = [], []
    for _ in range(bs):
        while True:
            i = rng.randrange(len(W))
            T = len(Fz[i])
            if T > ctx_frames:
                break
        s = rng.randrange(T - ctx_frames)
        ws.append(W[i][s * HOP:(s + ctx_frames) * HOP].astype(np.float32))
        fs.append(Fz[i][s:s + ctx_frames])
    return torch.from_numpy(np.stack(ws)), torch.from_numpy(np.stack(fs))


# ---------------------------------------------------------------- 模型
def build(cfg):
    m = cfg["model"]
    return SynthesizerTrnMs768NSFsid(cfg["data"]["filter_length"] // 2 + 1, cfg["train"]["segment_size"] // HOP,
                                     **m, is_half=False, sr=SR)


def init_by_slicing(student, teacher):
    """用原版声码器权重的子集初始化轻量版 (比随机初始化收敛快)"""
    ts = teacher.state_dict()
    sd = student.state_dict()
    for k, v in sd.items():
        if k in ts:
            t = ts[k]
            sl = tuple(slice(0, s) for s in v.shape)
            if t.dim() == v.dim():
                sd[k] = t[sl].clone()
    student.load_state_dict(sd)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=os.path.join(HERE, "data"))
    ap.add_argument("--cache", default=os.path.join(HERE, "data", "cache40k"))
    ap.add_argument("--steps", type=int, default=60000)
    ap.add_argument("--bs", type=int, default=16)
    ap.add_argument("--channels", type=int, default=256)
    ap.add_argument("--prepare-only", action="store_true")
    ap.add_argument("--resume", action="store_true")
    a = ap.parse_args()
    a.data = os.path.join(ORIG_CWD, a.data)
    a.cache = os.path.join(ORIG_CWD, a.cache)

    ddp = "WORLD_SIZE" in os.environ
    if ddp:
        dist.init_process_group("nccl")
        rank = dist.get_rank()
    else:
        rank = 0
    torch.cuda.set_device(rank)
    dev = torch.device("cuda", rank)

    if rank == 0 and not glob.glob(os.path.join(a.cache, "part*.npz")):
        prepare(a.data, a.cache)
    if a.prepare_only:
        return
    if ddp:
        dist.barrier()
    W, Fz = load_cache(a.cache)
    log("缓存语音条数 %d, 时长 %.1f 小时" % (len(W), sum(len(w) for w in W) / SR / 3600))

    cfg = json.load(open(os.path.join(RVC, "configs", "v1", "40k.json")))
    lite_cfg = copy.deepcopy(cfg)
    lite_cfg["model"]["upsample_initial_channel"] = a.channels
    os.makedirs(os.path.join(RVC, "configs", "lite"), exist_ok=True)
    if rank == 0:
        json.dump(lite_cfg, open(os.path.join(RVC, "configs", "lite", "40k.json"), "w"), indent=2)

    teacher = build(cfg)
    teacher.load_state_dict(torch.load(os.path.join(RVC, "assets/pretrained_v2/f0G40k.pth"), map_location="cpu")["model"])
    teacher = teacher.to(dev).eval()
    for p in teacher.parameters():
        p.requires_grad_(False)
    student_full = build(lite_cfg)
    init_by_slicing(student_full, teacher)
    sdec = student_full.dec.to(dev).train()
    netd = MultiPeriodDiscriminatorV2(False).to(dev)
    netd.load_state_dict(torch.load(os.path.join(RVC, "assets/pretrained_v2/f0D40k.pth"), map_location="cpu")["model"])

    og = torch.optim.AdamW(sdec.parameters(), 2e-4, betas=(0.8, 0.99), eps=1e-9)
    od = torch.optim.AdamW(netd.parameters(), 1e-4, betas=(0.8, 0.99), eps=1e-9)
    ck_path = os.path.join(HERE, "data", "lite_ckpt.pth")
    step = 0
    if a.resume and os.path.exists(ck_path):
        ck = torch.load(ck_path, map_location="cpu")
        sdec.load_state_dict(ck["dec"]); netd.load_state_dict(ck["d"])
        og.load_state_dict(ck["og"]); od.load_state_dict(ck["od"]); step = ck["step"]
        log("恢复自 step", step)
    if ddp:
        sdec = torch.nn.parallel.DistributedDataParallel(sdec, device_ids=[rank])
        netd = torch.nn.parallel.DistributedDataParallel(netd, device_ids=[rank])
    sch_g = torch.optim.lr_scheduler.ExponentialLR(og, 0.99999, last_epoch=-1)
    sch_d = torch.optim.lr_scheduler.ExponentialLR(od, 0.99999, last_epoch=-1)
    scaler = torch.cuda.amp.GradScaler()
    d = cfg["data"]
    mel = lambda y: mel_spectrogram_torch(y.float(), d["filter_length"], d["n_mel_channels"], SR, HOP, d["win_length"],
                                          d["mel_fmin"], d["mel_fmax"])
    rng = random.Random(1234 + rank)
    CTX, SEG = 64, 32
    t0 = time.time()
    while step < a.steps:
        wav, f0 = batch(W, Fz, a.bs, CTX, rng)
        wav, f0 = wav.to(dev), f0.to(dev)
        with torch.no_grad(), torch.cuda.amp.autocast():
            spec = spectrogram_torch(wav, d["filter_length"], SR, HOP, d["win_length"], center=False)
            T = min(spec.shape[-1], CTX)
            spec, f0 = spec[..., :T], f0[:, :T]
            sid = torch.randint(0, teacher.emb_g.num_embeddings, (a.bs,), device=dev)
            g = teacher.emb_g(sid).unsqueeze(-1)
            z, m_q, logs_q, y_mask = teacher.enc_q(spec, torch.full((a.bs,), T, device=dev), g=g)
            st = torch.randint(8, T - SEG - 8, (a.bs,), device=dev)
            zs = commons.slice_segments(z, st, SEG)
            fs = commons.slice_segments2(f0, st, SEG)
            ygt = commons.slice_segments(wav.unsqueeze(1), st * HOP, SEG * HOP)
            yt = teacher.dec(zs, fs, g=g).float()
        with torch.cuda.amp.autocast():
            ys = sdec(zs, fs, g=g)
            dr, dg, _, _ = netd(ygt, ys.detach())
            with torch.cuda.amp.autocast(enabled=False):
                ld, _, _ = discriminator_loss(dr, dg)
        od.zero_grad()
        scaler.scale(ld).backward()
        scaler.step(od)
        with torch.cuda.amp.autocast():
            dr, dg, fr, fg = netd(ygt, ys)
            with torch.cuda.amp.autocast(enabled=False):
                ms = mel(ys.squeeze(1))
                l_mel = F.l1_loss(ms, mel(ygt.squeeze(1))) * 45
                l_kd = F.l1_loss(ms, mel(yt.squeeze(1))) * 20
                l_fm = feature_loss(fr, fg)
                l_gen, _ = generator_loss(dg)
                lg = l_gen + l_fm + l_mel + l_kd
        og.zero_grad()
        scaler.scale(lg).backward()
        scaler.unscale_(og)
        torch.nn.utils.clip_grad_norm_(sdec.parameters(), 1000)
        scaler.step(og)
        scaler.update()
        sch_g.step(); sch_d.step()
        step += 1
        if step % 200 == 0:
            log("step %d  d=%.3f gen=%.3f fm=%.3f mel=%.3f kd=%.3f  %.2f it/s" % (
                step, ld.item(), l_gen.item(), l_fm.item(), l_mel.item() / 45, l_kd.item() / 20, 200 / (time.time() - t0)))
            t0 = time.time()
        if rank == 0 and (step % 2000 == 0 or step == a.steps):
            dmod = sdec.module if ddp else sdec
            dd = netd.module if ddp else netd
            torch.save({"dec": dmod.state_dict(), "d": dd.state_dict(), "og": og.state_dict(), "od": od.state_dict(),
                        "step": step}, ck_path)
            full = teacher.state_dict()
            full = {k: v.cpu() for k, v in full.items() if not k.startswith("dec.")}
            full.update({"dec." + k: v.cpu() for k, v in dmod.state_dict().items()})
            torch.save({"model": full, "iteration": step, "learning_rate": 1e-4},
                       os.path.join(RVC, "assets/pretrained_v2/f0G40k_lite.pth"))
            torch.save({"model": {k: v.cpu() for k, v in dd.state_dict().items()}, "iteration": step, "learning_rate": 1e-4},
                       os.path.join(RVC, "assets/pretrained_v2/f0D40k_lite.pth"))
            log("已保存 lite 底模 step", step)
    if ddp:
        dist.destroy_process_group()


if __name__ == "__main__":
    main()
