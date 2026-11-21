#!/usr/bin/env python3
"""
一键训练：样本音频 -> RVC v2 声音模型 -> 导出 C++ 实时变声器可用的 ONNX 声音包。

用法（在训练服务器上）:
    venv/bin/python train_voice.py --name 我的声音 --input /path/样本目录或文件 \
        [--sr 40k] [--epochs 300] [--batch 8] [--gpu 0]

产出:
    output/<name>/          可直接拷贝到 PC 端 voices/ 目录的声音包
        voice.json          元数据
        enc.onnx            内容编码器 + flow (CPU 推理)
        dec.onnx            NSF-HiFiGAN 声码器 (CPU 推理)
        index.bin           音色检索库 (k-means 聚类后的 HuBERT 特征)
        preview.wav         用训练集外的一句话做的离线转换试听 (若提供 --preview)
    output/common/hubert.onnx, rmvpe.onnx   公共模型（所有声音共享，只需拷贝一次）
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RVC = os.path.join(HERE, "rvc")
PY = sys.executable
AUDIO_EXT = (".wav", ".flac", ".mp3", ".m4a", ".ogg", ".opus", ".aac", ".wma")


def log(msg):
    print(time.strftime("[%H:%M:%S] ") + msg, flush=True)


def run(cmd, env=None):
    log("$ " + " ".join(cmd))
    r = subprocess.run(cmd, cwd=RVC, env=env)
    if r.returncode != 0:
        raise SystemExit("命令失败: %s" % " ".join(cmd))


def preprocess(inputs, exp_dir, sr, per=3.7):
    """与 RVC preprocess.py 等价: 高通 -> 静音切分 -> 3.7s 分段 -> 归一化 -> 写 gt/16k wav。
    这里用 librosa 读取音频，不依赖 ffmpeg。"""
    import librosa
    import numpy as np
    from scipy import signal
    from scipy.io import wavfile

    sys.path.insert(0, RVC)
    from train.dataset.slicer2 import Slicer

    gt_dir = os.path.join(exp_dir, "0_gt_wavs")
    w16_dir = os.path.join(exp_dir, "1_16k_wavs")
    for d in (gt_dir, w16_dir):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    slicer = Slicer(sr=sr, threshold=-42, min_length=1500, min_interval=400,
                    hop_size=15, max_sil_kept=500)
    bh, ah = signal.butter(N=5, Wn=48, btype="high", fs=sr)
    overlap, tail = 0.3, per + 0.3
    total_sec = 0.0
    n_seg = 0

    def norm_write(a, key, idx):
        nonlocal n_seg, total_sec
        peak = np.abs(a).max()
        if not np.isfinite(peak) or peak <= 0 or peak > 2.5 or len(a) < sr * 0.5:
            return
        a = (a / peak * (0.9 * 0.75)) + 0.25 * a
        wavfile.write(os.path.join(gt_dir, "%s_%s.wav" % (key, idx)), sr, a.astype(np.float32))
        a16 = librosa.resample(a, orig_sr=sr, target_sr=16000).astype(np.float32)
        wavfile.write(os.path.join(w16_dir, "%s_%s.wav" % (key, idx)), 16000, a16)
        n_seg += 1
        total_sec += len(a) / sr

    for k, path in enumerate(inputs):
        audio, _ = librosa.load(path, sr=sr, mono=True)
        audio = signal.lfilter(bh, ah, audio)
        idx = 0
        tmp = None
        for chunk in slicer.slice(audio):
            i = 0
            while True:
                start = int(sr * (per - overlap) * i)
                i += 1
                if len(chunk[start:]) > tail * sr:
                    norm_write(chunk[start:start + int(per * sr)], k, idx)
                    idx += 1
                else:
                    tmp = chunk[start:]
                    idx += 1
                    break
            norm_write(tmp, k, idx)
            idx += 1
        log("  切分 %s 完成" % os.path.basename(path))
    log("数据切分完成: %d 段, 共 %.1f 秒" % (n_seg, total_sec))
    if n_seg == 0:
        raise SystemExit("没有有效音频片段")
    return total_sec


def write_filelist_and_config(exp_dir, sr_tag, name, lite=False):
    import random
    gt = os.path.join(exp_dir, "0_gt_wavs")
    fe = os.path.join(exp_dir, "3_feature768")
    f0 = os.path.join(exp_dir, "2a_f0")
    f0n = os.path.join(exp_dir, "2b-f0nsf")
    stem = lambda d, suf: {os.path.basename(p)[: -len(suf)] for p in glob.glob(os.path.join(d, "*" + suf))}
    names = stem(gt, ".wav") & stem(fe, ".npy") & stem(f0, ".wav.npy") & stem(f0n, ".wav.npy")
    lines = ["%s/%s.wav|%s/%s.npy|%s/%s.wav.npy|%s/%s.wav.npy|0" % (gt, n, fe, n, f0, n, f0n, n)
             for n in sorted(names)]
    mute = os.path.join(RVC, "logs", "mute")
    for _ in range(2):
        lines.append("%s/0_gt_wavs/mute%s.wav|%s/3_feature768/mute.npy|%s/2a_f0/mute.wav.npy|%s/2b-f0nsf/mute.wav.npy|0"
                     % (mute, sr_tag, mute, mute, mute))
    random.shuffle(lines)
    with open(os.path.join(exp_dir, "filelist.txt"), "w", encoding="utf8") as f:
        f.write("\n".join(lines))
    cfg_rel = "v1/40k.json" if sr_tag == "40k" else "v2/%s.json" % sr_tag
    if lite:
        cfg_rel = "lite/%s.json" % sr_tag
    with open(os.path.join(RVC, "configs", cfg_rel), encoding="utf8") as f:
        cfg = json.load(f)
    with open(os.path.join(exp_dir, "config.json"), "w", encoding="utf8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)
    log("filelist: %d 条" % len(lines))


def build_index(exp_dir, out_path, max_centroids=4096):
    """把训练集 HuBERT 特征聚类成 <=4096 个中心，供 C++ 端暴力 kNN 检索 (等价于 RVC 的 faiss 索引)。"""
    import numpy as np
    from sklearn.cluster import MiniBatchKMeans
    feats = [np.load(p) for p in sorted(glob.glob(os.path.join(exp_dir, "3_feature768", "*.npy")))]
    big = np.concatenate(feats, 0).astype(np.float32)
    n = min(max_centroids, big.shape[0])
    if big.shape[0] > max_centroids:
        km = MiniBatchKMeans(n_clusters=n, batch_size=256 * 32, n_init=3, max_iter=100,
                             random_state=0).fit(big)
        big = km.cluster_centers_.astype(np.float32)
    with open(out_path, "wb") as f:
        np.array([0x58444956, 1, big.shape[0], big.shape[1]], dtype=np.int32).tofile(f)  # 'VIDX'
        big.tofile(f)
    log("检索库: %d x %d -> %s" % (big.shape[0], big.shape[1], out_path))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", required=True, help="声音名称 (英文/数字/下划线最稳妥)")
    ap.add_argument("--input", required=True, nargs="+", help="样本音频文件或目录 (可多个)")
    ap.add_argument("--sr", default="40k", choices=["40k", "48k"])
    ap.add_argument("--epochs", type=int, default=0, help="0=按数据时长自动选择")
    ap.add_argument("--batch", type=int, default=8)
    ap.add_argument("--gpu", default="0", help="GPU 编号, 多卡用 0-1")
    ap.add_argument("--save-every", type=int, default=50)
    ap.add_argument("--preview", default="", help="训练完成后用该音频做离线转换试听")
    ap.add_argument("--preview-key", type=int, default=0, help="试听时的变调(半音)")
    ap.add_argument("--skip-train", action="store_true", help="跳过训练, 仅重新导出")
    ap.add_argument("--lite", action="store_true",
                    help="使用轻量底模 (声码器计算量 1/4, 适合 4 核及以下的普通 CPU; 需先运行 distill_lite.py)")
    args = ap.parse_args()

    os.environ.pop("https_proxy", None)
    files = []
    for p in args.input:
        if os.path.isdir(p):
            files += sorted(f for f in glob.glob(os.path.join(p, "**", "*"), recursive=True)
                            if f.lower().endswith(AUDIO_EXT))
        elif os.path.isfile(p):
            files.append(os.path.abspath(p))
    if not files:
        raise SystemExit("找不到样本音频")
    log("样本音频 %d 个" % len(files))

    name = args.name
    sr = {"40k": 40000, "48k": 48000}[args.sr]
    exp_dir = os.path.join(RVC, "logs", name)
    os.makedirs(exp_dir, exist_ok=True)
    os.makedirs(os.path.join(RVC, "assets", "weights"), exist_ok=True)
    out_dir = os.path.join(HERE, "output", name)
    common_dir = os.path.join(HERE, "output", "common")
    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(common_dir, exist_ok=True)

    env = dict(os.environ)
    env["PYTHONPATH"] = RVC
    first_gpu = args.gpu.split("-")[0]

    if not args.skip_train:
        total_sec = preprocess(files, exp_dir, sr)
        # F0 (RMVPE, GPU)
        for d in ("2a_f0", "2b-f0nsf", "3_feature768"):
            shutil.rmtree(os.path.join(exp_dir, d), ignore_errors=True)
        e = dict(env, CUDA_VISIBLE_DEVICES=first_gpu)
        run([PY, "train/dataset/extract_f0.py", "cuda", "1", "0", "0", exp_dir, "true"], e)
        run([PY, "train/dataset/extract_hubert_feature.py", "cuda:0", "1", "0", "0", exp_dir, "v2", "true"], e)
        if args.lite and args.sr != "40k":
            raise SystemExit("轻量底模目前只有 40k")
        write_filelist_and_config(exp_dir, args.sr, name, args.lite)
        epochs = args.epochs
        if epochs <= 0:
            # 数据越少, 需要的 epoch 越多 (每 epoch 步数少)
            epochs = 600 if total_sec < 120 else 300 if total_sec < 600 else 200 if total_sec < 1800 else 120
        save_every = min(args.save_every, epochs)
        log("开始训练: %d epochs" % epochs)
        run([PY, "-m", "train.train", "-e", name, "-sr", args.sr, "-f0", "1", "-bs", str(args.batch),
             "-g", args.gpu, "-te", str(epochs), "-se", str(save_every),
             "-pg", "assets/pretrained_v2/f0G%s%s.pth" % (args.sr, "_lite" if args.lite else ""),
             "-pd", "assets/pretrained_v2/f0D%s%s.pth" % (args.sr, "_lite" if args.lite else ""),
             "-l", "1", "-c", "1", "-sw", "0", "-v", "v2"], dict(env, RVC_CUDA_GRAPH="0"))

    weight = os.path.join(RVC, "assets", "weights", "%s.pth" % name)
    gck = os.path.join(exp_dir, "G_2333333.pth")
    if os.path.exists(gck) and (not os.path.exists(weight) or os.path.getmtime(gck) > os.path.getmtime(weight) + 60):
        # 从最新训练检查点提取推理模型 (兼容中途停止的训练)
        code = ("import torch;from train import utils;from train.process_ckpt import savee;"
                "c=torch.load(%r,map_location='cpu');hps=utils.get_hparams_from_dir(%r);"
                "print(savee(c['model'],%r,1,%r,c.get('iteration',0),'v2',hps))" % (gck, exp_dir, args.sr, name))
        run([PY, "-c", code], env)
    if not os.path.exists(weight):
        raise SystemExit("未找到训练好的模型: %s" % weight)
    build_index(exp_dir, os.path.join(out_dir, "index.bin"))

    # 导出 ONNX
    exp = [PY, os.path.join(HERE, "export_onnx.py"), "--weight", weight, "--out", out_dir,
           "--common", common_dir, "--name", name]
    run(exp, dict(env, CUDA_VISIBLE_DEVICES=""))
    if args.preview:
        run([PY, os.path.join(HERE, "offline_convert.py"), "--voice", out_dir, "--common", common_dir,
             "--input", os.path.abspath(args.preview), "--output", os.path.join(out_dir, "preview.wav"),
             "--key", str(args.preview_key)], dict(env, CUDA_VISIBLE_DEVICES=""))
    log("完成! 声音包: %s  公共模型: %s" % (out_dir, common_dir))


if __name__ == "__main__":
    main()
