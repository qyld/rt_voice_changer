#!/usr/bin/env python3
"""
训练 "中文增强底模": 在官方 RVC v2 底模 (f0G40k/f0D40k, 英文 VCTK 训练) 上,
用多说话人中文语音 (AISHELL-3) 继续训练完整生成器 (enc_p/flow/dec/enc_q/emb_g 全部参与) 和判别器。
中文底模学过大量中文音素与中文声调, 之后用几分钟样本微调声音时, 咬字更清楚、音色更容易贴近目标。

数据处理、损失函数、训练循环完全复用 RVC 的 train/train.py (多说话人: filelist 最后一列为说话人 id,
映射到 emb_g 的 109 个槽位), 切分/特征提取复用 train_voice.py。

用法 (服务器 /root/vcx 下):
    venv/bin/python pretrain_zh.py prepare --data data/aishell3 --gpus 4-5     # 切分 + F0 + HuBERT 特征 (一次)
    venv/bin/python pretrain_zh.py train --gpus 4 --epochs 20                 # 标准版 -> f0G40k_zh.pth / f0D40k_zh.pth
    venv/bin/python pretrain_zh.py train --lite --gpus 5 --epochs 20          # 轻量版 -> f0G40k_lite_zh.pth / ..
    venv/bin/python pretrain_zh.py export [--lite]                            # 仅从最新检查点导出底模 (训练中途也可)
训练可随时中断, 再次运行 train 自动从检查点续训 (epoch 数为总数)。
之后 train_voice.py 默认使用中文底模 (--base official 可切回官方底模)。
"""
import argparse
import glob
import json
import os
import random
import shutil
import subprocess
import sys
import time
from multiprocessing import Pool

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import train_voice as tv  # noqa: E402

RVC = tv.RVC
PY = sys.executable
SR_TAG, SR = "40k", 40000
N_SLOTS = 109  # 官方底模 emb_g 的说话人槽位数
DATA_EXP = os.path.join(RVC, "logs", "pretrain_zh_data")
PT_DIR = os.path.join(RVC, "assets", "pretrained_v2")
log = tv.log


# ---------------------------------------------------------------- 数据准备
def list_speakers(root):
    """返回 {说话人: [wav...]} 与性别表。支持 AISHELL-3 的 train/wav/SSBxxxx/*.wav 目录结构 (也会包含 test/)。"""
    spk = {}
    for p in glob.glob(os.path.join(root, "**", "*.wav"), recursive=True):
        spk.setdefault(os.path.basename(os.path.dirname(p)), []).append(p)
    gender = {}
    for info in glob.glob(os.path.join(root, "**", "spk-info.txt"), recursive=True):
        for line in open(info, encoding="utf8", errors="ignore"):
            f = line.split()
            if len(f) >= 3 and not line.startswith("#"):
                gender[f[0]] = f[2].lower()
    return spk, gender


def choose_speakers(spk, gender, n_slots, min_utts):
    """选出 <= 109 个说话人: 男声全部优先 (AISHELL-3 女声占多数, 保证男女都覆盖), 其余按语句数多少补足女声"""
    cands = sorted([s for s, v in spk.items() if len(v) >= min_utts], key=lambda s: (-len(spk[s]), s))
    male = [s for s in cands if gender.get(s, "").startswith("m")]
    other = [s for s in cands if s not in male]
    n_male = min(len(male), n_slots * 2 // 5)  # 男声最多占 40%
    chosen = male[:n_male] + other[: n_slots - n_male]
    if len(chosen) < n_slots:
        chosen += [s for s in male[n_male:]][: n_slots - len(chosen)]
    return sorted(chosen)


_W = {}


def _init_worker():
    import warnings
    warnings.filterwarnings("ignore")
    _W["slicer"], _W["bah"] = tv.make_slicer(SR)


def _slice_one(job):
    """单条语音: 读取 -> 切分 -> 写 0_gt_wavs / 1_16k_wavs。返回 (段数, 秒数)"""
    import librosa
    sid, path = job
    try:
        audio, _ = librosa.load(path, sr=SR, mono=True)
    except Exception:
        return 0, 0.0
    utt = os.path.splitext(os.path.basename(path))[0]
    n, sec = 0, 0.0
    for idx, a in tv.slice_audio(audio, SR, _W["slicer"], _W["bah"]):
        tv.write_segment(DATA_EXP, "%03d_%s_%d" % (sid, utt, idx), a, SR)
        n += 1
        sec += len(a) / SR
    return n, sec


def cmd_prepare(a):
    spk, gender = list_speakers(a.data)
    chosen = choose_speakers(spk, gender, N_SLOTS, a.min_utts)
    if not chosen:
        raise SystemExit("在 %s 下没有找到说话人目录" % a.data)
    n_m = sum(gender.get(s, "").startswith("m") for s in chosen)
    log("说话人 %d 个 (男 %d / 女 %d), 可用说话人总数 %d" % (len(chosen), n_m, len(chosen) - n_m, len(spk)))
    for d in ("0_gt_wavs", "1_16k_wavs"):
        shutil.rmtree(os.path.join(DATA_EXP, d), ignore_errors=True)
        os.makedirs(os.path.join(DATA_EXP, d))
    jobs = []
    rng = random.Random(0)
    for sid, s in enumerate(chosen):
        files = sorted(spk[s])
        if a.max_utts > 0 and len(files) > a.max_utts:
            files = sorted(rng.sample(files, a.max_utts))
        jobs += [(sid, f) for f in files]
    log("语音 %d 条, 开始切分 (%d 进程)" % (len(jobs), a.workers))
    n_seg, total = 0, 0.0
    t0 = time.time()
    with Pool(a.workers, initializer=_init_worker) as pool:
        for i, (n, sec) in enumerate(pool.imap_unordered(_slice_one, jobs, chunksize=16)):
            n_seg += n
            total += sec
            if (i + 1) % 5000 == 0:
                log("  切分 %d / %d  (%.0fs)" % (i + 1, len(jobs), time.time() - t0))
    log("切分完成: %d 段, %.1f 小时" % (n_seg, total / 3600))
    json.dump({"speakers": chosen, "gender": {s: gender.get(s, "?") for s in chosen},
               "segments": n_seg, "hours": total / 3600},
              open(os.path.join(DATA_EXP, "speakers.json"), "w", encoding="utf8"), ensure_ascii=False, indent=1)
    env = dict(os.environ, PYTHONPATH=RVC)
    tv.extract_features(DATA_EXP, a.gpus.split("-"), env)
    log("特征提取完成")


# ---------------------------------------------------------------- 训练
def exp_name(lite):
    return "pretrain_zh_lite" if lite else "pretrain_zh"


def write_filelist(exp_dir, lite):
    d = DATA_EXP
    stem = lambda sub, suf: {os.path.basename(p)[: -len(suf)] for p in glob.glob(os.path.join(d, sub, "*" + suf))}
    names = stem("0_gt_wavs", ".wav") & stem("3_feature768", ".npy") & stem("2a_f0", ".wav.npy") \
        & stem("2b-f0nsf", ".wav.npy")
    if not names:
        raise SystemExit("没有准备好的数据, 请先运行 prepare")
    lines = ["%s/0_gt_wavs/%s.wav|%s/3_feature768/%s.npy|%s/2a_f0/%s.wav.npy|%s/2b-f0nsf/%s.wav.npy|%d"
             % (d, n, d, n, d, n, d, n, int(n.split("_")[0])) for n in sorted(names)]
    sids = sorted({int(n.split("_")[0]) for n in names})
    mute = os.path.join(RVC, "logs", "mute")
    for sid in sids[::4]:  # 少量静音样本, 让模型学会输出干净的静音
        lines.append("%s/0_gt_wavs/mute%s.wav|%s/3_feature768/mute.npy|%s/2a_f0/mute.wav.npy|%s/2b-f0nsf/mute.wav.npy|%d"
                     % (mute, SR_TAG, mute, mute, mute, sid))
    random.Random(0).shuffle(lines)
    os.makedirs(exp_dir, exist_ok=True)
    with open(os.path.join(exp_dir, "filelist.txt"), "w", encoding="utf8") as f:
        f.write("\n".join(lines))
    cfg_rel = "lite/40k.json" if lite else "v1/40k.json"  # RVC v2 40k 使用 v1/40k.json 的网络结构
    cfg = json.load(open(os.path.join(RVC, "configs", cfg_rel), encoding="utf8"))
    json.dump(cfg, open(os.path.join(exp_dir, "config.json"), "w", encoding="utf8"), ensure_ascii=False, indent=2)
    log("filelist: %d 条, 说话人 %d 个" % (len(lines), len(sids)))


def cmd_train(a):
    name = exp_name(a.lite)
    exp_dir = os.path.join(RVC, "logs", name)
    if not os.path.exists(os.path.join(exp_dir, "filelist.txt")) or a.refresh_list:
        write_filelist(exp_dir, a.lite)
    suf = "_lite" if a.lite else ""
    pg = os.path.join(PT_DIR, "f0G40k%s.pth" % suf)
    pd = os.path.join(PT_DIR, "f0D40k%s.pth" % suf)
    for p in (pg, pd):
        if not os.path.exists(p):
            raise SystemExit("找不到初始化底模 %s" % p)
    log("训练 %s: 初始化 %s, %d epochs, GPU %s" % (name, os.path.basename(pg), a.epochs, a.gpus))
    cmd = [PY, "-m", "train.train", "-e", name, "-sr", SR_TAG, "-f0", "1", "-bs", str(a.bs), "-g", a.gpus,
           "-te", str(a.epochs), "-se", "1", "-pg", pg, "-pd", pd, "-l", "1", "-c", "0", "-sw", "0", "-v", "v2"]
    log("$ " + " ".join(cmd))
    r = subprocess.run(cmd, cwd=RVC, env=dict(os.environ, PYTHONPATH=RVC, RVC_CUDA_GRAPH="0"))
    if r.returncode != 0:
        raise SystemExit("训练失败")
    cmd_export(a)


def cmd_export(a):
    """把检查点 (含优化器状态) 转成底模格式 {"model": state_dict}, 与官方 f0G40k.pth 一致 (包含 enc_q, 可继续训练)"""
    import torch
    exp_dir = os.path.join(RVC, "logs", exp_name(a.lite))
    suf = "_lite" if a.lite else ""
    for kind in "GD":
        ck = os.path.join(exp_dir, "%s_2333333.pth" % kind)
        if not os.path.exists(ck):
            raise SystemExit("没有检查点 %s" % ck)
        c = torch.load(ck, map_location="cpu")
        out = os.path.join(PT_DIR, "f0%s40k%s_zh.pth" % (kind, suf))
        torch.save({"model": {k: v.float() for k, v in c["model"].items()}, "iteration": c.get("iteration", 0),
                    "learning_rate": 1e-4}, out + ".tmp")
        os.replace(out + ".tmp", out)
        log("导出 %s (epoch %s)" % (out, c.get("iteration")))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("prepare", help="切分 + 提取 F0/HuBERT 特征")
    p.add_argument("--data", default=os.path.join(HERE, "data", "aishell3"))
    p.add_argument("--gpus", default="0", help="物理卡号, 多卡用 4-5")
    p.add_argument("--workers", type=int, default=32)
    p.add_argument("--min-utts", type=int, default=100, help="语句数少于此的说话人不用")
    p.add_argument("--max-utts", type=int, default=0, help="每个说话人最多用多少条 (0=全部)")
    p = sub.add_parser("train", help="继续训练底模")
    p.add_argument("--lite", action="store_true", help="轻量版 (声码器 256 通道, 从 f0G40k_lite.pth 初始化)")
    p.add_argument("--gpus", default="0", help="物理卡号, 多卡用 4-5")
    p.add_argument("--epochs", type=int, default=20, help="总 epoch 数 (续训时也是总数)")
    p.add_argument("--bs", type=int, default=16, help="每卡 batch size")
    p.add_argument("--refresh-list", action="store_true", help="重新生成 filelist (数据有变化时)")
    p = sub.add_parser("export", help="从最新检查点导出底模")
    p.add_argument("--lite", action="store_true")
    a = ap.parse_args()
    if getattr(a, "data", None):
        a.data = os.path.abspath(a.data)
    {"prepare": cmd_prepare, "train": cmd_train, "export": cmd_export}[a.cmd](a)


if __name__ == "__main__":
    main()
