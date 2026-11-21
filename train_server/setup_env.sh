#!/bin/bash
# 训练服务器环境安装（Ubuntu + NVIDIA GPU）。用法: bash setup_env.sh [工作目录]
set -e
ROOT=${1:-/root/vcx}
cd "$ROOT"
# RVC 训练代码 (随本目录附带 rvc_code.tgz, MIT 协议)
if [ ! -d rvc ]; then mkdir rvc && tar xzf "$(dirname "$0")/rvc_code.tgz" -C rvc; fi
unset https_proxy HTTPS_PROXY http_proxy HTTP_PROXY all_proxy ALL_PROXY
export HF_ENDPOINT=${HF_ENDPOINT:-https://hf-mirror.com}
[ -d venv ] || python3 -m venv venv
venv/bin/pip install -U pip wheel
venv/bin/pip install torch==2.4.1 torchaudio==2.4.1 --index-url https://download.pytorch.org/whl/cu121
venv/bin/pip install numpy==1.26.4 scipy librosa==0.10.2 soundfile transformers==4.49.0 \
    onnx==1.16.2 onnxruntime==1.19.2 onnxsim faiss-cpu==1.8.0 scikit-learn tensorboard tqdm \
    praat-parselmouth huggingface_hub matplotlib av einops ffmpeg-python pyarrow torch==2.4.1
venv/bin/python - <<PY
from huggingface_hub import hf_hub_download, snapshot_download
snapshot_download("lj1995/VoiceConversionWebUI", allow_patterns=["hubert_base/*",
    "pretrained_v2/f0G40k.pth","pretrained_v2/f0D40k.pth",
    "pretrained_v2/f0G48k.pth","pretrained_v2/f0D48k.pth"], local_dir="rvc/assets")
hf_hub_download("lj1995/VoiceConversionWebUI","rmvpe.pt",local_dir="rvc/assets/rmvpe")
hf_hub_download("lj1995/VoiceConversionWebUI","mute.zip",local_dir="rvc/.model-downloads")
PY
venv/bin/python -m zipfile -e rvc/.model-downloads/mute.zip rvc/logs
mkdir -p rvc/assets/weights
echo SETUP_DONE
