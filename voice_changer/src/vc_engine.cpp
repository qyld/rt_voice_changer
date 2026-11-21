#include "vc_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <future>
#include <sstream>
#include <thread>

#include "onnx_model.h"
#include "vc_log.h"

namespace {

Ort::Env& ort_env() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "voice_changer");
    return env;
}

using clk = std::chrono::steady_clock;
inline float ms_since(clk::time_point t) {
    return std::chrono::duration<float, std::milli>(clk::now() - t).count();
}

// 确定性高斯噪声: 同一绝对位置每次得到相同的值 -> 重叠窗口推理结果一致, 拼接更干净
inline uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
inline float gauss(uint64_t key) {
    uint64_t h = splitmix64(key);
    double u1 = ((h >> 11) + 0.5) * (1.0 / 9007199254740992.0);
    double u2 = ((splitmix64(h) >> 11) + 0.5) * (1.0 / 9007199254740992.0);
    return float(std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2));
}

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
// 极简 JSON 数值字段读取
double json_num(const std::string& s, const std::string& key, double def) {
    auto k = s.find("\"" + key + "\"");
    if (k == std::string::npos) return def;
    auto c = s.find(':', k);
    if (c == std::string::npos) return def;
    return std::atof(s.c_str() + c + 1);
}
std::string json_str(const std::string& s, const std::string& key) {
    auto k = s.find("\"" + key + "\"");
    if (k == std::string::npos) return "";
    auto q1 = s.find('"', s.find(':', k) + 1);
    auto q2 = s.find('"', q1 + 1);
    return s.substr(q1 + 1, q2 - q1 - 1);
}

float to_db(float rms) { return 20.f * std::log10(std::max(rms, 1e-6f)); }

}  // namespace

VoiceEngine::VoiceEngine() = default;
VoiceEngine::~VoiceEngine() = default;

EngineConfig cpu_profile() {
    EngineConfig c;  // 默认值即 CPU 档
    return c;
}
EngineConfig gpu_profile() {
    EngineConfig c;
    // 实测 (男声->女声, Whisper 字错误率): 块长 120/150ms 时接缝过多 CER 0.38; 200ms 时 0.215
    c.block_ms = 200;
    c.extra_ms = 2000;     // 大上下文: 更接近训练时的 3.7s 切片
    c.crossfade_ms = 50;
    c.lookahead_ms = 60;
    return c;
}

// GPU 上优先使用 fp16 模型 (xxx_fp16.onnx), 不存在则用 fp32
std::unique_ptr<OnnxModel> VoiceEngine::make_model(const fs::path& dir, const char* name) {
    const fs::path cpu = dir / (std::string(name) + ".onnx");
    fs::path p = cpu;
    if (device_ >= 0 && !std::getenv("VC_NO_FP16")) {
        fs::path h = dir / (std::string(name) + "_fp16.onnx");
        if (fs::exists(h)) p = h;
    }
    auto m = std::make_unique<OnnxModel>(ort_env(), p, threads_ ? threads_ : 4, device_, cpu);
    vc_log(std::string("加载模型 ") + p.filename().u8string() + (device_ >= 0 ? " (显卡 #" + std::to_string(device_) + ")" : " (CPU)"));
    return m;
}

bool VoiceEngine::load_common(const fs::path& dir, int threads, std::string& err, int device) {
    if (threads <= 0) {
        unsigned hc = std::max(2u, std::thread::hardware_concurrency());
        threads = (int)std::clamp(hc / 2, 2u, 8u);
    }
    threads_ = threads;
    device_ = device;
    device_note_.clear();
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            hubert_ = make_model(dir, "hubert");
            rmvpe_ = make_model(dir, "rmvpe");
            return true;
        } catch (const std::exception& e) {
            if (device_ >= 0) {  // GPU 失败 -> 回退 CPU
                device_note_ = std::string("显卡推理初始化失败, 已改用 CPU: ") + e.what();
                vc_log(device_note_);
                device_ = -1;
                continue;
            }
            err = std::string("加载公共模型失败: ") + e.what();
            vc_log(err);
            return false;
        }
    }
    return false;
}

bool VoiceEngine::load_voice(const fs::path& dir, std::string& err) {
    try {
        std::string js = read_text(dir / "voice.json");
        if (js.empty()) { err = "找不到 voice.json"; return false; }
        VoiceInfo v;
        v.dir = dir;
        v.name = json_str(js, "name");
        v.sample_rate = (int)json_num(js, "sample_rate", 40000);
        v.hop = (int)json_num(js, "hop", v.sample_rate / 100);
        v.inter_channels = (int)json_num(js, "inter_channels", 192);
        v.n_speakers = (int)json_num(js, "n_speakers", 1);
        auto enc = make_model(dir, "enc");
        auto dec = make_model(dir, "dec");
        // 检索库 (可选)
        idx_n_ = 0;
        idx_feats_.clear();
        std::ifstream f(dir / "index.bin", std::ios::binary);
        if (f) {
            int32_t h[4];
            f.read((char*)h, sizeof(h));
            if (h[0] == 0x58444956) {
                idx_n_ = h[2];
                idx_dim_ = h[3];
                idx_feats_.resize((size_t)idx_n_ * idx_dim_);
                f.read((char*)idx_feats_.data(), idx_feats_.size() * 4);
                idx_norm_.resize(idx_n_);
                for (int i = 0; i < idx_n_; ++i) {
                    const float* c = &idx_feats_[(size_t)i * idx_dim_];
                    float s = 0;
                    for (int d = 0; d < idx_dim_; ++d) s += c[d] * c[d];
                    idx_norm_[i] = s;
                }
            }
        }
        enc_ = std::move(enc);
        dec_ = std::move(dec);
        voice_ = v;
        configure(cfg_);
        return true;
    } catch (const std::exception& e) {
        err = std::string("加载声音模型失败: ") + e.what();
        vc_log(err);
        return false;
    }
}

void VoiceEngine::configure(const EngineConfig& c) {
    cfg_ = c;
    auto frames = [](int ms) { return std::max(1, (ms + 5) / 10); };
    block16_ = frames(c.block_ms) * 160;
    extra16_ = frames(c.extra_ms) * 160;
    cross16_ = frames(c.crossfade_ms) * 160;
    search16_ = frames(c.search_ms) * 160;
    la_frames_ = std::max(0, (c.lookahead_ms + 5) / 10);
    win16_ = extra16_ + search16_ + cross16_ + block16_ + la_frames_ * 160;
    p_len_ = win16_ / 160;
    ret_frames_ = (search16_ + cross16_ + block16_) / 160;
    const int hop = voice_.hop;
    block_m_ = block16_ / 160 * hop;
    cross_m_ = cross16_ / 160 * hop;
    search_m_ = search16_ / 160 * hop;
    block_io_ = block16_ / 160 * (c.io_rate / 100);
    rs_in_.init(c.io_rate, 16000);
    rs_out_.init(voice_.sample_rate, c.io_rate);
    fade_in_.resize(cross_m_);
    fade_out_.resize(cross_m_);
    for (int i = 0; i < cross_m_; ++i) {
        double s = std::sin(0.5 * M_PI * double(i) / std::max(1, cross_m_ - 1));
        fade_in_[i] = float(s * s);
        fade_out_[i] = 1.f - fade_in_[i];
    }
    reset_state();
}

void VoiceEngine::reset_state() {
    in16_.assign(win16_, 0.f);
    f0cache_.assign(p_len_, 0.f);
    gate_gain_.assign(p_len_, 0.f);
    gate_hold_ = 0;
    sola_buf_.assign(cross_m_, 0.f);
    fifo16_.assign(16, 0.f);     // 预填少量零, 吸收重采样器的 ±1 抖动
    fifo_out_.assign(16, 0.f);
    rs_in_.reset();
    rs_out_.reset();
    abs_end_frame_ = 0;
    phase_anchor_frame_ = -1;
    phase_anchor_ = 0;
}

float VoiceEngine::latency_ms() const {
    // 采集一块 + 交叉淡化 + 搜索 + 重采样延迟; 推理时间与声卡缓冲另计
    return (block16_ + cross16_ + search16_ + la_frames_ * 160) / 16.f;
}

void VoiceEngine::process(const float* in, float* out) {
    const float ig = params.in_gain.load(), og = params.out_gain.load();
    std::vector<float> x(in, in + block_io_);
    double e = 0;
    for (auto& v : x) { v *= ig; e += double(v) * v; }
    stats.in_db = to_db(float(std::sqrt(e / std::max(1, block_io_))));

    if (params.bypass.load()) {
        for (int i = 0; i < block_io_; ++i) out[i] = x[i] * og;
        stats.out_db = stats.in_db.load();
        return;
    }
    rs_in_.process(x.data(), x.size(), fifo16_);
    std::vector<float> om;
    if (ready()) {
        const size_t before = fifo16_.size();
        try {
            infer_block(om);
        } catch (const std::exception& e) {  // 绝不让推理异常结束程序: 本块输出静音
            if (errors++ < 5) vc_log(std::string("推理异常 (本块静音): ") + e.what());
            if (fifo16_.size() == before && before >= (size_t)block16_)  // 本块输入尚未取走
                fifo16_.erase(fifo16_.begin(), fifo16_.begin() + block16_);
            om.assign(block_m_, 0.f);
        }
    } else {
        fifo16_.erase(fifo16_.begin(), fifo16_.begin() + std::min<size_t>(block16_, fifo16_.size()));
        om.assign(block_m_, 0.f);
    }
    rs_out_.process(om.data(), om.size(), fifo_out_);
    int n = std::min<int>(block_io_, (int)fifo_out_.size());
    int pad = block_io_ - n;  // 只在启动瞬间可能发生
    e = 0;
    for (int i = 0; i < pad; ++i) out[i] = 0.f;
    for (int i = 0; i < n; ++i) {
        float v = fifo_out_[i] * og;
        out[pad + i] = v;
        e += double(v) * v;
    }
    fifo_out_.erase(fifo_out_.begin(), fifo_out_.begin() + n);
    stats.out_db = to_db(float(std::sqrt(e / std::max(1, block_io_))));
}

void VoiceEngine::run_hubert(std::vector<float>& feats, int& T) {
    std::vector<Tensor> in{Tensor::F({1, (int64_t)in16_.size()}, in16_)};
    std::vector<int64_t> shp;
    feats = hubert_->run(in, &shp);
    T = (int)shp[1];
}

void VoiceEngine::run_f0(std::vector<float>& f0) {
    // RMVPE 只需分析最新一段 (帧数须为 32 的倍数: N = 5120k-160)
    int f0_len = 5120 * ((block16_ + 800 - 1) / 5120 + 1) - 160;
    while (f0_len > win16_ && f0_len > 5120) f0_len -= 5120;
    std::vector<float> seg(in16_.end() - f0_len, in16_.end());
    std::vector<Tensor> in{Tensor::F({1, (int64_t)f0_len}, std::move(seg))};
    std::vector<int64_t> shp;
    auto sal = rmvpe_->run(in, &shp);
    const int T = (int)shp[1], C = (int)shp[2];
    f0.assign(T, 0.f);
    for (int t = 0; t < T; ++t) {
        const float* s = &sal[(size_t)t * C];
        int c = int(std::max_element(s, s + C) - s);
        if (s[c] <= 0.03f) continue;
        double num = 0, den = 0;
        for (int k = std::max(0, c - 4); k <= std::min(C - 1, c + 4); ++k) {
            double cents = 20.0 * k + 1997.3794084376191;
            num += s[k] * cents;
            den += s[k];
        }
        f0[t] = float(10.0 * std::pow(2.0, (num / den) / 1200.0));
    }
}

void VoiceEngine::index_blend(float* feats, int T, int from) {
    const float rate = params.index_rate.load();
    if (idx_n_ <= 0 || rate <= 0.f || idx_dim_ != 768) return;
    const float protect = params.protect.load();
    const int K = std::min(8, idx_n_);
    std::vector<float> dist(idx_n_);
    std::vector<int> ord(idx_n_);
    std::vector<float> agg(768);
    for (int t = std::max(0, from); t < T; ++t) {
        float* q = feats + (size_t)t * 768;
        float qn = 0;
        for (int d = 0; d < 768; ++d) qn += q[d] * q[d];
        for (int i = 0; i < idx_n_; ++i) {
            const float* c = &idx_feats_[(size_t)i * 768];
            float dot = 0;
            for (int d = 0; d < 768; ++d) dot += q[d] * c[d];
            dist[i] = std::max(qn + idx_norm_[i] - 2.f * dot, 1e-6f);
        }
        for (int i = 0; i < idx_n_; ++i) ord[i] = i;
        std::partial_sort(ord.begin(), ord.begin() + K, ord.end(),
                          [&](int a, int b) { return dist[a] < dist[b]; });
        double wsum = 0;
        std::fill(agg.begin(), agg.end(), 0.f);
        for (int k = 0; k < K; ++k) {
            double w = 1.0 / (double(dist[ord[k]]) * dist[ord[k]]);
            wsum += w;
        }
        for (int k = 0; k < K; ++k) {
            float w = float((1.0 / (double(dist[ord[k]]) * dist[ord[k]])) / wsum);
            const float* c = &idx_feats_[(size_t)ord[k] * 768];
            for (int d = 0; d < 768; ++d) agg[d] += w * c[d];
        }
        // 清辅音/呼吸 (无基频) 帧降低检索强度, 保护咬字
        int f0i = std::min(p_len_ - 1, 2 * t);
        float r = rate;
        if (protect < 0.5f && f0cache_[f0i] <= 0.f) r *= protect;
        for (int d = 0; d < 768; ++d) q[d] = r * agg[d] + (1.f - r) * q[d];
    }
}

float VoiceEngine::phase_at(long long s, long long win_start, const std::vector<float>& f0win) {
    if (phase_anchor_frame_ < win_start || phase_anchor_frame_ > s) {
        phase_anchor_frame_ = s;
        phase_anchor_ = 0.f;
        return 0.f;
    }
    double ph = phase_anchor_;
    const double k = double(voice_.hop) / voice_.sample_rate;
    for (long long f = phase_anchor_frame_; f < s; ++f) {
        double inc = f0win[(size_t)(f - win_start)] * k;
        ph += inc - std::floor(inc);
        ph -= std::floor(ph);
    }
    phase_anchor_frame_ = s;
    phase_anchor_ = float(ph);
    return phase_anchor_;
}

void VoiceEngine::infer_block(std::vector<float>& out) {
    auto t0 = clk::now();
    // ---- 1. 取新块 + 噪声门 ----
    std::vector<float> blk(fifo16_.begin(), fifo16_.begin() + block16_);
    fifo16_.erase(fifo16_.begin(), fifo16_.begin() + block16_);
    // 注意: 不再把低音量帧置零送进 HuBERT (会切掉轻声字头字尾); 噪声门改为作用在输出上
    const int bf = block16_ / 160;
    std::memmove(in16_.data(), in16_.data() + block16_, (win16_ - block16_) * sizeof(float));
    std::copy(blk.begin(), blk.end(), in16_.end() - block16_);
    std::memmove(f0cache_.data(), f0cache_.data() + bf, (p_len_ - bf) * sizeof(float));
    std::fill(f0cache_.end() - bf, f0cache_.end(), 0.f);
    std::memmove(gate_gain_.data(), gate_gain_.data() + bf, (p_len_ - bf) * sizeof(float));
    update_gate(bf);
    abs_end_frame_ += bf;
    const long long win_start = abs_end_frame_ - p_len_;

    // 静音快速路径: 输出区间完全无声时跳过推理, 节省 CPU
    // 噪声门在整个输出区间都关闭时跳过推理 (上一块已渐隐到 0, 下一块从 0 交叉淡入, 不会咔哒)
    const int chk = std::min(p_len_, ret_frames_ + la_frames_ + 2);
    bool silent = !force_infer_ && std::all_of(gate_gain_.end() - chk, gate_gain_.end(), [](float g) { return g <= 0.f; });
    if (silent) {
        out.assign(block_m_, 0.f);
        std::fill(sola_buf_.begin(), sola_buf_.end(), 0.f);
        stats.infer_ms = ms_since(t0);
        stats.f0_hz = 0;
        stats.blocks++;
        return;
    }

    // ---- 2. HuBERT 与 RMVPE 并行 ----
    std::vector<float> feats, f0;
    int T = 0;
    auto t1 = clk::now();
    if (device_ >= 0) {  // GPU: 顺序执行 (同一显卡队列, 并行无收益)
        run_hubert(feats, T);
        stats.hubert_ms = ms_since(t1);
        auto t = clk::now();
        run_f0(f0);
        stats.f0_ms = ms_since(t);
    } else {
        auto fut = std::async(std::launch::async, [&] { auto t = clk::now(); run_f0(f0); stats.f0_ms = ms_since(t); });
        run_hubert(feats, T);
        stats.hubert_ms = ms_since(t1);
        fut.get();
    }

    const float key = params.pitch_shift.load();
    const float kf = std::pow(2.f, key / 12.f);
    const int Tf = (int)f0.size();
    const int nw = std::min(Tf - 4, p_len_);
    for (int i = 0; i < nw; ++i) f0cache_[p_len_ - nw + i] = f0[3 + (Tf - 4 - nw) + i] * kf;
    static const bool no_smooth = std::getenv("VC_NO_F0SMOOTH") != nullptr;  // 调试开关
    if (!no_smooth) smooth_f0(p_len_ - nw);
    {
        float last = 0;
        for (int i = p_len_ - 1; i >= p_len_ - bf; --i) if (f0cache_[i] > 0) { last = f0cache_[i]; break; }
        stats.f0_hz = last;
    }

    // ---- 3. 音色检索 ----
    index_blend(feats.data(), T, extra16_ / 320);

    // ---- 4. Encoder + Flow ----
    auto t2 = clk::now();
    const int P = p_len_, C = voice_.inter_channels;
    std::vector<float> phone((size_t)P * 768);
    for (int j = 0; j < P; ++j) {
        int src = std::min(j / 2, T - 1);
        std::memcpy(&phone[(size_t)j * 768], &feats[(size_t)src * 768], 768 * sizeof(float));
    }
    const double mel_min = 1127.0 * std::log(1.0 + 50.0 / 700.0);
    const double mel_max = 1127.0 * std::log(1.0 + 1100.0 / 700.0);
    std::vector<int64_t> pitch(P);
    for (int j = 0; j < P; ++j) {
        double m = 1127.0 * std::log(1.0 + f0cache_[j] / 700.0);
        if (m > 0) m = (m - mel_min) * 254.0 / (mel_max - mel_min) + 1.0;
        m = std::clamp(m, 1.0, 255.0);
        pitch[j] = (int64_t)std::lround(m);
    }
    static const bool zero_noise = std::getenv("VC_ZERO_NOISE") != nullptr;  // 调试: 去掉随机性以便与参考实现逐样本比对
    std::vector<float> rnd((size_t)C * P);
    if (!zero_noise)
    for (int c = 0; c < C; ++c)
        for (int j = 0; j < P; ++j)
            rnd[(size_t)c * P + j] = gauss(((uint64_t)(win_start + j) << 8) ^ (uint64_t)c ^ 0xA5A5000000000000ull);
    const int64_t sid = std::clamp(params.sid.load(), 0, voice_.n_speakers - 1);
    std::vector<Tensor> ein{Tensor::F({1, P, 768}, std::move(phone)), Tensor::I({1, P}, std::move(pitch)),
                            Tensor::F({1, C, P}, std::move(rnd)), Tensor::I({1}, {sid})};
    auto z = enc_->run(ein);
    stats.enc_ms = ms_since(t2);

    // ---- 5. 声码器: 只解码尾部 (输出区 + 少量左侧余量) ----
    const int R = ret_frames_, LA = la_frames_;
    const int Mg = std::min(4, P - R - LA);
    const int RA = std::min(2, LA);                  // 右侧余量: 多解码 2 帧再丢弃, 避免卷积边缘效应
    const int s = P - LA - R - Mg, L = Mg + R + RA, hop = voice_.hop;
    std::vector<float> zs((size_t)C * L), f0s(L);
    for (int c = 0; c < C; ++c) std::memcpy(&zs[(size_t)c * L], &z[(size_t)c * P + s], L * sizeof(float));
    std::memcpy(f0s.data(), &f0cache_[s], L * sizeof(float));
    float ph0 = phase_at(win_start + s, win_start, f0cache_);
    std::vector<float> noise((size_t)L * hop);
    const uint64_t base = (uint64_t)(win_start + s) * hop;
    if (!zero_noise)
        for (size_t k = 0; k < noise.size(); ++k) noise[k] = gauss((base + k) ^ 0x5A5A000000000000ull);
    std::vector<Tensor> din{Tensor::F({1, C, L}, std::move(zs)), Tensor::F({1, L}, std::move(f0s)),
                            Tensor::F({1}, {ph0}), Tensor::F({1, (int64_t)L * hop}, std::move(noise)),
                            Tensor::I({1}, {sid})};
    auto audio = dec_->run(din);
    stats.synth_ms = ms_since(t2);
    std::vector<float> inf(audio.begin() + (size_t)Mg * hop, audio.begin() + (size_t)(Mg + R) * hop);  // R*hop

    // ---- 6. 音量包络跟随 (保留原声的轻重缓急/情绪起伏) ----
    const float rmix = params.rms_mix.load();
    if (rmix < 1.f) {
        const float* src = in16_.data() + (win16_ - (R + LA) * 160);
        auto rms_frames = [](const float* x, int n, int h) {
            int nf = n / h;
            std::vector<float> r(nf + 1);
            for (int f = 0; f <= nf; ++f) {
                double e = 0;
                int cnt = 0;
                for (int k = f * h - 2 * h; k < f * h + 2 * h; ++k)
                    if (k >= 0 && k < n) { e += double(x[k]) * x[k]; ++cnt; }
                r[f] = float(std::sqrt(e / std::max(1, cnt)));
            }
            return r;
        };
        auto r1 = rms_frames(src, R * 160, 160);
        auto r2 = rms_frames(inf.data(), R * hop, hop);
        const float ex = 1.f - rmix;
        for (int k = 0; k < R * hop; ++k) {
            float pos = float(k) / hop;
            int f = std::min((int)pos, R - 1);
            float a = pos - f;
            float v1 = r1[f] * (1 - a) + r1[f + 1] * a;
            float v2 = std::max(r2[f] * (1 - a) + r2[f + 1] * a, 1e-3f);
            inf[k] *= std::min(std::pow(v1 / v2, ex), 20.f);
        }
    }

    // ---- 6b. 噪声门 (按帧增益插值, 平滑开合) ----
    {
        const int f_start = P - LA - R;
        for (int k = 0; k < R * hop; ++k) {
            float pos = float(k) / hop;
            int f = std::min((int)pos, R - 1);
            float a = pos - f;
            float g0 = gate_gain_[f_start + f];
            float g1 = gate_gain_[std::min(f_start + f + 1, P - 1)];
            inf[k] *= g0 * (1 - a) + g1 * a;
        }
    }

    // ---- 7. SOLA 拼接 ----
    int best = 0;
    {
        double best_v = -1e30;
        for (int off = 0; off <= search_m_; ++off) {
            double nom = 0, den = 0;
            for (int i = 0; i < cross_m_; ++i) {
                float v = inf[off + i];
                nom += double(v) * sola_buf_[i];
                den += double(v) * v;
            }
            double sc = nom / std::sqrt(den + 1e-8);
            if (sc > best_v) { best_v = sc; best = off; }
        }
    }
    out.assign(inf.begin() + best, inf.begin() + best + block_m_);
    for (int i = 0; i < cross_m_; ++i) out[i] = out[i] * fade_in_[i] + sola_buf_[i] * fade_out_[i];
    std::copy(inf.begin() + best + block_m_, inf.begin() + best + block_m_ + cross_m_, sola_buf_.begin());

    stats.infer_ms = ms_since(t0);
    stats.blocks++;
}

// 平滑噪声门: 40ms 窗 RMS 超过门限即开门 (并提前 30ms 打开, 保护字头),
// 低于门限后保持 300ms, 再 80ms 线性渐隐 (保护字尾/轻声)
void VoiceEngine::update_gate(int nf) {
    const float thr = params.gate_db.load();
    const int P = p_len_;
    if (thr <= -90.f) {
        std::fill(gate_gain_.end() - nf, gate_gain_.end(), 1.f);
        return;
    }
    const int hold_frames = 30, release_frames = 8, pre_open = 3;
    for (int f = P - nf; f < P; ++f) {
        double e = 0;
        int cnt = 0;
        for (int k = f * 160 - 240; k < f * 160 + 400; ++k)
            if (k >= 0 && k < win16_) { e += double(in16_[k]) * in16_[k]; ++cnt; }
        bool loud = to_db(float(std::sqrt(e / std::max(1, cnt)))) >= thr;
        float prev = f > 0 ? gate_gain_[f - 1] : 0.f;
        float g;
        if (loud) {
            gate_hold_ = hold_frames;
            g = 1.f;
            for (int b = std::max(0, f - pre_open); b < f; ++b) gate_gain_[b] = 1.f;
        } else if (gate_hold_ > 0) {
            --gate_hold_;
            g = prev > 0.f ? 1.f : 0.f;
        } else {
            g = std::max(0.f, prev - 1.f / release_frames);
        }
        gate_gain_[f] = g;
    }
}

// 音高平滑: 词内 <=30ms 的无声空洞按对数线性插值补齐 (避免字中断音/电音), 再做 3 帧中值去除跳点
void VoiceEngine::smooth_f0(int from) {
    const int P = p_len_;
    from = std::max(1, from - 4);
    std::vector<float>& f = f0cache_;
    for (int i = from; i < P; ++i) {
        if (f[i] > 0 || f[i - 1] <= 0) continue;
        int j = i;
        while (j < P && f[j] <= 0) ++j;
        if (j < P && j - i <= 3) {
            double a = std::log(f[i - 1]), b = std::log(f[j]);
            for (int k = i; k < j; ++k) f[k] = float(std::exp(a + (b - a) * double(k - i + 1) / (j - i + 1)));
        }
        i = j;
    }
    // 八度/跳点纠错: 与前 150ms 有声帧的中位音高比较; 接近 ±1 个八度则折回;
    // 很轻的帧 (< -50dB, 字尾/气声, RMVPE 不可靠) 若偏离超过 4 个半音则沿用参考音高
    for (int i = from; i < P; ++i) {
        if (f[i] <= 0) continue;
        float buf[15];
        int n = 0;
        for (int k = std::max(0, i - 15); k < i; ++k) if (f[k] > 0) buf[n++] = f[k];
        if (n < 5) continue;
        std::nth_element(buf, buf + n / 2, buf + n);
        const double ref = buf[n / 2];
        double d = 1200.0 * std::log2(f[i] / ref);
        if (std::abs(d - 1200) < 300) { f[i] *= 0.5f; d -= 1200; }
        else if (std::abs(d + 1200) < 300) { f[i] *= 2.f; d += 1200; }
        if (std::abs(d) > 400) {
            double e = 0;
            for (int k = i * 160; k < std::min(win16_, i * 160 + 160); ++k) e += double(in16_[k]) * in16_[k];
            if (to_db(float(std::sqrt(e / 160))) < -50.f) f[i] = float(ref);
        }
    }
    std::vector<float> src(f.begin() + (from - 1), f.end());
    for (int i = from; i < P - 1; ++i) {
        float a = src[i - from], b = src[i - from + 1], c = src[i - from + 2];
        if (a > 0 && b > 0 && c > 0) f[i] = std::max(std::min(a, b), std::min(std::max(a, b), c));
    }
}

int VoiceEngine::gpu_model_count() const {
    int n = 0;
    for (auto* m : {hubert_.get(), rmvpe_.get(), enc_.get(), dec_.get()}) n += (m && m->on_gpu()) ? 1 : 0;
    return n;
}
bool VoiceEngine::gpu_fell_back() const {
    for (auto* m : {hubert_.get(), rmvpe_.get(), enc_.get(), dec_.get()}) if (m && m->fell_back()) return true;
    return false;
}

bool VoiceEngine::warmup(std::string* err) {
    if (!ready()) return false;
    try {
    // 用低幅噪声跑完整推理, 让 GPU 编译好固定尺寸的计算图; 之后恢复干净状态
    std::vector<float> n(block_io_), o(block_io_);
    for (int i = 0; i < block_io_; ++i) n[i] = 0.01f * gauss((uint64_t)i * 7919u);
    force_infer_ = true;
    const int e0 = errors.load();
    for (int i = 0; i < 2; ++i) process(n.data(), o.data());
    force_infer_ = false;
    reset_state();
    vc_log("预热完成: " + std::to_string(gpu_model_count()) + "/4 个模型在显卡上" + (gpu_fell_back() ? " (部分已改用 CPU)" : ""));
    if (errors.load() != e0) { if (err) *err = "预热推理出错, 详见日志"; return false; }
    return true;
    } catch (const std::exception& e) {
        force_infer_ = false;
        if (err) *err = e.what();
        vc_log(std::string("预热失败: ") + e.what());
        return false;
    }
}
