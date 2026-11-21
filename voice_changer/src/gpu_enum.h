// 显卡枚举 (DXGI): 序号与 DirectML device_id 一致
#pragma once
#include <string>
#include <vector>

struct GpuInfo {
    int index = -1;            // DirectML device_id
    std::string name;          // UTF-8
    std::string vendor;        // NVIDIA / AMD / Intel / 其他
    size_t vram_mb = 0;
    bool software = false;     // Microsoft Basic Render Driver (WARP)
};

// include_software: 是否包含软件渲染适配器 (仅测试用, 设置环境变量 VC_ALLOW_WARP=1 也会包含)
std::vector<GpuInfo> enum_gpus(bool include_software = false);
