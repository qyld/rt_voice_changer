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


def make_slicer(sr):
    """返回 (切分器, 高通滤波系数), 参数与 RVC preprocess.py 一致"""
    from scipy import signal
    sys.path.insert(0, RVC)
    from train.dataset.slicer2 import Slicer
    slicer = Slicer(sr=sr, threshold=-42, min_length=1500, min_interval=400,
                    hop_size=15, max_sil_kept=500)
    return slicer, signal.butter(N=5, Wn=48, btype="high", fs=sr)


def slice_audio(audio, sr, slicer, bah, per=3.7):
    """与 RVC preprocess.py 等价: 高通 -> 静音切分 -> 3.7s 分段 -> 归一化。逐段 yield (段号, float32 音频)。"""
    import numpy as np
    from scipy import signal
    audio = signal.lfilter(bah[0], bah[1], audio)
    overlap, tail = 0.3, per + 0.3

    def norm(a):
        if a is None:
            return None
        peak = np.abs(a).max() if len(a) else 0
        if not np.isfinite(peak) or peak <= 0 or peak > 2.5 or len(a) < sr * 0.5:
            return None
        return ((a / peak * (0.9 * 0.75)) + 0.25 * a).astype(np.float32)

    idx = 0
    for chunk in slicer.slice(audio):
        i = 0
        tmp = None
        while True:
            start = int(sr * (per - overlap) * i)
            i += 1
            if len(chunk[start:]) > tail * sr:
                a = norm(chunk[start:start + int(per * sr)])
                if a is not None:
                    yield idx, a
                idx += 1
            else:
                tmp = chunk[start:]
                idx += 1
                break
        a = norm(tmp)
        if a is not None:
            yield idx, a
        idx += 1


def write_segment(exp_dir, stem, a, sr):
    import librosa
    from scipy.io import wavfile
    wavfile.write(os.path.join(exp_dir, "0_gt_wavs", stem + ".wav"), sr, a)
    a16 = librosa.resample(a, orig_sr=sr, target_sr=16000).astype("float32")
    wavfile.write(os.path.join(exp_dir, "1_16k_wavs", stem + ".wav"), 16000, a16)


def preprocess(inputs, exp_dir, sr, per=3.7):
    """切分样本并写 gt/16k wav。这里用 librosa 读取音频，不依赖 ffmpeg。"""
    import librosa

    for d in ("0_gt_wavs", "1_16k_wavs"):
        shutil.rmtree(os.path.join(exp_dir, d), ignore_errors=True)
        os.makedirs(os.path.join(exp_dir, d))
    slicer, bah = make_slicer(sr)
    total_sec = 0.0
    n_seg = 0
    for k, path in enumerate(inputs):
        audio, _ = librosa.load(path, sr=sr, mono=True)
        for idx, a in slice_audio(audio, sr, slicer, bah, per):
            write_segment(exp_dir, "%s_%s" % (k, idx), a, sr)
            n_seg += 1
            total_sec += len(a) / sr
        log("  切分 %s 完成" % os.path.basename(path))
    log("数据切分完成: %d 段, 共 %.1f 秒" % (n_seg, total_sec))
    if n_seg == 0:
        raise SystemExit("没有有效音频片段")
    return total_sec


def extract_features(exp_dir, gpus, env):
    """RMVPE F0 + HuBERT 特征, 按 GPU 数分片并行。gpus 为物理卡号列表。
    注意: 两个提取脚本都会用 i_gpu 参数覆盖 CUDA_VISIBLE_DEVICES, 所以必须传物理卡号。"""
    for d in ("2a_f0", "2b-f0nsf", "3_feature768"):
        shutil.rmtree(os.path.join(exp_dir, d), ignore_errors=True)
    n = len(gpus)
    for script in ("f0", "hubert"):
        procs = []
        for i, g in enumerate(gpus):
            if script == "f0":
                cmd = [PY, "train/dataset/extract_f0.py", "cuda", str(n), str(i), str(g), exp_dir, "true"]
            else:
                cmd = [PY, "train/dataset/extract_hubert_feature.py", "cuda:0", str(n), str(i), str(g),
                       exp_dir, "v2", "true"]
            log("$ " + " ".join(cmd))
            procs.append(subprocess.Popen(cmd, cwd=RVC, env=dict(env, CUDA_VISIBLE_DEVICES=str(g))))
        if any(p.wait() != 0 for p in procs):
            raise SystemExit("特征提取失败 (%s)" % script)


def pretrained_paths(sr_tag, lite, base):
    """底模路径。base: zh=中文增强底模 (pretrain_zh.py 产出), official=官方底模, auto=有中文底模就用"""
    d = os.path.join(RVC, "assets", "pretrained_v2")
    suf = "_lite" if lite else ""
    official = [os.path.join(d, "f0%s%s%s.pth" % (x, sr_tag, suf)) for x in "GD"]
    zh = [os.path.join(d, "f0%s%s%s_zh.pth" % (x, sr_tag, suf)) for x in "GD"]
    if base == "zh" and not all(os.path.exists(p) for p in zh):
        raise SystemExit("找不到中文底模 %s, 请先运行 pretrain_zh.py 或用 --base official" % zh[0])
    if base == "zh" or (base == "auto" and all(os.path.exists(p) for p in zh)):
        return zh
    return official


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
    ap.add_argument("--base", default="auto", choices=["auto", "zh", "official"],
                    help="底模: zh=中文增强底模 (更像目标/咬字更清楚), official=官方底模; 默认 auto: 中文底模存在则用它")
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

    if not args.skip_train:
        total_sec = preprocess(files, exp_dir, sr)
        extract_features(exp_dir, [args.gpu.split("-")[0]], env)
        if args.lite and args.sr != "40k":
            raise SystemExit("轻量底模目前只有 40k")
        write_filelist_and_config(exp_dir, args.sr, name, args.lite)
        epochs = args.epochs
        if epochs <= 0:
            # 数据越少, 需要的 epoch 越多 (每 epoch 步数少)
            epochs = 600 if total_sec < 120 else 300 if total_sec < 600 else 200 if total_sec < 1800 else 120
        save_every = min(args.save_every, epochs)
        pg, pd = pretrained_paths(args.sr, args.lite, args.base)
        log("开始训练: %d epochs, 底模 %s" % (epochs, os.path.basename(pg)))
        run([PY, "-m", "train.train", "-e", name, "-sr", args.sr, "-f0", "1", "-bs", str(args.batch),
             "-g", args.gpu, "-te", str(epochs), "-se", str(save_every),
             "-pg", pg, "-pd", pd,
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
