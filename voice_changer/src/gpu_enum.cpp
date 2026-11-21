#include "gpu_enum.h"

#include <cstdlib>

#ifdef _WIN32
#include <dxgi1_6.h>
#include <windows.h>
#pragma comment(lib, "dxgi.lib")

static std::string to_u8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::vector<GpuInfo> enum_gpus(bool include_software) {
    std::vector<GpuInfo> out;
    if (std::getenv("VC_ALLOW_WARP")) include_software = true;
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) return out;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        a->Release();
        GpuInfo g;
        g.index = (int)i;
        g.name = to_u8(d.Description);
        g.vram_mb = d.DedicatedVideoMemory >> 20;
        // 软件渲染 / 微软基础显示适配器 (虚拟机、远程桌面) 不是真显卡
        g.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || d.VendorId == 0x1414;
        switch (d.VendorId) {
            case 0x10DE: g.vendor = "NVIDIA"; break;
            case 0x1002: case 0x1022: g.vendor = "AMD"; break;
            case 0x8086: g.vendor = "Intel"; break;
            default: g.vendor = "其他";
        }
        if (g.software && !include_software) continue;
        out.push_back(g);
    }
    f->Release();
    return out;
}
#else
std::vector<GpuInfo> enum_gpus(bool) { return {}; }
#endif
