// 实时变声引擎 (CPU 或 DirectML GPU, ONNX Runtime)
//   麦克风 -> 16k -> HuBERT(内容) + RMVPE(音高) -> [音色检索混合] -> Encoder+Flow -> NSF-HiFiGAN -> SOLA 拼接 -> 输出
#pragma once
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "resampler.h"

namespace fs = std::filesystem;

struct EngineConfig {
    // 结构参数 (修改后需重新 configure)
    int block_ms = 250;       // 每次推理处理的新音频长度 (越小延迟越低, CPU 压力越大)
    int extra_ms = 800;       // 左侧历史上下文 (越大越稳定, 越耗 CPU)
    int crossfade_ms = 50;    // SOLA 交叉淡化长度
    int search_ms = 10;       // SOLA 相位搜索范围
    int lookahead_ms = 80;    // 前瞻: 不输出窗口最右沿特征不可靠的部分 (提升咬字清晰度, 增加等量延迟)
    int threads = 0;          // ONNX 线程数, 0=自动
    int io_rate = 48000;      // 声卡采样率
};

struct RuntimeParams {  // 运行中可随时调整 (原子)
    std::atomic<float> pitch_shift{0.f};   // 变调 (半音)
    std::atomic<float> index_rate{0.5f};   // 音色检索强度 0..1 (越大越像目标音色, 过大可能吐字变糊)
    std::atomic<float> rms_mix{0.25f};     // 0=音量包络完全跟随原声 (保留语气起伏), 1=模型自身音量
    std::atomic<float> gate_db{-60.f};     // 噪声门限 dBFS (平滑门: 开门后保持 300ms 再渐隐, 不切字尾)
    std::atomic<float> in_gain{1.f};
    std::atomic<float> out_gain{1.f};
    std::atomic<float> protect{0.33f};     // 清辅音/呼吸保护 (0..0.5, 0.5=关闭)
    std::atomic<int> sid{0};
    std::atomic<bool> bypass{false};
};

struct EngineStats {
    std::atomic<float> infer_ms{0}, hubert_ms{0}, f0_ms{0}, synth_ms{0}, enc_ms{0};
    std::atomic<float> in_db{-120}, out_db{-120};
    std::atomic<float> f0_hz{0};
    std::atomic<long long> blocks{0};
};

// 推荐的结构参数档位
EngineConfig cpu_profile();
EngineConfig gpu_profile();

struct VoiceInfo {
    std::string name;
    fs::path dir;
    int sample_rate = 40000;
    int hop = 400;
    int inter_channels = 192;
    int n_speakers = 1;
};

class OnnxModel;

class VoiceEngine {
public:
    VoiceEngine();
    ~VoiceEngine();

    // device: -1=CPU, >=0=DirectML 显卡序号; GPU 初始化失败会自动回退 CPU (见 device_note())
    bool load_common(const fs::path& dir, int threads, std::string& err, int device = -1);
    bool load_voice(const fs::path& dir, std::string& err);
    void configure(const EngineConfig& cfg);
    bool ready() const { return hubert_ && rmvpe_ && enc_ && dec_; }
    int device() const { return device_; }               // 实际使用的设备
    const std::string& device_note() const { return device_note_; }
    void warmup();                                        // 按当前尺寸预跑一次 (GPU 首次编译图)

    int block_io() const { return block_io_; }           // 每块 io 采样数
    float latency_ms() const;                             // 算法固有延迟估计
    const VoiceInfo& voice() const { return voice_; }
    const EngineConfig& config() const { return cfg_; }

    // 处理一块: in 为 block_io 个 io_rate 采样, out 写 block_io 个
    void process(const float* in, float* out);
    void reset_state();

    RuntimeParams params;
    EngineStats stats;

private:
    void infer_block(std::vector<float>& out_model);
    void run_hubert(std::vector<float>& feats, int& T);
    void run_f0(std::vector<float>& f0);
    void index_blend(float* feats, int T, int from);
    void update_gate(int new_frames);
    void smooth_f0(int from);
    std::unique_ptr<OnnxModel> make_model(const fs::path& dir, const char* name);
    float phase_at(long long abs_frame, long long win_start_abs, const std::vector<float>& f0win);

    std::unique_ptr<OnnxModel> hubert_, rmvpe_, enc_, dec_;
    int threads_ = 0;
    int device_ = -1;
    std::string device_note_;
    bool force_infer_ = false;
    VoiceInfo voice_;
    EngineConfig cfg_;

    // 检索库
    std::vector<float> idx_feats_, idx_norm_;
    int idx_n_ = 0, idx_dim_ = 768;

    // 尺寸 (16k 采样)
    int block16_ = 4000, extra16_ = 12800, cross16_ = 800, search16_ = 192;
    int win16_ = 0, p_len_ = 0, ret_frames_ = 0, la_frames_ = 0;
    int block_m_ = 0, cross_m_ = 0, search_m_ = 0;  // 模型采样率下
    int block_io_ = 12000;

    std::vector<float> in16_;         // 滑动输入窗口 (16k)
    std::vector<float> f0cache_;      // Hz (已变调), 长度 p_len_
    std::vector<float> gate_gain_;    // 每 10ms 帧的噪声门增益 0..1, 长度 p_len_
    int gate_hold_ = 0;
    std::vector<float> sola_buf_;     // 上一块尾部
    std::vector<float> fade_in_, fade_out_;
    std::vector<float> fifo16_, fifo_out_;
    Resampler rs_in_, rs_out_, rs_bypass_;
    std::vector<float> bypass_delay_;
    long long abs_end_frame_ = 0;     // 已推进的绝对 10ms 帧数
    long long phase_anchor_frame_ = -1;
    float phase_anchor_ = 0.f;
    float gate_prev_ = 0.f;
};
