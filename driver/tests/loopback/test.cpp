// 用户态单元测试：模拟 WaveRT 渲染/采集按时间推进，验证环形缓冲行为。
#include "sysvad.h"
#include <initguid.h>
#include <ksmedia.h>
#include "aivcloopback.h"
#include <stdio.h>
#include <math.h>
#include <vector>

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static WAVEFORMATEXTENSIBLE MakeFmt(int rate, int ch, int container, int valid)
{
    WAVEFORMATEXTENSIBLE w = {};
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels = (WORD)ch;
    w.Format.nSamplesPerSec = rate;
    w.Format.wBitsPerSample = (WORD)container;
    w.Format.nBlockAlign = (WORD)(ch * container / 8);
    w.Format.nAvgBytesPerSec = rate * w.Format.nBlockAlign;
    w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w.Samples.wValidBitsPerSample = (WORD)valid;
    w.dwChannelMask = ch == 2 ? KSAUDIO_SPEAKER_STEREO : KSAUDIO_SPEAKER_MONO;
    w.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    return w;
}

static void PutSample(BYTE *p, int bps, double v)   // v in [-1,1)
{
    LONG s = (LONG)(v * 2147483647.0);
    ULONG u = (ULONG)s;
    if (bps == 2) { p[0] = (BYTE)(u >> 16); p[1] = (BYTE)(u >> 24); }
    else if (bps == 3) { p[0] = (BYTE)(u >> 8); p[1] = (BYTE)(u >> 16); p[2] = (BYTE)(u >> 24); }
    else { memcpy(p, &u, 4); }
}

static double GetSample(const BYTE *p, int bps)
{
    ULONG u;
    if (bps == 2) u = ((ULONG)p[0] << 16) | ((ULONG)p[1] << 24);
    else if (bps == 3) u = ((ULONG)p[0] << 8) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 24);
    else memcpy(&u, p, 4);
    return (LONG)u / 2147483648.0;
}

// 模拟：渲染端以 wRate 写入 440Hz 正弦，采集端以 rRate 读出；两端都按"毫秒字节位移 + 余数结转"
// 的方式（与 minwavertstream.cpp UpdatePosition 一致）推进，并且调用时刻错开、块大小不规则。
static void RunScenario(const char *name, int wRate, int wCh, int wBits, int wValid,
                        int rRate, int rCh, int rBits, int rValid, int ms, int stopRenderAtMs)
{
    printf("[%s] render %d/%dch/%d -> capture %d/%dch/%d\n", name, wRate, wCh, wBits, rRate, rCh, rBits);
    WAVEFORMATEXTENSIBLE wf = MakeFmt(wRate, wCh, wBits, wValid);
    WAVEFORMATEXTENSIBLE rf = MakeFmt(rRate, rCh, rBits, rValid);
    AIVC_PCM_FORMAT wp, rp;
    CHECK(AivcParseFormat(&wf.Format, &wp), "parse render fmt");
    CHECK(AivcParseFormat(&rf.Format, &rp), "parse capture fmt");

    static CAivcLoopbackWriter writer;   // 实际驱动中是流对象成员
    static CAivcLoopbackReader reader;
    writer.Reset(&wp);
    reader.Reset(&rp);
    reader.Start();

    const double freq = 440.0;
    ULONGLONG wFrames = 0;
    ULONG wCarry = 0, rCarry = 0;
    int wLast = 0, rLast = 0;
    std::vector<BYTE> wbuf(1 << 16), rbuf(1 << 16);
    std::vector<double> out;   // 采集到的左声道（或单声道）
    int wbps = wBits / 8, rbps = rBits / 8;
    ULONG wPartial = 0;        // 字节流中尚未构成整帧的字节数（模拟 DMA 字节级推进）
    BYTE wCarryBytes[8] = {};

    for (int t = 1; t <= ms; t++)
    {
        // 渲染端：每 1~3ms 一次位置更新
        if (t < stopRenderAtMs && (t % 3 == 0 || t % 7 == 1))
        {
            int elapsed = t - wLast; wLast = t;
            ULONG bytes = (wf.Format.nAvgBytesPerSec * elapsed + wCarry) / 1000;
            wCarry = (wf.Format.nAvgBytesPerSec * elapsed + wCarry) % 1000;
            // 生成连续字节流
            ULONG need = bytes;
            ULONG produced = 0;
            // 先放上次剩余的半帧字节
            memcpy(wbuf.data(), wCarryBytes, wPartial);
            produced = wPartial;
            while (produced < need)
            {
                BYTE frame[8];
                double v = 0.5 * sin(2 * 3.14159265358979 * freq * (double)wFrames / wRate);
                for (int c = 0; c < wCh; c++) PutSample(frame + c * wbps, wbps, v);
                wFrames++;
                memcpy(wbuf.data() + produced, frame, wf.Format.nBlockAlign);
                produced += wf.Format.nBlockAlign;
            }
            wPartial = produced - need;
            memcpy(wCarryBytes, wbuf.data() + need, wPartial);
            writer.Write(wbuf.data(), need);
        }
        // 采集端：每 2ms 或 5ms 一次
        if (t % 2 == 0 || t % 5 == 0)
        {
            int elapsed = t - rLast; rLast = t;
            ULONG bytes = (rf.Format.nAvgBytesPerSec * elapsed + rCarry) / 1000;
            rCarry = (rf.Format.nAvgBytesPerSec * elapsed + rCarry) % 1000;
            reader.Read(rbuf.data(), bytes);
            static std::vector<BYTE> acc;
            acc.insert(acc.end(), rbuf.begin(), rbuf.begin() + bytes);
            size_t frames = acc.size() / rf.Format.nBlockAlign;
            for (size_t i = 0; i < frames; i++) out.push_back(GetSample(acc.data() + i * rf.Format.nBlockAlign, rbps));
            acc.erase(acc.begin(), acc.begin() + frames * rf.Format.nBlockAlign);
            if (t == ms) acc.clear();
        }
    }

    // 分析：首个非零样本时刻（预缓冲延迟）、最大相邻样本跳变（爆音检测）、最大幅度、结尾是否静音
    size_t first = 0;
    while (first < out.size() && fabs(out[first]) < 1e-6) first++;
    double maxJump = 0, peak = 0;
    for (size_t i = first + 1; i < out.size(); i++)
    {
        maxJump = fmax(maxJump, fabs(out[i] - out[i - 1]));
        peak = fmax(peak, fabs(out[i]));
    }
    // 440Hz、幅度 0.5 的正弦，在 44.1k 下相邻样本最大差约 0.5*2*pi*440/44100 = 0.031
    double expectJump = 0.5 * 2 * 3.14159265358979 * freq / rRate;
    double startMs = first * 1000.0 / rRate;
    printf("  captured %zu frames, first sound at %.1f ms, peak %.3f, max step %.4f (sine bound %.4f)\n",
           out.size(), startMs, peak, maxJump, expectJump);
    CHECK(first < out.size(), "no sound captured");
    CHECK(startMs >= 25 && startMs <= 45, "prefill delay %.1f ms outside 25..45", startMs);
    CHECK(peak > 0.45 && peak < 0.51, "peak %.3f", peak);
    CHECK(maxJump < expectJump * 1.5, "discontinuity: step %.4f", maxJump);
    // 掉音检测：从开始出声后 10ms 到渲染停止前，每个 3ms 窗口都应有接近满幅的样本
    {
        size_t win = (size_t)(rRate * 3 / 1000);
        size_t endFrame = (size_t)((double)min(ms, stopRenderAtMs) * rRate / 1000) - (size_t)(rRate * 0.06);
        int dropouts = 0;
        for (size_t s = first + rRate / 100; s + win < min(endFrame, out.size()); s += win)
        {
            double p = 0;
            for (size_t i = s; i < s + win; i++) p = fmax(p, fabs(out[i]));
            if (p < 0.45) dropouts++;
        }
        printf("  dropout windows: %d\n", dropouts);
        CHECK(dropouts == 0, "%d dropout windows", dropouts);
    }
    CHECK(out.size() > (size_t)((double)ms * rRate / 1000 * 0.99) && out.size() < (size_t)((double)ms * rRate / 1000 * 1.01), "frame count %zu off", out.size());
    if (stopRenderAtMs < ms)
    {
        size_t tail = out.size() - (size_t)(rRate * 0.05);
        double tailPeak = 0;
        for (size_t i = tail; i < out.size(); i++) tailPeak = fmax(tailPeak, fabs(out[i]));
        printf("  after render stopped: tail peak %.6f\n", tailPeak);
        CHECK(tailPeak == 0.0, "not silent after render stop");
    }
}

// 两个读者互不影响；采集端在渲染端未运行时只输出静音
static void TestMultiReaderAndSilence()
{
    printf("[multi-reader / silence]\n");
    WAVEFORMATEXTENSIBLE f = MakeFmt(48000, 2, 16, 16);
    AIVC_PCM_FORMAT p; AivcParseFormat(&f.Format, &p);
    CAivcLoopbackReader r1, r2;
    r1.Reset(&p); r2.Reset(&p);
    std::vector<BYTE> b(19200);
    r1.Read(b.data(), (ULONG)b.size());
    bool silent = true;
    for (BYTE x : b) if (x) silent = false;
    CHECK(silent, "reader without writer must be silent");

    CAivcLoopbackWriter w; w.Reset(&p);
    std::vector<BYTE> wb(48 * 4 * 100);   // 100ms
    for (size_t i = 0; i < wb.size(); i += 2) { SHORT s = 10000; memcpy(&wb[i], &s, 2); }
    w.Write(wb.data(), (ULONG)wb.size());
    std::vector<BYTE> o1(48 * 4 * 60), o2(48 * 4 * 60);
    r1.Read(o1.data(), (ULONG)o1.size());
    r2.Read(o2.data(), (ULONG)o2.size());
    CHECK(memcmp(o1.data(), o2.data(), o1.size()) == 0, "two readers should get identical data");
    SHORT last; memcpy(&last, &o1[o1.size() - 2], 2);
    CHECK(last == 10000, "reader should reach full gain, got %d", last);

    // 非法格式
    WAVEFORMATEXTENSIBLE bad = MakeFmt(48000, 6, 16, 16);
    AIVC_PCM_FORMAT bp;
    CHECK(!AivcParseFormat(&bad.Format, &bp), "6ch must be rejected");
    bad = MakeFmt(48000, 2, 8, 8);
    CHECK(!AivcParseFormat(&bad.Format, &bp), "8bit must be rejected");
}

int main()
{
    if (AivcLoopbackInitialize() != STATUS_SUCCESS) { printf("init failed\n"); return 1; }
    RunScenario("same-format",   48000, 1, 16, 16, 48000, 2, 16, 16, 3000, 1 << 30);
    RunScenario("mono->stereo24",48000, 1, 16, 16, 48000, 2, 24, 24, 3000, 1 << 30);
    RunScenario("44k1->48k",     44100, 2, 16, 16, 48000, 1, 32, 24, 3000, 1 << 30);
    RunScenario("48k->44k1",     48000, 2, 32, 32, 44100, 2, 24, 24, 3000, 1 << 30);
    RunScenario("render stops",  48000, 1, 16, 16, 48000, 1, 16, 16, 2000, 1000);
    TestMultiReaderAndSilence();
    AivcLoopbackCleanup();
    printf(g_fail ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}

