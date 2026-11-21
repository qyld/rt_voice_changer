// 命令行工具: 设备列表 / 文件转换 (与实时完全相同的流式算法, 用于试听和性能测试) / 控制台实时变声
#include <miniaudio.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "audio_io.h"
#include "vc_engine.h"

#ifdef _WIN32
#include <windows.h>
#endif

static fs::path exe_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
#else
    return fs::current_path();
#endif
}

static void usage() {
    std::printf(
        "用法:\n"
        "  vc_cli list                                     列出音频设备\n"
        "  vc_cli convert --voice <声音目录> --in <输入音频> --out <输出.wav> [参数]\n"
        "  vc_cli run --voice <声音目录> [--in 设备号] [--out 设备号] [--monitor 设备号] [参数]\n"
        "参数:\n"
        "  --key <半音>      变调, 男变女常用 +8~+12, 女变男 -8~-12 (默认 0)\n"
        "  --index <0..1>    音色检索强度 (默认 0.5)\n"
        "  --rms <0..1>      音量包络: 0=完全跟随原声 (默认 0.25)\n"
        "  --gate <dB>       噪声门限 (默认 -50, -100 关闭)\n"
        "  --protect <0..0.5> 清辅音保护 (默认 0.33)\n"
        "  --block <ms>      块长 (默认 250)   --extra <ms> 上下文 (默认 800)\n"
        "  --crossfade <ms>  (默认 50)         --threads <n> (默认自动)\n"
        "  --buffer <ms>     输出缓冲 (默认 40) --common <目录> 公共模型目录\n");
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    std::vector<std::wstring> args(wargv, wargv + wargc);
    auto u8 = [](const std::wstring& w) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n ? n - 1 : 0, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
        return s;
    };
#else
    std::vector<std::string> args(argv, argv + argc);
    auto u8 = [](const std::string& s) { return s; };
#endif
    if (args.size() < 2) { usage(); return 1; }
    std::string cmd = u8(args[1]);
    std::map<std::string, fs::path> opt;
    for (size_t i = 2; i + 1 < args.size(); i += 2) opt[u8(args[i])] = fs::path(args[i + 1]);
    auto getf = [&](const char* k, float d) { return opt.count(k) ? (float)std::atof(opt[k].string().c_str()) : d; };
    auto geti = [&](const char* k, int d) { return opt.count(k) ? std::atoi(opt[k].string().c_str()) : d; };

    if (cmd == "list") {
        AudioIO io;
        std::string err;
        if (!io.init(err)) { std::printf("%s\n", err.c_str()); return 1; }
        std::printf("== 输入设备 (麦克风) ==\n");
        for (size_t i = 0; i < io.capture_devices().size(); ++i)
            std::printf("  [%zu] %s%s\n", i, io.capture_devices()[i].name.c_str(), io.capture_devices()[i].is_default ? "  (默认)" : "");
        std::printf("== 输出设备 ==\n");
        for (size_t i = 0; i < io.playback_devices().size(); ++i)
            std::printf("  [%zu] %s%s\n", i, io.playback_devices()[i].name.c_str(), io.playback_devices()[i].is_default ? "  (默认)" : "");
        return 0;
    }
    if (cmd != "convert" && cmd != "run") { usage(); return 1; }
    if (!opt.count("--voice")) { usage(); return 1; }

    EngineConfig cfg;
    cfg.block_ms = geti("--block", 250);
    cfg.extra_ms = geti("--extra", 800);
    cfg.crossfade_ms = geti("--crossfade", 50);
    cfg.threads = geti("--threads", 0);
    cfg.lookahead_ms = geti("--lookahead", 80);
    cfg.io_rate = 48000;

    VoiceEngine eng;
    std::string err;
    fs::path common = opt.count("--common") ? opt["--common"] : exe_dir() / "models" / "common";
    auto t0 = std::chrono::steady_clock::now();
    eng.configure(cfg);
    if (!eng.load_common(common, cfg.threads, err) || !eng.load_voice(opt["--voice"], err)) {
        std::printf("%s\n", err.c_str());
        return 1;
    }
    eng.configure(cfg);
    std::printf("模型加载 %.2f 秒, 声音: %s (%d Hz)\n",
                std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count(),
                eng.voice().name.c_str(), eng.voice().sample_rate);
    eng.params.pitch_shift = getf("--key", 0);
    eng.params.index_rate = getf("--index", 0.5f);
    eng.params.rms_mix = getf("--rms", 0.25f);
    eng.params.gate_db = getf("--gate", -50);
    eng.params.protect = getf("--protect", 0.33f);

    if (cmd == "convert") {
        ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 1, cfg.io_rate);
        ma_decoder dec;
#ifdef _WIN32
        if (ma_decoder_init_file_w(opt["--in"].wstring().c_str(), &dc, &dec) != MA_SUCCESS) {
#else
        if (ma_decoder_init_file(opt["--in"].string().c_str(), &dc, &dec) != MA_SUCCESS) {
#endif
            std::printf("无法读取输入音频\n");
            return 1;
        }
        std::vector<float> x;
        float buf[4096];
        ma_uint64 got = 0;
        while (ma_decoder_read_pcm_frames(&dec, buf, 4096, &got) == MA_SUCCESS && got > 0) x.insert(x.end(), buf, buf + got);
        ma_decoder_uninit(&dec);
        const int B = eng.block_io();
        const int delay = (cfg.crossfade_ms + cfg.search_ms + cfg.lookahead_ms) * cfg.io_rate / 1000;  // SOLA 引入的固定延迟, 对齐用
        size_t n_in = x.size();
        x.resize(((n_in + delay) / B + 2) * B, 0.f);
        std::vector<float> y(x.size());
        double worst = 0, total = 0;
        int nb = 0;
        for (size_t p = 0; p + B <= x.size(); p += B) {
            auto t = std::chrono::steady_clock::now();
            eng.process(&x[p], &y[p]);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
            if (nb > 0) { worst = std::max(worst, ms); total += ms; }
            ++nb;
        }
        double blk_ms = 1000.0 * B / cfg.io_rate;
        std::printf("块长 %.0f ms, 平均推理 %.1f ms, 最慢 %.1f ms, 实时率 RTF=%.3f (须 <1, 建议 <0.7)\n", blk_ms,
                    total / std::max(1, nb - 1), worst, total / std::max(1, nb - 1) / blk_ms);
        std::printf("  分项 (最后一块): HuBERT %.1f ms, RMVPE %.1f ms, 编码器 %.1f ms, 编码+声码器 %.1f ms\n",
                    eng.stats.hubert_ms.load(), eng.stats.f0_ms.load(), eng.stats.enc_ms.load(), eng.stats.synth_ms.load());
        ma_encoder_config ec = ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, 1, cfg.io_rate);
        ma_encoder enc;
#ifdef _WIN32
        if (ma_encoder_init_file_w(opt["--out"].wstring().c_str(), &ec, &enc) != MA_SUCCESS) {
#else
        if (ma_encoder_init_file(opt["--out"].string().c_str(), &ec, &enc) != MA_SUCCESS) {
#endif
            std::printf("无法写输出文件\n");
            return 1;
        }
        std::vector<int16_t> s16(n_in);
        for (size_t i = 0; i < n_in; ++i) s16[i] = (int16_t)std::lround(std::clamp(y[i + delay], -1.f, 1.f) * 32767.f);
        ma_encoder_write_pcm_frames(&enc, s16.data(), n_in, nullptr);
        ma_encoder_uninit(&enc);
        std::printf("已写出 %s\n", opt["--out"].u8string().c_str());
        return 0;
    }

    // run: 控制台实时
    AudioIO io;
    if (!io.init(err)) { std::printf("%s\n", err.c_str()); return 1; }
    auto find_default = [](const std::vector<AudioDeviceDesc>& v) {
        for (size_t i = 0; i < v.size(); ++i) if (v[i].is_default) return (int)i;
        return -1;
    };
    int ci = geti("--in", find_default(io.capture_devices()));
    int oi = geti("--out", find_default(io.playback_devices()));
    int mi = geti("--monitor", -1);
    if (!io.start(eng, ci, oi, mi, geti("--buffer", 40), err)) { std::printf("%s\n", err.c_str()); return 1; }
    std::printf("实时变声中 (算法延迟约 %.0f ms). 按 Ctrl+C 退出.\n", eng.latency_ms());
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::printf("\r输入 %6.1f dB  输出 %6.1f dB  F0 %6.1f Hz  推理 %6.1f ms  负载 %5.1f%%  欠载 %d   ",
                    eng.stats.in_db.load(), eng.stats.out_db.load(), eng.stats.f0_hz.load(), eng.stats.infer_ms.load(),
                    io.load.load() * 100, io.underruns.load());
        std::fflush(stdout);
    }
}
