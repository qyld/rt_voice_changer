// ShmWriter / ShmReader 单元测试：读写一致、环形回绕、落后跳帧、断开检测、写端重启、并发撕裂检测。
#include "shm_output.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (cond) ++g_pass;                                                            \
        else { ++g_fail; fprintf(stderr, "  失败 %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

using namespace aivc;
static const wchar_t* kTestName = L"Local\\AIVoiceChanger_UnitTest";
constexpr uint32_t kMod = 1u << 23;  // float 可精确表示的整数范围内取模
static float val(uint64_t i) { return float(i % kMod); }

static void write_seq(ShmWriter& w, uint64_t& next, size_t n) {
    std::vector<float> b(n);
    for (auto& v : b) v = val(next++);
    w.write(b.data(), n);
}

static bool is_seq(const float* p, size_t n, uint64_t start) {
    for (size_t i = 0; i < n; ++i)
        if (p[i] != val(start + i)) return false;
    return true;
}

static void test_open_missing() {
    puts("[open_missing] 写端不存在时 open 失败");
    ShmReader r;
    CHECK(!r.open(L"Local\\AIVoiceChanger_DoesNotExist"));
    CHECK(!r.connected());
    float x[4];
    CHECK(r.read_new(x, 4) == 0);
}

static void test_basic_and_wrap() {
    puts("[basic_wrap] 读写一致 + 环形回绕");
    ShmWriter w; CHECK(w.create(kTestName));
    ShmReader r; CHECK(r.open(kTestName));
    CHECK(r.connected());
    std::vector<float> out(8192);
    CHECK(r.read_new(out.data(), out.size()) == 0);  // 首次同步，无数据

    uint64_t next = 0, rd = 0;
    write_seq(w, next, 1000);
    size_t n = r.read_new(out.data(), out.size());
    CHECK(n == 1000);
    CHECK(is_seq(out.data(), n, 0));
    rd += n;

    // 不规则块大小写读 5 倍容量，跨越多次回绕；每次读完不落后
    std::mt19937 rng(1);
    bool ok = true;
    while (next < uint64_t(kShmCapacity) * 5) {
        write_seq(w, next, 1 + rng() % 3000);
        while ((n = r.read_new(out.data(), 1 + rng() % 4000)) > 0) {
            if (!is_seq(out.data(), n, rd)) ok = false;
            rd += n;
        }
    }
    CHECK(ok);
    CHECK(rd == next);
    CHECK(r.stats().frames_skipped == 0);
    // 请求量小于可读量时分批读出
    write_seq(w, next, 3000);
    CHECK(r.read_new(out.data(), 1000) == 1000 && is_seq(out.data(), 1000, rd)); rd += 1000;
    CHECK(r.available() == 2000);
    CHECK(r.read_new(out.data(), 5000) == 2000 && is_seq(out.data(), 2000, rd)); rd += 2000;
}

static void test_lag_skip() {
    puts("[lag_skip] 落后过多时跳到最新");
    ShmWriter w; CHECK(w.create(kTestName));
    ShmReader r; CHECK(r.open(kTestName));
    r.set_lag_policy(9600, 960);  // 落后 >200ms → 只保留最近 20ms
    std::vector<float> out(kShmCapacity);
    r.read_new(out.data(), out.size());

    uint64_t next = 0;
    write_seq(w, next, 9600);  // 恰好等于上限：不跳
    size_t n = r.read_new(out.data(), out.size());
    CHECK(n == 9600 && is_seq(out.data(), n, 0));
    CHECK(r.stats().frames_skipped == 0);

    uint64_t base = next;
    write_seq(w, next, 24000);  // 落后 500ms
    n = r.read_new(out.data(), out.size());
    CHECK(n == 960);
    CHECK(is_seq(out.data(), n, next - 960));  // 读到的是最新的 960 个
    CHECK(r.stats().frames_skipped == 24000 - 960);

    // 落后超过整个环形容量（数据已被覆盖）同样能恢复
    uint64_t skipped0 = r.stats().frames_skipped;
    write_seq(w, next, uint64_t(kShmCapacity) * 3 + 123);
    n = r.read_new(out.data(), out.size());
    CHECK(n == 960 && is_seq(out.data(), n, next - 960));
    CHECK(r.stats().frames_skipped - skipped0 == uint64_t(kShmCapacity) * 3 + 123 - 960);
    // 之后继续正常
    write_seq(w, next, 480);
    n = r.read_new(out.data(), out.size());
    CHECK(n == 480 && is_seq(out.data(), n, next - 480));
    (void)base;
}

static void test_disconnect() {
    puts("[disconnect] 心跳超时 / 写端关闭 / 恢复");
    ShmWriter w; CHECK(w.create(kTestName));
    ShmReader r; CHECK(r.open(kTestName));
    uint64_t next = 0;
    std::vector<float> out(4096);
    write_seq(w, next, 480);
    CHECK(r.connected());
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    CHECK(r.connected());  // 600ms < 1s 仍在线
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    CHECK(!r.connected());  // 1.2s 无心跳 → 断开
    CHECK(r.heartbeat_age_ms() > kHeartbeatTimeoutMs);
    CHECK(r.read_new(out.data(), out.size()) == 0);  // 断开时不读数据
    w.heartbeat();
    CHECK(r.connected());  // 心跳恢复
    w.close();
    CHECK(!r.connected());  // 写端关闭立即视为断开
}

static void test_writer_restart() {
    puts("[restart] 写端重启（读端持有旧句柄）");
    ShmReader r;
    uint64_t next = 0;
    std::vector<float> out(8192);
    {
        ShmWriter w; CHECK(w.create(kTestName));
        CHECK(r.open(kTestName));
        r.read_new(out.data(), out.size());
        write_seq(w, next, 5000);
        CHECK(r.read_new(out.data(), out.size()) == 5000);
    }  // 写端退出（读端仍持有映射）
    CHECK(!r.connected());
    ShmWriter w2; CHECK(w2.create(kTestName));  // 重新创建：复用同一内核对象，会话号变化
    CHECK(r.connected());
    uint32_t resyncs0 = r.stats().resyncs;
    uint64_t n2 = 0;
    write_seq(w2, n2, 300);  // write_pos 从 0 重新开始，小于旧读位置
    size_t n = r.read_new(out.data(), out.size());
    CHECK(r.stats().resyncs > resyncs0);
    CHECK(n == 300 && is_seq(out.data(), n, 0));
    write_seq(w2, n2, 100);
    n = r.read_new(out.data(), out.size());
    CHECK(n == 100 && is_seq(out.data(), n, 300));
}

static void test_concurrent() {
    puts("[concurrent] 跨线程实时读写，数据连续无撕裂（约 2 秒）");
    ShmWriter w; CHECK(w.create(kTestName));
    ShmReader r; CHECK(r.open(kTestName));
    std::vector<float> out(kShmCapacity);
    r.read_new(out.data(), out.size());
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> written{0};
    std::thread wt([&] {
        uint64_t next = 0; std::mt19937 rng(7);
        auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2)) {
            write_seq(w, next, 1 + rng() % 2000);  // 远快于实时，制造压力
            written.store(next);
            if (rng() % 8 == 0) std::this_thread::yield();
        }
        stop.store(true);
    });
    uint64_t chunks = 0, bad = 0, gaps = 0, rd_total = 0;
    float last = -1.0f;
    uint64_t prev_skipped = 0;
    while (!stop.load() || r.available() > 0) {
        size_t n = r.read_new(out.data(), 4096);
        if (n == 0) { std::this_thread::yield(); continue; }
        ++chunks; rd_total += n;
        for (size_t i = 1; i < n; ++i)
            if (out[i] != float((uint32_t(out[i - 1]) + 1) % kMod)) { ++bad; break; }
        // 块间连续，除非期间发生了跳帧
        bool skipped = r.stats().frames_skipped != prev_skipped;
        prev_skipped = r.stats().frames_skipped;
        if (last >= 0 && !skipped && out[0] != float((uint32_t(last) + 1) % kMod)) ++gaps;
        last = out[n - 1];
    }
    wt.join();
    printf("  写入 %llu, 读出 %llu, 块 %llu, 丢弃 %llu, 重同步 %u, 块内撕裂 %llu, 未计入的间断 %llu\n",
           (unsigned long long)written.load(), (unsigned long long)rd_total,
           (unsigned long long)chunks, (unsigned long long)r.stats().frames_skipped,
           r.stats().resyncs, (unsigned long long)bad, (unsigned long long)gaps);
    CHECK(chunks > 100);
    CHECK(bad == 0);
    CHECK(gaps == 0);
    CHECK(rd_total + r.stats().frames_skipped + 960 >= written.load());  // 首次同步 keep 之外全部计入
}

static void test_sine_realtime() {
    puts("[sine] 实时 440Hz 正弦：频率与幅度一致（约 1 秒）");
    ShmWriter w; CHECK(w.create(kTestName));
    ShmReader r; CHECK(r.open(kTestName));
    std::vector<float> tmp(4096), got;
    r.read_new(tmp.data(), tmp.size());
    std::thread wt([&] {
        double ph = 0, step = 2 * 3.14159265358979 * 440.0 / kShmSampleRate;
        std::vector<float> b(480);
        auto next = std::chrono::steady_clock::now();
        for (int k = 0; k < 100; ++k) {
            for (auto& v : b) { v = float(0.3 * std::sin(ph)); ph += step; }
            w.write(b.data(), b.size());
            next += std::chrono::milliseconds(10);
            std::this_thread::sleep_until(next);
        }
    });
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1300)) {
        size_t n = r.read_new(tmp.data(), tmp.size());
        got.insert(got.end(), tmp.begin(), tmp.begin() + n);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    wt.join();
    size_t zc = 0; double sq = 0; float pk = 0;
    for (size_t i = 0; i < got.size(); ++i) {
        sq += double(got[i]) * got[i];
        pk = std::max(pk, std::fabs(got[i]));
        if (i && got[i - 1] < 0 && got[i] >= 0) ++zc;
    }
    double secs = double(got.size()) / kShmSampleRate;
    double f = zc / secs, rms = std::sqrt(sq / std::max<size_t>(1, got.size()));
    printf("  读到 %zu 样本 (%.2fs), 估计频率 %.1f Hz, RMS %.4f, 峰值 %.4f, 丢弃 %llu\n", got.size(),
           secs, f, rms, pk, (unsigned long long)r.stats().frames_skipped);
    CHECK(got.size() >= 47000);
    CHECK(std::fabs(f - 440.0) < 5.0);
    CHECK(std::fabs(rms - 0.3 / std::sqrt(2.0)) < 0.01);
    CHECK(r.stats().frames_skipped == 0);
}

int main() {
    test_open_missing();
    test_basic_and_wrap();
    test_lag_skip();
    test_disconnect();
    test_writer_restart();
    test_concurrent();
    test_sine_realtime();
    printf("\n结果：通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
