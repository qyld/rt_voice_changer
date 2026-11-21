// AI 变声器 主程序: Win32 + WebView2 界面 (前端在 ui/index.html, 编进 exe 资源)
//   JS -> C++ : window.chrome.webview.postMessage(JSON 字符串)  {"cmd": ..., ...}
//   C++ -> JS : PostWebMessageAsJson                            {"type": "state"|"status"|"toast"|..., ...}
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl.h>

#include <WebView2.h>
#include <dbghelp.h>
#include <exception>
#pragma comment(lib, "dbghelp.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "audio_io.h"
#include "gpu_enum.h"
#include "mini_json.h"
#include "resource.h"
#include "settings.h"
#include "vc_engine.h"
#include "vc_log.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

static bool g_force_cpu = false;

#define WM_APP_TASK_DONE (WM_APP + 1)

static std::wstring w(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring r(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), r.data(), n);
    return r;
}
static std::string u8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string r(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), r.data(), n, nullptr, nullptr);
    return r;
}
static fs::path exe_dir() {
    wchar_t b[MAX_PATH];
    GetModuleFileNameW(nullptr, b, MAX_PATH);
    return fs::path(b).parent_path();
}
static fs::path user_dir() {  // %APPDATA%\AIVoiceChanger (安装到 Program Files 时 exe 目录不可写)
    PWSTR p = nullptr;
    fs::path r = exe_dir();
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) r = fs::path(p) / L"AIVoiceChanger";
    CoTaskMemFree(p);
    std::error_code ec;
    fs::create_directories(r, ec);
    return r;
}
static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static std::string json_field(const std::string& js, const std::string& key) {
    auto k = js.find("\"" + key + "\"");
    if (k == std::string::npos) return "";
    auto q1 = js.find('"', js.find(':', k) + 1), q2 = js.find('"', q1 + 1);
    return (q1 == std::string::npos || q2 == std::string::npos) ? "" : js.substr(q1 + 1, q2 - q1 - 1);
}

struct VoiceEntry {
    std::string id, name, desc;
    fs::path dir;
    int sr = 40000;
    bool lite = false, preview = false;
};

class App {
public:
    HWND hwnd = nullptr;
    ComPtr<ICoreWebView2Controller> ctrl;
    ComPtr<ICoreWebView2> web;

    void init() {
        st.load(user_dir() / "config.ini");
        std::string err;
        if (!io.init(err)) { vc_log("音频初始化失败: " + err); toast(err, true); }
        gpus = enum_gpus();
        for (auto& g : gpus) vc_log("显卡 #" + std::to_string(g.index) + ": " + g.name + " (" + g.vendor + ", " + std::to_string(g.vram_mb) + " MB)");
        scan_voices();
        load_settings();
        if (g_force_cpu && compute >= 0) {  // 命令行 --cpu (开始菜单 "CPU 模式")
            vc_log("命令行 --cpu: 强制使用 CPU");
            compute = -1;
            cfg = cpu_profile();
            save();
        }
        // 防崩溃循环: 上次在显卡初始化/预热过程中程序异常退出 -> 这次改用 CPU
        if (st.geti("gpu_trial", 0) && compute >= 0) {
            vc_log("检测到上次使用显卡 #" + std::to_string(compute) + " 时程序异常退出, 本次改用 CPU");
            pending_toast = "上次使用显卡时程序异常退出，已自动改用 CPU。可在 高级模式 → 设备与算力 中重新选择显卡。";
            compute = -1;
            cfg = cpu_profile();
            st.set("gpu_trial", 0);
            save();
        }
    }

    // ------------------------------------------------------------ 消息处理
    void on_message(const std::string& msg) {
        JObj m = json_parse_flat(msg);
        const std::string cmd = m["cmd"].str;
        auto num = [&](const char* k) { return (float)m[k].num; };
        if (cmd == "ready") {
            send_state();
            if (!pending_toast.empty()) { toast(pending_toast, true); pending_toast.clear(); }
            if (!common_loaded && !busy) reload_models();
        } else if (cmd == "selectVoice") {
            voice_id = m["id"].str;
            reload_models(/*voice_only=*/true);
        } else if (cmd == "toggle") {
            if (io.running()) stop(); else start();
        } else if (cmd == "preset") {
            const std::string p = m["value"].str;
            pitch = p == "m2f" ? 8.f : p == "f2m" ? -8.f : 0.f;
            push_params(); save(); send_state();
        } else if (cmd == "param") {
            const std::string k = m["key"].str;
            float v = num("value");
            if (k == "pitch") pitch = v;
            else if (k == "index_rate") index_rate = v;
            else if (k == "rms_mix") rms_mix = v;
            else if (k == "gate_db") gate_db = v;
            else if (k == "protect") protect = v;
            else if (k == "in_gain_db") in_gain_db = v;
            else if (k == "out_gain_db") out_gain_db = v;
            else if (k == "monitor") monitor = v != 0;
            else if (k == "bypass") bypass = v != 0;
            push_params(); save();
        } else if (cmd == "device") {
            const std::string kind = m["kind"].str, name = m["name"].str;
            bool was = io.running();
            if (was) io.stop();
            if (kind == "mic") mic_name = name;
            else if (kind == "out") out_name = name;
            else if (kind == "monitor") mon_name = name;
            save();
            if (was) start();
            send_state();
        } else if (cmd == "compute") {
            int d = (int)num("device");
            if (d != compute) {
                compute = d;
                cfg = d >= 0 ? gpu_profile() : cpu_profile();
                save();
                reload_models();
            }
        } else if (cmd == "config") {
            bool was = io.running();
            if (was) io.stop();
            cfg.block_ms = (int)num("block_ms");
            cfg.extra_ms = (int)num("extra_ms");
            cfg.crossfade_ms = (int)num("crossfade_ms");
            cfg.lookahead_ms = (int)num("lookahead_ms");
            out_buffer_ms = (int)num("out_buffer_ms");
            eng.configure(cfg);
            save();
            if (was) start();
            send_state();
        } else if (cmd == "resetConfig") {
            cfg = compute >= 0 ? gpu_profile() : cpu_profile();
            out_buffer_ms = 40;
            eng.configure(cfg);
            save();
            send_state();
        } else if (cmd == "mode") {
            ui_mode = m["value"].str;
            save();
        } else if (cmd == "autoConfig") {
            auto_config();
        } else if (cmd == "wizardDone") {
            first_run = false;
            save();
        } else if (cmd == "refreshDevices") {
            io.refresh();
            send_state();
        } else if (cmd == "openVoices") {
            ShellExecuteW(nullptr, L"open", voices_root().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (cmd == "rescan") {
            scan_voices();
            send_state();
        }
    }

    // 后台任务完成 (UI 线程)
    void on_task_done() {
        std::function<void()> f;
        {
            std::lock_guard<std::mutex> lk(task_mu);
            f.swap(task_done);
        }
        if (f) f();
    }

    // 定时推送状态 + CPU 过载自动降级
    void on_timer() {
        if (!web) return;
        const bool run = io.running();
        if (run && io.load.load() > 0.95f) {
            if (++overload_ticks >= 30 && cfg.block_ms < 600) {  // 连续 3 秒跟不上
                overload_ticks = 0;
                io.stop();
                cfg.block_ms += 100;
                cfg.extra_ms = std::min(cfg.extra_ms, 600);
                save();
                start();
                toast("电脑性能不足，已自动把块长调整为 " + std::to_string(cfg.block_ms) + " ms（延迟略增，避免断音）", false);
                send_state();
            }
        } else overload_ticks = 0;
        if (eng.errors.load() > 0 && !error_toasted) {
            error_toasted = true;
            toast("推理过程中出现错误（已自动静音该段，不影响继续使用）。如频繁出现请把日志发给我们：" + u8((user_dir() / L"log.txt").wstring()), true);
        }
        std::ostringstream o;
        o << "{\"type\":\"status\",\"running\":" << (run ? "true" : "false") << ",\"busy\":" << (busy ? "true" : "false")
          << ",\"inDb\":" << eng.stats.in_db.load() << ",\"outDb\":" << eng.stats.out_db.load()
          << ",\"f0\":" << eng.stats.f0_hz.load() << ",\"inferMs\":" << eng.stats.infer_ms.load()
          << ",\"load\":" << io.load.load() << ",\"underruns\":" << io.underruns.load()
          << ",\"latencyMs\":" << (eng.latency_ms() + eng.stats.infer_ms.load() + out_buffer_ms + 20)
          << ",\"obs\":" << (run && io.obs_connected() ? "true" : "false") << "}";
        post(o.str());
    }

    void shutdown() {
        io.stop();
        if (worker.joinable()) worker.join();
        save();
    }

private:
    VoiceEngine eng;
    AudioIO io;
    Settings st;
    std::vector<GpuInfo> gpus;
    std::vector<VoiceEntry> voices;
    std::string voice_id, mic_name, out_name = "auto", mon_name, ui_mode = "simple", pending_toast;
    bool error_toasted = false;
    int compute = -1, out_buffer_ms = 40, overload_ticks = 0;
    bool first_run = true, monitor = false, bypass = false;
    std::atomic<bool> common_loaded{false};
    std::atomic<bool> busy{false};
    EngineConfig cfg;
    float pitch = 0, index_rate = 0.5f, rms_mix = 0.25f, gate_db = -60, protect = 0.33f, in_gain_db = 0, out_gain_db = 0;
    std::thread worker;
    std::mutex task_mu;
    std::function<void()> task_done;

    fs::path voices_root() const {
        fs::path a = exe_dir() / "voices";
        return fs::exists(a) ? a : user_dir() / "voices";
    }

    void post(const std::string& json) {
        if (web) web->PostWebMessageAsJson(w(json).c_str());
    }
    void toast(const std::string& text, bool error) {
        post("{\"type\":\"toast\",\"error\":" + std::string(error ? "true" : "false") + ",\"text\":" + json_escape(text) + "}");
    }

    void scan_voices() {
        voices.clear();
        std::error_code ec;
        for (const fs::path& root : {exe_dir() / "voices", user_dir() / "voices"}) {
            if (!fs::exists(root, ec)) continue;
            for (auto& e : fs::directory_iterator(root, ec)) {
                if (!e.is_directory() || !fs::exists(e.path() / "voice.json") || !fs::exists(e.path() / "enc.onnx")) continue;
                VoiceEntry v;
                v.dir = e.path();
                v.id = e.path().filename().u8string();
                std::string js = read_file(e.path() / "voice.json");
                v.name = json_field(js, "display_name");
                if (v.name.empty()) v.name = v.id;
                v.desc = json_field(js, "description");
                v.lite = js.find("\"lite\": true") != std::string::npos || fs::file_size(e.path() / "dec.onnx", ec) < 30u * 1024 * 1024;
                v.preview = fs::exists(e.path() / "preview.wav");
                if (std::none_of(voices.begin(), voices.end(), [&](auto& x) { return x.id == v.id; })) voices.push_back(v);
            }
        }
        if (ctrl) {  // 试听文件通过虚拟主机访问
            ComPtr<ICoreWebView2_3> w3;
            if (web && SUCCEEDED(web.As(&w3)))
                w3->SetVirtualHostNameToFolderMapping(L"voices.aivc", voices_root().c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        }
    }

    void load_settings() {
        first_run = st.geti("configured", 0) == 0;
        compute = st.geti("compute", -2);
        if (compute == -2) compute = gpus.empty() ? -1 : gpus[0].index;  // 首次: 有显卡默认用显卡
        cfg = compute >= 0 ? gpu_profile() : cpu_profile();
        cfg.block_ms = st.geti("block_ms", cfg.block_ms);
        cfg.extra_ms = st.geti("extra_ms", cfg.extra_ms);
        cfg.crossfade_ms = st.geti("crossfade_ms", cfg.crossfade_ms);
        cfg.lookahead_ms = st.geti("lookahead_ms", cfg.lookahead_ms);
        out_buffer_ms = st.geti("out_buffer_ms", 40);
        pitch = st.getf("pitch", 0);
        index_rate = st.getf("index_rate", 0.5f);
        rms_mix = st.getf("rms_mix", 0.25f);
        gate_db = st.getf("gate_db", -60);
        protect = st.getf("protect", 0.33f);
        in_gain_db = st.getf("in_gain_db", 0);
        out_gain_db = st.getf("out_gain_db", 0);
        monitor = st.geti("monitor_on", 0) != 0;
        mic_name = st.get("in_device");
        out_name = st.get("out_device", "auto");
        mon_name = st.get("monitor_device");
        ui_mode = st.get("ui_mode", "simple");
        voice_id = st.get("voice");
        if (std::none_of(voices.begin(), voices.end(), [&](auto& v) { return v.id == voice_id; }))
            voice_id = voices.empty() ? "" : voices[0].id;
    }

    void save() {
        st.set("configured", first_run ? 0 : 1);
        st.set("compute", compute);
        st.set("block_ms", cfg.block_ms);
        st.set("extra_ms", cfg.extra_ms);
        st.set("crossfade_ms", cfg.crossfade_ms);
        st.set("lookahead_ms", cfg.lookahead_ms);
        st.set("out_buffer_ms", out_buffer_ms);
        st.set("pitch", pitch);
        st.set("index_rate", index_rate);
        st.set("rms_mix", rms_mix);
        st.set("gate_db", gate_db);
        st.set("protect", protect);
        st.set("in_gain_db", in_gain_db);
        st.set("out_gain_db", out_gain_db);
        st.set("monitor_on", monitor ? 1 : 0);
        st.set("in_device", mic_name);
        st.set("out_device", out_name);
        st.set("monitor_device", mon_name);
        st.set("ui_mode", ui_mode);
        st.set("voice", voice_id);
        st.save();
    }

    void push_params() {
        eng.params.pitch_shift = pitch;
        eng.params.index_rate = index_rate;
        eng.params.rms_mix = rms_mix;
        eng.params.gate_db = gate_db;
        eng.params.protect = protect;
        eng.params.in_gain = std::pow(10.f, in_gain_db / 20.f);
        eng.params.out_gain = std::pow(10.f, out_gain_db / 20.f);
        eng.params.bypass = bypass;
        io.monitor_enabled = monitor;
    }

    // 在后台线程执行 job, 完成后在 UI 线程执行 done
    void run_task(std::function<void()> job, std::function<void()> done) {
        if (busy) return;
        if (worker.joinable()) worker.join();
        busy = true;
        post("{\"type\":\"busy\",\"value\":true}");
        worker = std::thread([this, job, done] {
            try {
                job();
            } catch (const std::exception& e) {
                vc_log(std::string("后台任务异常: ") + e.what());
            } catch (...) {
                vc_log("后台任务未知异常");
            }
            {
                std::lock_guard<std::mutex> lk(task_mu);
                task_done = [this, done] { busy = false; post("{\"type\":\"busy\",\"value\":false}"); done(); };
            }
            PostMessageW(hwnd, WM_APP_TASK_DONE, 0, 0);
        });
    }

    void reload_models(bool voice_only = false) {
        bool was = io.running();
        if (was) io.stop();
        auto it = std::find_if(voices.begin(), voices.end(), [&](auto& v) { return v.id == voice_id; });
        fs::path vdir = it != voices.end() ? it->dir : fs::path();
        auto err = std::make_shared<std::string>();
        bool reload_common = !voice_only || !common_loaded || compute != eng.device();
        if (compute >= 0) { st.set("gpu_trial", 1); st.save(); }  // 若在显卡初始化中崩溃, 下次启动会改用 CPU
        auto fell = std::make_shared<bool>(false);
        auto c = compute;
        vc_log("加载模型: 设备=" + std::string(c >= 0 ? "显卡 #" + std::to_string(c) : "CPU") + ", 声音=" + vdir.filename().u8string());
        run_task(
            [this, vdir, err, reload_common, fell, c] {
                auto load = [&](int dev) -> bool {
                    if (reload_common || dev != eng.device()) {
                        common_loaded = eng.load_common(exe_dir() / "models" / "common", 0, *err, dev);
                        if (!common_loaded) return false;
                    }
                    if (!vdir.empty() && !eng.load_voice(vdir, *err)) return false;
                    eng.configure(cfg);
                    std::string we;
                    if (!eng.warmup(&we)) { *err = "预热失败: " + we; return false; }
                    return true;
                };
                if (!load(c) && c >= 0) {  // 显卡整体失败 -> CPU
                    vc_log("显卡加载/预热失败, 改用 CPU: " + *err);
                    *fell = true;
                    err->clear();
                    cfg = cpu_profile();
                    load(-1);
                }
            },
            [this, err, was, fell] {
                st.set("gpu_trial", 0);
                if (*fell) {
                    compute = -1;
                    toast("显卡推理不可用，已自动改用 CPU（详情见日志 log.txt）", true);
                } else if (compute >= 0 && eng.gpu_model_count() == 0) {  // 全部模型都退回了 CPU
                    vc_log("所有模型在显卡上执行失败, 改用 CPU 档参数");
                    compute = -1;
                    cfg = cpu_profile();
                    eng.configure(cfg);
                    toast("这块显卡暂不支持本软件的推理，已自动改用 CPU（详情见日志 log.txt）", true);
                } else if (eng.gpu_fell_back()) {
                    toast("部分模型在显卡上运行失败，已自动改用 CPU 运行这部分（不影响使用）", false);
                }
                if (!err->empty()) toast(*err, true);
                if (!eng.device_note().empty()) toast(eng.device_note(), true);
                save();
                send_state();
                if (was && eng.ready()) start();
            });
    }

    int resolve_out() const {
        if (out_name == "none") return -1;
        if (out_name == "auto" || out_name.empty()) return io.find_virtual_mic_output();  // 装了驱动则输出到驱动, 否则只给 OBS 插件
        return io.find_device(false, out_name);
    }

    void start() {
        if (!eng.ready()) { toast("声音模型还没加载好", true); return; }
        eng.configure(cfg);
        push_params();
        int ci = mic_name.empty() ? -1 : io.find_device(true, mic_name);
        int mi = mon_name.empty() ? -1 : io.find_device(false, mon_name);
        std::string err;
        if (!io.start(eng, ci, resolve_out(), mi, out_buffer_ms, err)) toast(err, true);
        send_state();
    }
    void stop() {
        io.stop();
        send_state();
    }

    // 性能自动测试: 每块显卡 + CPU 各跑约 3 秒, 选出能实时运行的最佳设备与参数
    void auto_config() {
        auto it = std::find_if(voices.begin(), voices.end(), [&](auto& v) { return v.id == voice_id; });
        if (it == voices.end()) { toast("请先放入声音模型", true); return; }
        bool was = io.running();
        if (was) io.stop();
        fs::path vdir = it->dir;
        auto result = std::make_shared<std::vector<std::pair<int, float>>>();
        auto chosen = std::make_shared<int>(-1);
        auto chosen_cfg = std::make_shared<EngineConfig>(cpu_profile());
        auto gl = gpus;
        run_task(
            [this, vdir, result, chosen, chosen_cfg, gl] {
                const int rate = 48000;
                std::vector<float> sig(rate * 4);
                double ph = 0;
                for (size_t i = 0; i < sig.size(); ++i) {
                    double f = 160 + 30 * std::sin(2 * M_PI * 3 * double(i) / rate);
                    ph += f / rate;
                    ph -= std::floor(ph);
                    sig[i] = float(0.08 * (2 * ph - 1));
                }
                auto bench = [&](int dev, EngineConfig c) -> float {
                  try {
                    VoiceEngine e;
                    std::string er;
                    e.configure(c);
                    if (!e.load_common(exe_dir() / "models" / "common", 0, er, dev) || !e.load_voice(vdir, er)) return 99.f;
                    if (dev >= 0 && e.device() < 0) return 99.f;  // 显卡不可用
                    e.configure(c);
                    if (!e.warmup() && dev >= 0) return 99.f;
                    const int B = e.block_io();
                    std::vector<float> o(B);
                    double tot = 0;
                    int n = 0;
                    for (size_t p = 0; p + B <= sig.size(); p += B, ++n) {
                        auto t = std::chrono::steady_clock::now();
                        e.process(&sig[p], o.data());
                        tot += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
                    }
                    if (dev >= 0 && e.gpu_fell_back()) vc_log("性能测试: 显卡 #" + std::to_string(dev) + " 部分模型改用 CPU");
                    return float(tot / std::max(1, n) / (1000.0 * B / rate));
                  } catch (const std::exception& ex) {
                    vc_log(std::string("性能测试异常: ") + ex.what());
                    return 99.f;
                  }
                };
                for (auto& g : gl) {
                    float r = bench(g.index, gpu_profile());
                    result->push_back({g.index, r});
                    if (r < 0.6f && *chosen < 0) { *chosen = g.index; *chosen_cfg = gpu_profile(); }
                }
                if (*chosen < 0) {
                    EngineConfig c = cpu_profile();
                    float r = bench(-1, c);
                    if (r > 0.75f) {  // 普通档跟不上: 降一档
                        c.block_ms = 400;
                        c.extra_ms = 500;
                        r = bench(-1, c);
                    }
                    result->push_back({-1, r});
                    *chosen_cfg = c;
                }
            },
            [this, result, chosen, chosen_cfg] {
                std::ostringstream o;
                o << "{\"type\":\"benchResult\",\"items\":[";
                bool first = true;
                float cpu_rtf = 0;
                for (auto& [d, r] : *result) {
                    std::string name = "CPU";
                    for (auto& g : gpus) if (g.index == d) name = g.name;
                    if (d < 0) cpu_rtf = r;
                    o << (first ? "" : ",") << "{\"name\":" << json_escape(name) << ",\"rtf\":" << r << "}";
                    first = false;
                }
                bool ok = *chosen >= 0 || cpu_rtf < 0.9f;
                o << "],\"ok\":" << (ok ? "true" : "false") << "}";
                post(o.str());
                compute = *chosen;
                cfg = *chosen_cfg;
                save();
                reload_models();
            });
    }

    void send_state() {
        std::ostringstream o;
        o << "{\"type\":\"state\",\"mode\":" << json_escape(ui_mode) << ",\"firstRun\":" << (first_run ? "true" : "false")
          << ",\"running\":" << (io.running() ? "true" : "false") << ",\"voice\":" << json_escape(voice_id)
          << ",\"compute\":" << compute << ",\"activeDevice\":" << (eng.gpu_model_count() > 0 ? eng.device() : -1) << ",\"gpuModels\":" << eng.gpu_model_count()
          << ",\"virtualMic\":" << (io.find_virtual_mic_output() >= 0 ? "true" : "false");
        o << ",\"voices\":[";
        for (size_t i = 0; i < voices.size(); ++i)
            o << (i ? "," : "") << "{\"id\":" << json_escape(voices[i].id) << ",\"name\":" << json_escape(voices[i].name)
              << ",\"desc\":" << json_escape(voices[i].desc) << ",\"lite\":" << (voices[i].lite ? "true" : "false")
              << ",\"preview\":" << (voices[i].preview ? "true" : "false") << "}";
        o << "],\"gpus\":[";
        for (size_t i = 0; i < gpus.size(); ++i)
            o << (i ? "," : "") << "{\"index\":" << gpus[i].index << ",\"name\":" << json_escape(gpus[i].name)
              << ",\"vendor\":" << json_escape(gpus[i].vendor) << ",\"vram\":" << gpus[i].vram_mb << "}";
        auto devs = [&](const std::vector<AudioDeviceDesc>& v) {
            std::string s = "[";
            for (size_t i = 0; i < v.size(); ++i)
                s += (i ? "," : "") + std::string("{\"name\":") + json_escape(v[i].name) + ",\"def\":" + (v[i].is_default ? "true" : "false") + "}";
            return s + "]";
        };
        o << "],\"mics\":" << devs(io.capture_devices()) << ",\"outs\":" << devs(io.playback_devices());
        o << ",\"mic\":" << json_escape(mic_name) << ",\"out\":" << json_escape(out_name) << ",\"monitorDev\":" << json_escape(mon_name);
        o << ",\"p\":{\"pitch\":" << pitch << ",\"index_rate\":" << index_rate << ",\"rms_mix\":" << rms_mix
          << ",\"gate_db\":" << gate_db << ",\"protect\":" << protect << ",\"in_gain_db\":" << in_gain_db
          << ",\"out_gain_db\":" << out_gain_db << ",\"monitor\":" << (monitor ? 1 : 0) << ",\"bypass\":" << (bypass ? 1 : 0) << "}";
        o << ",\"cfg\":{\"block_ms\":" << cfg.block_ms << ",\"extra_ms\":" << cfg.extra_ms << ",\"crossfade_ms\":" << cfg.crossfade_ms
          << ",\"lookahead_ms\":" << cfg.lookahead_ms << ",\"out_buffer_ms\":" << out_buffer_ms << "}}";
        post(o.str());
    }
};

static App g_app;

static std::wstring load_ui_html() {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_UI_HTML), RT_RCDATA);
    if (!r) return L"<h1>UI resource missing</h1>";
    HGLOBAL h = LoadResource(nullptr, r);
    const char* p = (const char*)LockResource(h);
    return w(std::string(p, SizeofResource(nullptr, r)));
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            if (g_app.ctrl) {
                RECT rc;
                GetClientRect(h, &rc);
                g_app.ctrl->put_Bounds(rc);
            }
            return 0;
        case WM_TIMER:
            g_app.on_timer();
            return 0;
        case WM_APP_TASK_DONE:
            g_app.on_task_done();
            return 0;
        case WM_HOTKEY:  // Ctrl+Alt+V 开关变声
            g_app.on_message("{\"cmd\":\"toggle\"}");
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize = {900, 620};
            return 0;
        }
        case WM_DESTROY:
            g_app.shutdown();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    // 崩溃时写日志 + 小型转储 (crash.dmp), 便于定位显卡驱动等问题
    char b[128];
    std::snprintf(b, sizeof(b), "程序崩溃: 异常代码 0x%08lX, 地址 %p", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    vc_log(b);
    HANDLE f = CreateFileW((user_dir() / L"crash.dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mi{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpNormal, &mi, nullptr, nullptr);
        CloseHandle(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdline, int show) {
    vc_log_set_file((user_dir() / L"log.txt").wstring());
    g_force_cpu = cmdline && wcsstr(cmdline, L"--cpu") != nullptr;
    vc_log(std::string("==== AI 变声器 v2.0.1 启动 ====") + (g_force_cpu ? " (CPU 模式)" : ""));
    SetUnhandledExceptionFilter(crash_filter);
    std::set_terminate([] { vc_log("程序异常终止 (未捕获的异常)"); std::abort(); });
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hbrBackground = CreateSolidBrush(RGB(15, 17, 23));
    wc.lpszClassName = L"AIVoiceChangerWnd";
    RegisterClassExW(&wc);
    const UINT dpi = GetDpiForSystem();
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"AI 变声器", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              MulDiv(1080, dpi, 96), MulDiv(720, dpi, 96), nullptr, nullptr, inst, nullptr);
    g_app.hwnd = hwnd;
    g_app.init();
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'V');

    const std::wstring data_dir = (user_dir() / L"webview").wstring();
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, data_dir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([hwnd](HRESULT r, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(r) || !env) {
                MessageBoxW(hwnd, L"无法启动界面组件 WebView2。请安装 Microsoft Edge WebView2 运行时后重试。", L"AI 变声器", MB_ICONERROR);
                return S_OK;
            }
            env->CreateCoreWebView2Controller(
                hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                          [hwnd](HRESULT r2, ICoreWebView2Controller* c) -> HRESULT {
                              if (FAILED(r2) || !c) return S_OK;
                              g_app.ctrl = c;
                              c->get_CoreWebView2(&g_app.web);
                              ComPtr<ICoreWebView2Controller2> c2;
                              if (SUCCEEDED(g_app.ctrl.As(&c2))) c2->put_DefaultBackgroundColor({255, 15, 17, 23});
                              ComPtr<ICoreWebView2Settings> s;
                              g_app.web->get_Settings(&s);
                              const bool dev = GetEnvironmentVariableW(L"VC_DEVTOOLS", nullptr, 0) > 0;
                              s->put_AreDevToolsEnabled(dev);
                              s->put_AreDefaultContextMenusEnabled(dev);
                              s->put_IsStatusBarEnabled(FALSE);
                              s->put_IsZoomControlEnabled(FALSE);
                              ComPtr<ICoreWebView2_3> w3;
                              if (SUCCEEDED(g_app.web.As(&w3)))
                                  w3->SetVirtualHostNameToFolderMapping(L"voices.aivc", (exe_dir() / L"voices").c_str(),
                                                                        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                              EventRegistrationToken tok;
                              g_app.web->add_WebMessageReceived(
                                  Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                      [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                                          LPWSTR m = nullptr;
                                          if (SUCCEEDED(a->TryGetWebMessageAsString(&m)) && m) {
                                              g_app.on_message(u8(m));
                                              CoTaskMemFree(m);
                                          }
                                          return S_OK;
                                      })
                                      .Get(),
                                  &tok);
                              RECT rc;
                              GetClientRect(hwnd, &rc);
                              c->put_Bounds(rc);
                              g_app.web->NavigateToString(load_ui_html().c_str());
                              SetTimer(hwnd, 1, 100, nullptr);
                              return S_OK;
                          })
                          .Get());
            return S_OK;
        }).Get());
    if (FAILED(hr))
        MessageBoxW(hwnd, L"无法启动界面组件 WebView2。请安装 Microsoft Edge WebView2 运行时后重试。", L"AI 变声器", MB_ICONERROR);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
