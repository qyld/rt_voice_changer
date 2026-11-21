// test_writer — 用 ShmWriter 持续写入 440Hz 正弦（48kHz 单声道），模拟 AI 变声器主程序。
// 用法： test_writer.exe [秒数，默认无限] [频率Hz，默认440] [幅度，默认0.3]
// 启动后在 OBS 中添加 "AI 变声器" 音频源，即可听到/看到 440Hz 音调。
#include "shm_output.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

int main(int argc, char** argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 0.0;
    double freq    = argc > 2 ? atof(argv[2]) : 440.0;
    double amp     = argc > 3 ? atof(argv[3]) : 0.3;

    aivc::ShmWriter w;
    if (!w.create()) {
        fprintf(stderr, "创建共享内存失败, GetLastError=%lu\n", GetLastError());
        return 1;
    }
    printf("test_writer: 正在写入 %.1f Hz 正弦到 Local\\AIVoiceChanger_Out (pid=%lu)%s\n", freq,
           GetCurrentProcessId(), seconds > 0 ? "" : "，Ctrl+C 退出");
    fflush(stdout);

    timeBeginPeriod(1);
    constexpr size_t kBlock = 480;  // 10 ms
    std::vector<float> buf(kBlock);
    const double step  = 2.0 * 3.14159265358979323846 * freq / aivc::kShmSampleRate;
    double       phase = 0.0;
    uint64_t     total = 0;
    auto         t0 = std::chrono::steady_clock::now(), next = t0;

    while (seconds <= 0 || total < uint64_t(seconds * aivc::kShmSampleRate)) {
        for (auto& v : buf) {
            v = float(amp * std::sin(phase));
            phase += step;
            if (phase > 2.0 * 3.14159265358979323846) phase -= 2.0 * 3.14159265358979323846;
        }
        w.write(buf.data(), kBlock);
        total += kBlock;
        if (total % (aivc::kShmSampleRate * 5) == 0) {
            printf("已写入 %.0f 秒\n", double(total) / aivc::kShmSampleRate);
            fflush(stdout);
        }
        next += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(next);
    }
    timeEndPeriod(1);
    printf("完成，共写入 %llu 样本\n", (unsigned long long)total);
    return 0;
}
