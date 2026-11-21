#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ring_buffer.h"

class VoiceEngine;
struct AudioIOImpl;

struct AudioDeviceDesc {
    std::string name;
    bool is_default = false;
};

class AudioIO {
public:
    AudioIO();
    ~AudioIO();
    bool init(std::string& err);
    const std::vector<AudioDeviceDesc>& capture_devices() const { return caps_; }
    const std::vector<AudioDeviceDesc>& playback_devices() const { return plays_; }
    void refresh();

    // out_idx: 输出设备 (直播一般选虚拟声卡 CABLE Input), mon_idx: 监听耳机 (-1 不监听)
    bool start(VoiceEngine& eng, int cap_idx, int out_idx, int mon_idx, int out_buffer_ms, std::string& err);
    void stop();
    bool running() const { return running_; }

    std::atomic<int> underruns{0}, overruns{0};
    std::atomic<float> buffered_ms{0};
    std::atomic<float> load{0};   // 推理耗时 / 块时长
    std::atomic<bool> monitor_enabled{true};

private:
    friend struct AudioIOImpl;
    void worker();
    std::unique_ptr<AudioIOImpl> impl_;
    std::vector<AudioDeviceDesc> caps_, plays_;
    std::atomic<bool> running_{false};
    std::thread th_;
    VoiceEngine* eng_ = nullptr;
};
