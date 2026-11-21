// shm_output.h — AI 变声器 → OBS 插件 的共享内存音频协议（header-only，Windows）
//
// 写端（主引擎 voice_changer）：ShmWriter::create() 后，在音频 worker 线程里
//   每产生一块变声输出（48 kHz 单声道 float32）就调用 write(ptr, n)；
//   write 内部会顺带刷新心跳。空闲时（无输出）也应每 ≤200ms 调一次 heartbeat()。
// 读端（OBS 插件 obs-ai-voice）：ShmReader::open() 后，周期性 read_new() 取新数据；
//   connected() 判断写端是否在线（心跳超过 1 秒未更新视为断开）。
//
// 内存布局（命名文件映射 Local\AIVoiceChanger_Out）：
//   [ShmHeader 256 字节][float data[capacity]]
//   data 是环形缓冲；write_pos 是累计写入的样本数（单调递增，64 位，不回绕），
//   样本 i 存在 data[i % capacity]。写端先写数据，再以 release 语义更新 write_pos；
//   读端 acquire 读取 write_pos 后拷贝，拷完再检查是否被写端覆盖（防撕裂读）。
//
// 单写端、多读端均可（读端只读，各自维护读位置）。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace aivc {

constexpr wchar_t  kShmName[]       = L"Local\\AIVoiceChanger_Out";
constexpr uint32_t kShmMagic        = 0x43564941u;  // 'AIVC'（小端内存序 "AIVC"）
constexpr uint32_t kShmVersion      = 1;
constexpr uint32_t kShmSampleRate   = 48000;
constexpr uint32_t kShmChannels     = 1;
constexpr uint32_t kShmCapacity     = kShmSampleRate * 2;  // 2 秒环形缓冲（样本数）
constexpr uint32_t kShmHeaderSize   = 256;
constexpr uint64_t kHeartbeatTimeoutMs = 1000;           // 心跳超时 → 视为断开
// 写端单次 write() 建议 ≤ 0.5 秒；读端把"距写位置超过 capacity - 0.5s"的数据视为可能已被覆盖。
constexpr uint32_t kShmSafeSpan = kShmCapacity - kShmSampleRate / 2;

static_assert(std::atomic<uint64_t>::is_always_lock_free, "需要无锁 64 位原子");

struct ShmHeader {
    uint32_t magic;          // kShmMagic（写端最后写入，表示头部已初始化）
    uint32_t version;        // kShmVersion
    uint32_t sample_rate;    // 48000
    uint32_t channels;       // 1
    uint32_t capacity;       // 环形缓冲容量（样本/帧数）
    uint32_t header_size;    // 数据区偏移 = 256
    uint32_t writer_pid;     // 写端进程 ID（调试用）
    uint32_t reserved0;
    std::atomic<uint64_t> session;       // 写端每次 create 生成的新会话号（重启检测）
    std::atomic<uint64_t> write_pos;     // 累计写入样本数
    std::atomic<uint64_t> heartbeat_ms;  // 写端最近一次心跳，GetTickCount64() 毫秒
    std::atomic<uint64_t> reader_heartbeat_ms;  // 读端 (OBS 插件) 最近一次读取时间, 写端据此显示"OBS 已连接"
    uint8_t  pad[kShmHeaderSize - 64];
};
static_assert(sizeof(ShmHeader) == kShmHeaderSize, "ShmHeader 必须为 256 字节");

constexpr size_t kShmTotalSize = kShmHeaderSize + size_t(kShmCapacity) * sizeof(float);

inline uint64_t shm_now_ms() { return GetTickCount64(); }

// ---------------------------------------------------------------- 写端
class ShmWriter {
public:
    ShmWriter() = default;
    ~ShmWriter() { close(); }
    ShmWriter(const ShmWriter&) = delete;
    ShmWriter& operator=(const ShmWriter&) = delete;

    // 创建（或接管已存在的）映射并初始化头部。失败返回 false。
    bool create(const wchar_t* name = kShmName) {
        close();
        map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                  DWORD(kShmTotalSize), name);
        if (!map_) return false;
        void* p = MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, kShmTotalSize);
        if (!p) { close(); return false; }
        hdr_  = static_cast<ShmHeader*>(p);
        data_ = reinterpret_cast<float*>(static_cast<uint8_t*>(p) + kShmHeaderSize);

        // 先让旧读端看到 magic 失效，再初始化
        hdr_->magic = 0;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        hdr_->version     = kShmVersion;
        hdr_->sample_rate = kShmSampleRate;
        hdr_->channels    = kShmChannels;
        hdr_->capacity    = kShmCapacity;
        hdr_->header_size = kShmHeaderSize;
        hdr_->writer_pid  = GetCurrentProcessId();
        std::memset(data_, 0, size_t(kShmCapacity) * sizeof(float));
        pos_ = 0;
        LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
        uint64_t sess = (uint64_t(GetCurrentProcessId()) << 32) ^ uint64_t(qpc.QuadPart);
        if (sess == 0) sess = 1;
        hdr_->write_pos.store(0, std::memory_order_relaxed);
        hdr_->session.store(sess, std::memory_order_relaxed);
        hdr_->heartbeat_ms.store(shm_now_ms(), std::memory_order_relaxed);
        hdr_->reader_heartbeat_ms.store(0, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        reinterpret_cast<std::atomic<uint32_t>*>(&hdr_->magic)
            ->store(kShmMagic, std::memory_order_release);
        return true;
    }

    void close() {
        if (hdr_) {
            // 主动标记离线，读端立即感知
            hdr_->heartbeat_ms.store(0, std::memory_order_release);
            UnmapViewOfFile(hdr_);
        }
        hdr_ = nullptr; data_ = nullptr;
        if (map_) { CloseHandle(map_); map_ = nullptr; }
    }

    bool is_open() const { return hdr_ != nullptr; }

    // 写入 n 个单声道 float 样本（48 kHz）。n 超过容量时只保留最后 capacity 个。
    void write(const float* s, size_t n) {
        if (!hdr_ || !s || n == 0) return;
        if (n > kShmCapacity) { s += n - kShmCapacity; pos_ += n - kShmCapacity; n = kShmCapacity; }
        size_t idx   = size_t(pos_ % kShmCapacity);
        size_t first = n < kShmCapacity - idx ? n : kShmCapacity - idx;
        std::memcpy(data_ + idx, s, first * sizeof(float));
        if (n > first) std::memcpy(data_, s + first, (n - first) * sizeof(float));
        pos_ += n;
        hdr_->write_pos.store(pos_, std::memory_order_release);
        heartbeat();
    }

    // 刷新心跳（无音频输出时也要周期调用，否则读端 1 秒后判定断开）
    void heartbeat() {
        if (hdr_) hdr_->heartbeat_ms.store(shm_now_ms(), std::memory_order_release);
    }

    uint64_t write_pos() const { return pos_; }

    // 是否有读端 (OBS 插件) 在 1 秒内读取过
    bool reader_connected() const {
        if (!hdr_) return false;
        uint64_t hb = hdr_->reader_heartbeat_ms.load(std::memory_order_acquire), now = shm_now_ms();
        return hb != 0 && (now < hb || now - hb <= kHeartbeatTimeoutMs);
    }

private:
    HANDLE     map_  = nullptr;
    ShmHeader* hdr_  = nullptr;
    float*     data_ = nullptr;
    uint64_t   pos_  = 0;
};

// ---------------------------------------------------------------- 读端
class ShmReader {
public:
    struct Stats {
        uint64_t frames_read = 0;     // 累计读出样本数
        uint64_t frames_skipped = 0;  // 因落后过多/被覆盖而丢弃的样本数
        uint32_t resyncs = 0;         // 跳到最新的次数（含写端重启）
    };

    ShmReader() = default;
    ~ShmReader() { close(); }
    ShmReader(const ShmReader&) = delete;
    ShmReader& operator=(const ShmReader&) = delete;

    // max_lag：读端落后超过该样本数则跳到最新（只保留 keep 个最新样本）
    // 默认 200ms / 20ms，直播场景优先低延迟。
    void set_lag_policy(uint32_t max_lag, uint32_t keep) {
        if (max_lag > kShmSafeSpan) max_lag = kShmSafeSpan;
        max_lag_ = max_lag; keep_ = keep < max_lag ? keep : max_lag;
    }

    // 打开映射（写端尚未创建时返回 false，可稍后重试）。
    bool open(const wchar_t* name = kShmName) {
        close();
        // 以读写方式打开 (只写读端心跳字段, 不改音频数据); 失败则退回只读
        map_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
        writable_ = map_ != nullptr;
        if (!map_) map_ = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (!map_) return false;
        void* p = MapViewOfFile(map_, writable_ ? (FILE_MAP_READ | FILE_MAP_WRITE) : FILE_MAP_READ, 0, 0, kShmTotalSize);
        if (!p) { close(); return false; }
        hdr_  = static_cast<const ShmHeader*>(p);
        data_ = reinterpret_cast<const float*>(static_cast<const uint8_t*>(p) + kShmHeaderSize);
        session_ = 0;  // 首次 read_new 时同步到最新
        return true;
    }

    void close() {
        if (hdr_) UnmapViewOfFile(hdr_);
        hdr_ = nullptr; data_ = nullptr; session_ = 0;
        if (map_) { CloseHandle(map_); map_ = nullptr; }
    }

    bool is_open() const { return hdr_ != nullptr; }

    // 头部合法且心跳未超时
    bool connected() const {
        if (!header_valid()) return false;
        uint64_t hb = hdr_->heartbeat_ms.load(std::memory_order_acquire);
        if (hb == 0) return false;
        uint64_t now = shm_now_ms();
        return now < hb || now - hb <= kHeartbeatTimeoutMs;
    }

    // 距最近一次心跳的毫秒数（未打开/无效返回 UINT64_MAX）
    uint64_t heartbeat_age_ms() const {
        if (!header_valid()) return UINT64_MAX;
        uint64_t hb = hdr_->heartbeat_ms.load(std::memory_order_acquire), now = shm_now_ms();
        return hb == 0 ? UINT64_MAX : (now > hb ? now - hb : 0);
    }

    // 读出自上次以来的新样本，最多 max_frames 个，返回实际个数。
    // 首次调用 / 写端重启时同步到最新位置（不回放旧数据）。
    // 落后 > max_lag 时跳到 write_pos - keep。断开时返回 0。
    size_t read_new(float* out, size_t max_frames) {
        if (!connected() || !out || max_frames == 0) return 0;
        if (writable_)
            const_cast<ShmHeader*>(hdr_)->reader_heartbeat_ms.store(shm_now_ms(), std::memory_order_release);
        uint64_t sess = hdr_->session.load(std::memory_order_acquire);
        uint64_t wp   = hdr_->write_pos.load(std::memory_order_acquire);
        if (sess != session_ || wp < rp_) {       // 首次打开或写端重启
            session_ = sess;
            rp_ = wp > keep_ ? wp - keep_ : 0;
            if (rp_ > wp) rp_ = wp;
            ++stats_.resyncs;
        }
        uint64_t lag = wp - rp_;
        if (lag > max_lag_) {                    // 落后过多：跳到最新
            uint64_t nrp = wp - keep_;
            stats_.frames_skipped += nrp - rp_;
            rp_ = nrp; lag = keep_;
            ++stats_.resyncs;
        }
        size_t n = size_t(lag < max_frames ? lag : max_frames);
        if (n == 0) return 0;
        size_t idx   = size_t(rp_ % kShmCapacity);
        size_t first = n < kShmCapacity - idx ? n : kShmCapacity - idx;
        std::memcpy(out, data_ + idx, first * sizeof(float));
        if (n > first) std::memcpy(out + first, data_, (n - first) * sizeof(float));
        // 撕裂检测：拷贝期间写端若已（或正在，未发布的块）覆盖 [rp_, rp_+n)，则整段丢弃
        std::atomic_thread_fence(std::memory_order_acquire);
        uint64_t wp2 = hdr_->write_pos.load(std::memory_order_acquire);
        if (hdr_->session.load(std::memory_order_acquire) != session_ ||
            wp2 - rp_ > kShmSafeSpan) {
            uint64_t nrp = wp2 > keep_ ? wp2 - keep_ : 0;
            stats_.frames_skipped += nrp > rp_ ? nrp - rp_ : 0;
            rp_ = nrp; ++stats_.resyncs;
            return 0;
        }
        rp_ += n;
        stats_.frames_read += n;
        return n;
    }

    // 当前可读（未读）样本数
    uint64_t available() const {
        if (!header_valid()) return 0;
        uint64_t wp = hdr_->write_pos.load(std::memory_order_acquire);
        return wp > rp_ ? wp - rp_ : 0;
    }

    const Stats& stats() const { return stats_; }
    uint32_t writer_pid() const { return header_valid() ? hdr_->writer_pid : 0; }

private:
    bool writable_ = false;
    bool header_valid() const {
        if (!hdr_) return false;
        uint32_t m = reinterpret_cast<const std::atomic<uint32_t>*>(&hdr_->magic)
                         ->load(std::memory_order_acquire);
        return m == kShmMagic && hdr_->version == kShmVersion &&
               hdr_->sample_rate == kShmSampleRate && hdr_->channels == kShmChannels &&
               hdr_->capacity == kShmCapacity && hdr_->header_size == kShmHeaderSize;
    }

    HANDLE           map_  = nullptr;
    const ShmHeader* hdr_  = nullptr;
    const float*     data_ = nullptr;
    uint64_t         rp_   = 0;
    uint64_t         session_ = 0;
    uint32_t         max_lag_ = kShmSampleRate / 5;   // 200 ms
    uint32_t         keep_    = kShmSampleRate / 50;  // 20 ms
    Stats            stats_;
};

}  // namespace aivc
