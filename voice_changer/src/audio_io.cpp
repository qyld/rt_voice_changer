#define MINIAUDIO_IMPLEMENTATION
#include "audio_io.h"

#include <miniaudio.h>

#include <chrono>
#include <cstdlib>
#include <cstring>

#include "shm_output.h"
#include "vc_engine.h"

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

struct OutState {
    RingBuffer rb{1 << 18};
    std::atomic<bool> primed{false};
    int prefill = 0;
    int max_level = 0;
    std::atomic<int>* underruns = nullptr;
    std::atomic<bool>* enabled = nullptr;
};

struct AudioIOImpl {
    ma_context ctx{};
    bool ctx_ok = false;
    std::vector<ma_device_info> cap_info, play_info;
    ma_device cap{}, out{}, mon{};
    bool cap_ok = false, out_ok = false, mon_ok = false;
    RingBuffer in_rb{1 << 18};
    OutState o, m;
    aivc::ShmWriter shm;   // 给 OBS 插件的共享内存输出
};

namespace {
AudioIOImpl* g_impl = nullptr;
AudioIO* g_io = nullptr;

std::vector<float> g_test_input;  // 测试: 用音频文件循环代替麦克风 (环境变量 VC_TEST_INPUT)
size_t g_test_pos = 0;

void capture_cb(ma_device*, void*, const void* in, ma_uint32 n) {
    if (!g_impl) return;
    const float* src = (const float*)in;
    float tmp[4096];
    if (!g_test_input.empty() && n <= 4096) {
        for (ma_uint32 i = 0; i < n; ++i) { tmp[i] = g_test_input[g_test_pos]; g_test_pos = (g_test_pos + 1) % g_test_input.size(); }
        src = tmp;
    }
    if (g_impl->in_rb.write(src, n) < n) g_io->overruns++;
}

void play_common(OutState& s, float* out, ma_uint32 n) {
    if (s.enabled && !s.enabled->load()) {
        s.rb.skip(s.rb.size());
        std::memset(out, 0, n * sizeof(float));
        return;
    }
    size_t lvl = s.rb.size();
    if (!s.primed) {
        if ((int)lvl >= s.prefill) s.primed = true;
        else { std::memset(out, 0, n * sizeof(float)); return; }
    }
    if ((int)lvl > s.max_level) s.rb.skip(lvl - s.prefill);  // 延迟堆积: 追帧
    size_t got = s.rb.read(out, n);
    if (got < n) {
        std::memset(out + got, 0, (n - got) * sizeof(float));
        s.primed = false;
        if (s.underruns) (*s.underruns)++;
    }
}
void out_cb(ma_device*, void* out, const void*, ma_uint32 n) {
    if (g_impl) play_common(g_impl->o, (float*)out, n);
}
void mon_cb(ma_device*, void* out, const void*, ma_uint32 n) {
    if (g_impl) play_common(g_impl->m, (float*)out, n);
}
}  // namespace

AudioIO::AudioIO() : impl_(std::make_unique<AudioIOImpl>()) {}
AudioIO::~AudioIO() {
    stop();
    if (impl_->ctx_ok) ma_context_uninit(&impl_->ctx);
}

bool AudioIO::init(std::string& err) {
    ma_backend backends[] = {ma_backend_wasapi};
    if (std::getenv("VC_NULL_AUDIO")) backends[0] = ma_backend_null;  // 测试用: 模拟声卡
    if (ma_context_init(backends, 1, nullptr, &impl_->ctx) != MA_SUCCESS &&
        ma_context_init(nullptr, 0, nullptr, &impl_->ctx) != MA_SUCCESS) {
        err = "音频系统初始化失败";
        return false;
    }
    impl_->ctx_ok = true;
    refresh();
    return true;
}

void AudioIO::refresh() {
    ma_device_info *p = nullptr, *c = nullptr;
    ma_uint32 np = 0, nc = 0;
    caps_.clear();
    plays_.clear();
    if (ma_context_get_devices(&impl_->ctx, &p, &np, &c, &nc) != MA_SUCCESS) return;
    impl_->cap_info.assign(c, c + nc);
    impl_->play_info.assign(p, p + np);
    for (ma_uint32 i = 0; i < nc; ++i) caps_.push_back({c[i].name, (bool)c[i].isDefault});
    for (ma_uint32 i = 0; i < np; ++i) plays_.push_back({p[i].name, (bool)p[i].isDefault});
}

static bool open_play(ma_context* ctx, ma_device* dev, const ma_device_id* id, int rate, ma_device_data_proc cb) {
    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    dc.playback.pDeviceID = id;
    dc.playback.format = ma_format_f32;
    dc.playback.channels = 1;  // miniaudio 自动复制到立体声
    dc.sampleRate = rate;
    dc.periodSizeInMilliseconds = 10;
    dc.performanceProfile = ma_performance_profile_low_latency;
    dc.dataCallback = cb;
    dc.wasapi.noAutoConvertSRC = MA_FALSE;
    return ma_device_init(ctx, &dc, dev) == MA_SUCCESS;
}

bool AudioIO::start(VoiceEngine& eng, int cap_idx, int out_idx, int mon_idx, int out_buffer_ms, std::string& err) {
    stop();
    eng_ = &eng;
    g_impl = impl_.get();
    g_io = this;
    const int rate = eng.config().io_rate;
    auto& im = *impl_;
    im.in_rb.resize(rate * 4);
    for (OutState* s : {&im.o, &im.m}) {
        s->rb.resize(rate * 4);
        s->primed = false;
        s->prefill = rate * out_buffer_ms / 1000;
        s->max_level = s->prefill + eng.block_io() + rate / 10;
        s->underruns = &underruns;
    }
    im.m.enabled = &monitor_enabled;
    im.o.enabled = nullptr;
    underruns = 0;
    overruns = 0;

    ma_device_config dc = ma_device_config_init(ma_device_type_capture);
    dc.capture.pDeviceID = (cap_idx >= 0 && cap_idx < (int)im.cap_info.size()) ? &im.cap_info[cap_idx].id : nullptr;
    dc.capture.format = ma_format_f32;
    dc.capture.channels = 1;
    dc.sampleRate = rate;
    dc.periodSizeInMilliseconds = 10;
    dc.performanceProfile = ma_performance_profile_low_latency;
    dc.dataCallback = capture_cb;
    if (ma_device_init(&im.ctx, &dc, &im.cap) != MA_SUCCESS) { err = "无法打开麦克风设备"; return false; }
    im.cap_ok = true;
    if (out_idx >= 0 && out_idx < (int)im.play_info.size()) {
        if (!open_play(&im.ctx, &im.out, &im.play_info[out_idx].id, rate, out_cb)) { err = "无法打开输出设备"; stop(); return false; }
        im.out_ok = true;
    }
    im.shm.create();
    if (mon_idx >= 0 && mon_idx < (int)im.play_info.size() && mon_idx != out_idx) {
        if (open_play(&im.ctx, &im.mon, &im.play_info[mon_idx].id, rate, mon_cb)) im.mon_ok = true;
    }
    if (const char* tf = std::getenv("VC_TEST_INPUT"); tf && g_test_input.empty()) {
        ma_decoder_config dcf = ma_decoder_config_init(ma_format_f32, 1, rate);
        ma_decoder dec;
        if (ma_decoder_init_file(tf, &dcf, &dec) == MA_SUCCESS) {
            float b[4096];
            ma_uint64 got = 0;
            while (ma_decoder_read_pcm_frames(&dec, b, 4096, &got) == MA_SUCCESS && got) g_test_input.insert(g_test_input.end(), b, b + got);
            ma_decoder_uninit(&dec);
        }
    }
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    running_ = true;
    th_ = std::thread(&AudioIO::worker, this);
    if (im.out_ok) ma_device_start(&im.out);
    if (im.mon_ok) ma_device_start(&im.mon);
    ma_device_start(&im.cap);
    return true;
}

void AudioIO::stop() {
    auto& im = *impl_;
    if (im.cap_ok) { ma_device_uninit(&im.cap); im.cap_ok = false; }
    running_ = false;
    if (th_.joinable()) th_.join();
    if (im.out_ok) { ma_device_uninit(&im.out); im.out_ok = false; }
    if (im.mon_ok) { ma_device_uninit(&im.mon); im.mon_ok = false; }
    im.shm.close();
#ifdef _WIN32
    timeEndPeriod(1);
#endif
}

void AudioIO::worker() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
    auto& im = *impl_;
    const int B = eng_->block_io();
    const int rate = eng_->config().io_rate;
    std::vector<float> in(B), out(B);
    eng_->reset_state();
    while (running_) {
        size_t avail = im.in_rb.size();
        if ((int)avail < B) {
            im.shm.heartbeat();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if ((int)avail > 3 * B) im.in_rb.skip(avail - B);  // 处理跟不上时丢弃积压, 防止延迟越来越大
        im.in_rb.read(in.data(), B);
        auto t = std::chrono::steady_clock::now();
        eng_->process(in.data(), out.data());
        float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t).count();
        load = ms / (1000.f * B / rate);
        if (im.out_ok) im.o.rb.write(out.data(), B);
        im.shm.write(out.data(), B);
        if (im.mon_ok) im.m.rb.write(out.data(), B);
        buffered_ms = 1000.f * im.o.rb.size() / rate;
    }
}

bool AudioIO::obs_connected() const { return impl_->shm.reader_connected(); }

int AudioIO::find_device(bool capture, const std::string& name) const {
    const auto& v = capture ? caps_ : plays_;
    for (size_t i = 0; i < v.size(); ++i) if (v[i].name == name) return (int)i;
    return -1;
}

int AudioIO::find_virtual_mic_output() const {
    for (size_t i = 0; i < plays_.size(); ++i)
        if (plays_[i].name.find("AI变声器 内部输出") != std::string::npos) return (int)i;
    return -1;
}
