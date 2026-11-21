// 实时变声器 图形界面 (Dear ImGui + DirectX 11)
#include <d3d11.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "audio_io.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "settings.h"
#include "vc_engine.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT g_resize_w = 0, g_resize_h = 0;
static bool g_hotkey_toggle = false;  // 全局快捷键 Ctrl+Alt+V: 变声/原声 切换

static void create_rtv() {
    ID3D11Texture2D* bb = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
}
static void cleanup_rtv() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }

static bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL lv[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, lv, 2, D3D11_SDK_VERSION, &sd,
                                               &g_swap, &g_dev, &fl, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)  // 无独显/远程桌面: 软件渲染
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, lv, 2, D3D11_SDK_VERSION, &sd, &g_swap,
                                           &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) return false;
    create_rtv();
    return true;
}

static LRESULT WINAPI wnd_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, msg, w, l)) return true;
    switch (msg) {
        case WM_SIZE:
            if (w != SIZE_MINIMIZED) { g_resize_w = LOWORD(l); g_resize_h = HIWORD(l); }
            return 0;
        case WM_SYSCOMMAND:
            if ((w & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_HOTKEY:
            if (w == 1) g_hotkey_toggle = true;
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

static fs::path exe_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

struct App {
    VoiceEngine eng;
    AudioIO io;
    Settings st;
    std::vector<fs::path> voices;
    std::vector<std::string> voice_names;
    int voice_sel = -1, loaded_voice = -1;
    int cap_sel = -1, out_sel = -1, mon_sel = -1;
    bool common_ok = false, monitor = true;
    std::string status = "就绪";
    EngineConfig cfg;
    int out_buffer_ms = 40;
    float pitch = 0, index_rate = 0.5f, rms_mix = 0.25f, gate = -50, in_gain_db = 0, out_gain_db = 0, protect = 0.33f;
    bool bypass = false;

    void scan_voices() {
        voices.clear();
        voice_names.clear();
        fs::path root = exe_dir() / "voices";
        std::error_code ec;
        if (fs::exists(root, ec))
            for (auto& e : fs::directory_iterator(root, ec))
                if (e.is_directory() && fs::exists(e.path() / "voice.json") && fs::exists(e.path() / "enc.onnx")) {
                    voices.push_back(e.path());
                    voice_names.push_back(e.path().filename().u8string());
                }
    }
    static int find_name(const std::vector<AudioDeviceDesc>& v, const std::string& name, bool fallback_default) {
        for (size_t i = 0; i < v.size(); ++i) if (v[i].name == name) return (int)i;
        if (fallback_default) for (size_t i = 0; i < v.size(); ++i) if (v[i].is_default) return (int)i;
        return -1;
    }
    void init() {
        st.load(exe_dir() / "config.ini");
        cfg.block_ms = st.geti("block_ms", 250);
        cfg.extra_ms = st.geti("extra_ms", 800);
        cfg.crossfade_ms = st.geti("crossfade_ms", 50);
        cfg.lookahead_ms = st.geti("lookahead_ms", 80);
        cfg.threads = st.geti("threads", 0);
        out_buffer_ms = st.geti("out_buffer_ms", 40);
        pitch = st.getf("pitch", 0);
        index_rate = st.getf("index_rate", 0.5f);
        rms_mix = st.getf("rms_mix", 0.25f);
        gate = st.getf("gate_db", -50);
        protect = st.getf("protect", 0.33f);
        in_gain_db = st.getf("in_gain_db", 0);
        out_gain_db = st.getf("out_gain_db", 0);
        monitor = st.geti("monitor_on", 1) != 0;
        std::string err;
        if (!io.init(err)) status = err;
        cap_sel = find_name(io.capture_devices(), st.get("in_device"), true);
        // 输出设备默认优先选择虚拟声卡 (VB-CABLE / VoiceMeeter)
        out_sel = find_name(io.playback_devices(), st.get("out_device"), false);
        if (out_sel < 0)
            for (size_t i = 0; i < io.playback_devices().size(); ++i) {
                auto& n = io.playback_devices()[i].name;
                if (n.find("CABLE") != std::string::npos || n.find("VoiceMeeter") != std::string::npos) { out_sel = (int)i; break; }
            }
        if (out_sel < 0) out_sel = find_name(io.playback_devices(), "", true);
        mon_sel = find_name(io.playback_devices(), st.get("monitor_device"), false);
        eng.configure(cfg);
        common_ok = eng.load_common(exe_dir() / "models" / "common", cfg.threads, err);
        if (!common_ok) status = err + "  (请把 hubert.onnx / rmvpe.onnx 放到 models/common)";
        scan_voices();
        auto last = st.get("voice");
        for (size_t i = 0; i < voice_names.size(); ++i) if (voice_names[i] == last) voice_sel = (int)i;
        if (voice_sel < 0 && !voices.empty()) voice_sel = 0;
        if (voice_sel >= 0 && common_ok) load_voice();
    }
    void load_voice() {
        std::string err;
        if (eng.load_voice(voices[voice_sel], err)) {
            loaded_voice = voice_sel;
            status = "已加载声音: " + voice_names[voice_sel];
        } else status = err;
    }
    void push_params() {
        eng.params.pitch_shift = pitch;
        eng.params.index_rate = index_rate;
        eng.params.rms_mix = rms_mix;
        eng.params.gate_db = gate;
        eng.params.protect = protect;
        eng.params.in_gain = std::pow(10.f, in_gain_db / 20.f);
        eng.params.out_gain = std::pow(10.f, out_gain_db / 20.f);
        eng.params.bypass = bypass;
        io.monitor_enabled = monitor;
    }
    void start() {
        if (!eng.ready()) { status = "请先加载声音模型"; return; }
        eng.configure(cfg);
        std::string err;
        push_params();
        if (io.start(eng, cap_sel, out_sel, mon_sel, out_buffer_ms, err)) status = "变声中...";
        else status = err;
    }
    void save() {
        st.set("block_ms", cfg.block_ms);
        st.set("extra_ms", cfg.extra_ms);
        st.set("crossfade_ms", cfg.crossfade_ms);
        st.set("lookahead_ms", cfg.lookahead_ms);
        st.set("threads", cfg.threads);
        st.set("out_buffer_ms", out_buffer_ms);
        st.set("pitch", pitch);
        st.set("index_rate", index_rate);
        st.set("rms_mix", rms_mix);
        st.set("gate_db", gate);
        st.set("protect", protect);
        st.set("in_gain_db", in_gain_db);
        st.set("out_gain_db", out_gain_db);
        st.set("monitor_on", monitor ? 1 : 0);
        if (voice_sel >= 0) st.set("voice", voice_names[voice_sel]);
        auto dn = [](const std::vector<AudioDeviceDesc>& v, int i) { return (i >= 0 && i < (int)v.size()) ? v[i].name : std::string(); };
        st.set("in_device", dn(io.capture_devices(), cap_sel));
        st.set("out_device", dn(io.playback_devices(), out_sel));
        st.set("monitor_device", dn(io.playback_devices(), mon_sel));
        st.save();
    }

    static bool combo(const char* label, int* sel, const std::vector<std::string>& items, bool allow_none = false) {
        const char* preview = (*sel >= 0 && *sel < (int)items.size()) ? items[*sel].c_str() : (allow_none ? "(不使用)" : "(未选择)");
        bool changed = false;
        if (ImGui::BeginCombo(label, preview)) {
            if (allow_none && ImGui::Selectable("(不使用)", *sel < 0)) { *sel = -1; changed = true; }
            for (int i = 0; i < (int)items.size(); ++i)
                if (ImGui::Selectable((items[i] + "##" + std::to_string(i)).c_str(), *sel == i)) { *sel = i; changed = true; }
            ImGui::EndCombo();
        }
        return changed;
    }
    static void meter(const char* label, float db) {
        float v = std::clamp((db + 60.f) / 60.f, 0.f, 1.f);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s %.0f dB", label, db);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, db > -3 ? ImVec4(0.9f, 0.2f, 0.2f, 1) : ImVec4(0.2f, 0.75f, 0.35f, 1));
        ImGui::ProgressBar(v, ImVec2(-1, 0), buf);
        ImGui::PopStyleColor();
    }

    void draw() {
        const bool run = io.running();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::SeparatorText("声音模型");
        ImGui::BeginDisabled(run);
        ImGui::SetNextItemWidth(-90);
        if (combo("##voice", &voice_sel, voice_names) && voice_sel >= 0 && common_ok) load_voice();
        ImGui::SameLine();
        if (ImGui::Button("刷新列表", ImVec2(-1, 0))) scan_voices();
        ImGui::EndDisabled();
        if (loaded_voice >= 0)
            ImGui::TextDisabled("采样率 %d Hz", eng.voice().sample_rate);

        ImGui::SeparatorText("音频设备");
        std::vector<std::string> caps, plays;
        for (auto& d : io.capture_devices()) caps.push_back(d.name);
        for (auto& d : io.playback_devices()) plays.push_back(d.name);
        ImGui::BeginDisabled(run);
        combo("麦克风", &cap_sel, caps);
        combo("输出 (直播软件)", &out_sel, plays);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("推荐安装 VB-CABLE 虚拟声卡: 这里选 \"CABLE Input\",\n直播软件(OBS/直播伴侣)的麦克风选 \"CABLE Output\"。");
        combo("监听耳机", &mon_sel, plays, true);
        if (ImGui::Button("刷新设备")) io.refresh();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("监听自己", &monitor);

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, run ? ImVec4(0.7f, 0.2f, 0.2f, 1) : ImVec4(0.2f, 0.55f, 0.3f, 1));
        if (ImGui::Button(run ? "停止变声" : "开始变声", ImVec2(-1, 42))) {
            if (run) { io.stop(); status = "已停止"; }
            else start();
        }
        ImGui::PopStyleColor();

        ImGui::SeparatorText("声音调节 (实时生效)");
        ImGui::SliderFloat("变调 (半音)", &pitch, -24, 24, "%.0f");
        if (ImGui::Button("男变女 +12")) pitch = 12;
        ImGui::SameLine();
        if (ImGui::Button("男变女 +8")) pitch = 8;
        ImGui::SameLine();
        if (ImGui::Button("原调 0")) pitch = 0;
        ImGui::SameLine();
        if (ImGui::Button("女变男 -8")) pitch = -8;
        ImGui::SameLine();
        if (ImGui::Button("女变男 -12")) pitch = -12;
        ImGui::SliderFloat("音色相似度", &index_rate, 0, 1, "%.2f");
        ImGui::SliderFloat("音量跟随原声", &rms_mix, 0, 1, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = 输出音量完全跟随你的说话轻重 (情绪起伏最真实)\n1 = 使用模型自身音量");
        ImGui::SliderFloat("清辅音保护", &protect, 0, 0.5f, "%.2f");
        ImGui::SliderFloat("噪声门限 (dB)", &gate, -100, -20, "%.0f");
        ImGui::SliderFloat("输入增益 (dB)", &in_gain_db, -20, 20, "%.1f");
        ImGui::SliderFloat("输出增益 (dB)", &out_gain_db, -20, 20, "%.1f");
        if (g_hotkey_toggle) { bypass = !bypass; g_hotkey_toggle = false; }
        ImGui::Checkbox("原声直通 (临时关闭变声)  快捷键 Ctrl+Alt+V", &bypass);
        push_params();

        ImGui::SeparatorText("性能 (停止后修改)");
        ImGui::BeginDisabled(run);
        ImGui::SliderInt("块长 ms", &cfg.block_ms, 100, 1000);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("越小延迟越低, 但 CPU 压力越大。负载超过 80%% 请调大。");
        ImGui::SliderInt("上下文 ms", &cfg.extra_ms, 200, 3000);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("越大音质越稳定, 越耗 CPU");
        ImGui::SliderInt("交叉淡化 ms", &cfg.crossfade_ms, 20, 150);
        ImGui::SliderInt("前瞻 ms", &cfg.lookahead_ms, 0, 200);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("等待少量后续语音再输出最新部分, 吐字更清晰 (增加等量延迟)");
        ImGui::SliderInt("输出缓冲 ms", &out_buffer_ms, 10, 300);
        ImGui::SliderInt("线程数 (0自动)", &cfg.threads, 0, 32);
        ImGui::EndDisabled();

        ImGui::SeparatorText("状态");
        meter("输入", eng.stats.in_db.load());
        meter("输出", eng.stats.out_db.load());
        float blk = (float)cfg.block_ms;
        ImGui::Text("推理 %.0f ms / 块 %.0f ms   负载 %.0f%%   基频 %.0f Hz", eng.stats.infer_ms.load(), blk,
                    io.load.load() * 100, eng.stats.f0_hz.load());
        ImGui::Text("估计总延迟 约 %.0f ms   欠载次数 %d", eng.latency_ms() + eng.stats.infer_ms.load() + out_buffer_ms + 20,
                    io.underruns.load());
        if (run && io.load.load() > 0.9f) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "CPU 跟不上! 请增大块长或减小上下文");
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::End();
    }
};

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    ImGui_ImplWin32_EnableDpiAwareness();
    float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, wnd_proc, 0, 0, inst, nullptr, nullptr, nullptr, nullptr, L"VoiceChangerWnd", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"实时变声器 (CPU)", WS_OVERLAPPEDWINDOW, 100, 60, (int)(560 * scale),
                              (int)(1000 * scale), nullptr, nullptr, inst, nullptr);
    if (!create_device(hwnd)) { MessageBoxW(hwnd, L"无法初始化 Direct3D 11", L"错误", MB_OK); return 1; }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& gio = ImGui::GetIO();
    gio.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FrameRounding = 4;
    const char* fonts[] = {"C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\simhei.ttf", "C:\\Windows\\Fonts\\simsun.ttc"};
    for (auto f : fonts)
        if (GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES &&
            gio.Fonts->AddFontFromFileTTF(f, 18.f * scale, nullptr, gio.Fonts->GetGlyphRangesChineseFull()))
            break;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'V');
    App app;
    app.init();

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (IsIconic(hwnd)) {  // 最小化时不渲染, 把 CPU 留给变声
            if (g_hotkey_toggle) { app.bypass = !app.bypass; app.eng.params.bypass = app.bypass; g_hotkey_toggle = false; }
            Sleep(50);
            continue;
        }
        if (g_resize_w) {
            cleanup_rtv();
            g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            create_rtv();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        app.draw();
        ImGui::Render();
        const float clear[4] = {0.1f, 0.1f, 0.12f, 1};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }
    app.io.stop();
    app.save();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_rtv();
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    return 0;
}
