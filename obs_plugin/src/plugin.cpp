// obs-ai-voice — OBS Studio 插件：从共享内存读取 AI 变声器输出，作为音频源推给 OBS。
// 无需任何虚拟声卡。协议见 voice_changer/src/shm_output.h。
#include <obs-module.h>
#include <util/platform.h>

#include "shm_output.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

OBS_DECLARE_MODULE()

#define PLUGIN_NAME    "obs-ai-voice"
#define PLUGIN_VERSION "2.0.0"
#define SOURCE_ID      "ai_voice_changer_source"

#define LOGI(fmt, ...) blog(LOG_INFO, "[" PLUGIN_NAME "] " fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) blog(LOG_WARNING, "[" PLUGIN_NAME "] " fmt, ##__VA_ARGS__)

MODULE_EXPORT const char* obs_module_name(void) { return "AI Voice Changer"; }
MODULE_EXPORT const char* obs_module_description(void) {
    return "AI 变声器音频源：通过共享内存直接接收变声输出，无需虚拟声卡";
}

namespace {

constexpr uint32_t kTickMs        = 10;
constexpr uint32_t kSilenceFrames = aivc::kShmSampleRate * kTickMs / 1000;  // 480
constexpr uint32_t kReadChunk     = 4096;
constexpr uint64_t kNsPerSec      = 1000000000ull;
constexpr int64_t  kTsResyncNs    = 40 * 1000000ll;  // 时间戳偏离 >40ms 时重置时间轴

inline uint64_t frames_to_ns(uint64_t n) { return n * kNsPerSec / aivc::kShmSampleRate; }

struct AiVoiceSource {
    obs_source_t*     source = nullptr;
    std::thread       worker;
    std::atomic<bool> running{false};

    // 线程间共享的状态（供属性页显示）
    std::atomic<bool>     connected{false};
    std::atomic<uint32_t> writer_pid{0};
    std::atomic<uint64_t> frames_read{0};
    std::atomic<uint64_t> frames_skipped{0};
    std::atomic<uint32_t> connect_count{0};

    uint64_t next_ts = 0;  // 仅 worker 线程使用

    void push(const float* data, uint32_t frames) {
        uint64_t now  = os_gettime_ns();
        uint64_t dur  = frames_to_ns(frames);
        uint64_t cand = now > dur ? now - dur : 0;  // 这批样本大致在 dur 之前开始
        if (next_ts == 0 || std::llabs(int64_t(cand - next_ts)) > kTsResyncNs) next_ts = cand;

        obs_source_audio a = {};
        a.data[0]         = reinterpret_cast<const uint8_t*>(data);
        a.frames          = frames;
        a.speakers        = SPEAKERS_MONO;
        a.format          = AUDIO_FORMAT_FLOAT;
        a.samples_per_sec = aivc::kShmSampleRate;
        a.timestamp       = next_ts;
        obs_source_output_audio(source, &a);
        next_ts += dur;
    }

    void run() {
        timeBeginPeriod(1);
        aivc::ShmReader reader;
        std::vector<float> buf(kReadChunk);
        std::vector<float> silence(kSilenceFrames, 0.0f);
        auto     next_tick  = std::chrono::steady_clock::now();
        auto     last_open  = next_tick - std::chrono::seconds(10);
        auto     last_log   = next_tick;
        uint64_t log_frames = 0;
        double   log_sq     = 0.0;
        float    log_peak   = 0.0f;
        bool     was_conn   = false;

        while (running.load(std::memory_order_relaxed)) {
            auto now = std::chrono::steady_clock::now();
            if (!reader.connected() && now - last_open >= std::chrono::milliseconds(500)) {
                last_open = now;
                reader.open();  // 写端未启动时失败，稍后重试；写端重启时也重新映射
            }

            bool conn = reader.connected();
            if (conn != was_conn) {
                was_conn = conn;
                connected.store(conn);
                writer_pid.store(conn ? reader.writer_pid() : 0);
                if (conn) {
                    connect_count.fetch_add(1);
                    LOGI("已连接变声器写端 (pid=%u)", reader.writer_pid());
                } else {
                    LOGI("变声器写端已断开，输出静音");
                }
                next_ts = 0;
            }

            if (conn) {
                size_t n;
                while ((n = reader.read_new(buf.data(), buf.size())) > 0) {
                    for (size_t i = 0; i < n; ++i) {
                        float v = buf[i];
                        log_sq += double(v) * v;
                        float av = std::fabs(v);
                        if (av > log_peak) log_peak = av;
                    }
                    log_frames += n;
                    push(buf.data(), uint32_t(n));
                }
                frames_read.store(reader.stats().frames_read);
                frames_skipped.store(reader.stats().frames_skipped);
            } else {
                push(silence.data(), kSilenceFrames);
            }

            if (now - last_log >= std::chrono::seconds(10)) {
                if (conn && log_frames > 0) {
                    LOGI("10s 统计: 接收 %llu 样本, RMS=%.4f, 峰值=%.4f, 累计丢弃 %llu",
                         (unsigned long long)log_frames, std::sqrt(log_sq / double(log_frames)),
                         log_peak, (unsigned long long)reader.stats().frames_skipped);
                }
                last_log = now; log_frames = 0; log_sq = 0.0; log_peak = 0.0f;
            }

            next_tick += std::chrono::milliseconds(kTickMs);
            auto after = std::chrono::steady_clock::now();
            if (next_tick < after - std::chrono::milliseconds(100)) next_tick = after;  // 卡顿后不追帧
            std::this_thread::sleep_until(next_tick);
        }
        reader.close();
        timeEndPeriod(1);
    }

    std::string status_text() const {
        char s[512];
        if (connected.load()) {
            snprintf(s, sizeof(s),
                     "状态：已连接 ✅（变声器进程 PID %u）\n已接收 %.1f 秒音频，丢弃 %llu 样本，连接次数 %u",
                     writer_pid.load(), double(frames_read.load()) / aivc::kShmSampleRate,
                     (unsigned long long)frames_skipped.load(), connect_count.load());
        } else {
            snprintf(s, sizeof(s),
                     "状态：未连接 ⚠（正在输出静音）\n请启动 AI 变声器主程序并开始变声，插件会自动连接。");
        }
        return s;
    }
};

const char* src_get_name(void*) { return "AI 变声器"; }

void* src_create(obs_data_t*, obs_source_t* source) {
    auto* s   = new AiVoiceSource();
    s->source = source;
    s->running.store(true);
    s->worker = std::thread([s] { s->run(); });
    LOGI("音频源已创建: %s", obs_source_get_name(source));
    return s;
}

void src_destroy(void* data) {
    auto* s = static_cast<AiVoiceSource*>(data);
    s->running.store(false);
    if (s->worker.joinable()) s->worker.join();
    delete s;
}

bool on_refresh(obs_properties_t*, obs_property_t*, void*) { return true; }  // 返回 true → 重建属性页

obs_properties_t* src_get_properties(void* data) {
    auto*             s     = static_cast<AiVoiceSource*>(data);
    obs_properties_t* props = obs_properties_create();
    std::string       txt   = s ? s->status_text() : "状态：未知";
    obs_property_t*   p     = obs_properties_add_text(props, "status", txt.c_str(), OBS_TEXT_INFO);
    obs_property_text_set_info_type(p, (s && s->connected.load()) ? OBS_TEXT_INFO_NORMAL
                                                                   : OBS_TEXT_INFO_WARNING);
    obs_properties_add_button(props, "refresh", "刷新状态", on_refresh);
    obs_properties_add_text(props, "about",
                            "本音频源通过共享内存 Local\\AIVoiceChanger_Out 接收 48kHz 单声道变声输出，"
                            "无需安装 VB-CABLE 等虚拟声卡。",
                            OBS_TEXT_INFO);
    return props;
}

}  // namespace

bool obs_module_load(void) {
    static obs_source_info info = {};
    info.id             = SOURCE_ID;
    info.type           = OBS_SOURCE_TYPE_INPUT;
    info.output_flags   = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
    info.icon_type      = OBS_ICON_TYPE_AUDIO_INPUT;
    info.get_name       = src_get_name;
    info.create         = src_create;
    info.destroy        = src_destroy;
    info.get_properties = src_get_properties;
    obs_register_source(&info);
    LOGI("插件已加载 v%s，注册音频源 '%s'", PLUGIN_VERSION, SOURCE_ID);
    return true;
}

void obs_module_unload(void) { LOGI("插件已卸载"); }
