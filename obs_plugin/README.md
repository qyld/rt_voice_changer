# obs-ai-voice — OBS「AI 变声器」音频源插件

装上这个插件后，OBS 里可以直接添加「AI 变声器」音频源，不用再装 VB-CABLE 之类的虚拟声卡。
变声器主程序把输出写进共享内存 `Local\AIVoiceChanger_Out`，插件每 10ms 读一次并推给 OBS。

- 适用版本：OBS Studio 30.x（Windows 64 位，已在 30.2.3 上验证）
- 音频格式：48 kHz、单声道、float32（OBS 会自动混到你设置的输出格式）
- 协议头文件：[`../voice_changer/src/shm_output.h`](../voice_changer/src/shm_output.h)（header-only，主程序与插件共用）

## 安装（给主播）

1. 先关掉 OBS。
2. 把 `obs-ai-voice.dll` 拷到 OBS 安装目录下的 `obs-plugins\64bit\`：
   - 默认安装路径：`C:\Program Files\obs-studio\obs-plugins\64bit\`（需要管理员权限）
   - 便携版：`<便携版目录>\obs-plugins\64bit\`
3. 打开 OBS，在「来源」里点 **＋ → AI 变声器**，新建一个音频源。
4. 启动 AI 变声器主程序并开始变声，插件会自动连接（每 0.5 秒重试一次），不用在意两者谁先启动。
5. 在来源上右键 →「属性」可以看连接状态（点「刷新状态」更新）。没连上时这个源输出静音，不会产生噪声。

> 记得把麦克风原声（「麦克风/Aux」）静音或删掉，不然观众会同时听到原声和变声。

卸载：删除 `obs-plugins\64bit\obs-ai-voice.dll`。

## 从源码构建（开发者）

需要 VS 2026（MSVC v145）和 CMake 3.20 或更高版本。

```powershell
cd E:\tts\obs_plugin
powershell -ExecutionPolicy Bypass -File scripts\fetch_obs_deps.ps1   # 下载 libobs 头文件和 OBS 便携版，从 obs.dll 生成 obs.lib
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

产物都在 `build\Release\`：
| 文件 | 用途 |
|---|---|
| `obs-ai-voice.dll` | OBS 插件（静态 CRT，不依赖 VC 运行库） |
| `test_writer.exe` | 测试写端：持续写 440Hz 正弦，用法 `test_writer.exe [秒数] [频率] [幅度]` |
| `test_shm.exe` | 共享内存协议单元测试（也可以跑 `ctest -C Release --test-dir build`） |

`cmake --build build --config Release --target install_to_obs` 会把 dll 拷到 `deps\obs-portable\obs-plugins\64bit\`。要拷到别的 OBS，可以在配置时加 `-DOBS_PORTABLE_DIR=...`。

### 本地验证

```powershell
build\Release\test_writer.exe 30
deps\obs-portable\bin\64bit\obs64.exe --portable    # 需在 bin\64bit 目录下启动
```

日志在 `deps\obs-portable\config\obs-studio\logs\`（便携模式；安装版的日志在 `%APPDATA%\obs-studio\logs`），应该能看到下面这些行：
```
[obs-ai-voice] 插件已加载 v2.0.0，注册音频源 'ai_voice_changer_source'
[obs-ai-voice] 已连接变声器写端 (pid=…)
[obs-ai-voice] 10s 统计: 接收 480000 样本, RMS=0.2121, 峰值=0.3000, 累计丢弃 0
[obs-ai-voice] 变声器写端已断开，输出静音
```

## 共享内存协议（v1）

| 偏移 | 字段 | 说明 |
|---|---|---|
| 0 | `magic` u32 | `0x43564941`（"AIVC"），写端初始化完成后最后写入 |
| 4 | `version` u32 | 1 |
| 8 | `sample_rate` u32 | 48000 |
| 12 | `channels` u32 | 1 |
| 16 | `capacity` u32 | 96000 个样本，即 2 秒环形缓冲 |
| 20 | `header_size` u32 | 256，数据区从这里开始 |
| 24 | `writer_pid` u32 | 写端进程 ID |
| 32 | `session` u64 原子 | 写端每次 `create()` 都生成新值，读端据此发现写端重启 |
| 40 | `write_pos` u64 原子 | 累计写入的样本数（单调递增、不回绕），release 语义发布 |
| 48 | `heartbeat_ms` u64 原子 | `GetTickCount64()`；超过 1 秒没更新就算断开，写 0 表示主动下线 |
| 256 | `float data[96000]` | 样本 `i` 存放在 `data[i % capacity]` |

读端行为（`ShmReader::read_new`）：
- 首次读取或写端重启时，直接跳到最新位置，只保留最近 20ms，不回放旧数据。
- 落后超过 200ms 时，跳到 `write_pos - 20ms`，丢掉的样本数记在 `stats().frames_skipped`。
- 拷贝完后会再检查一次 `write_pos`，如果期间数据可能被覆盖（距写位置超过 1.5 秒），这批数据整个丢掉，防止读到撕裂数据。
- 心跳超时、写端已关闭、头部不合法时都返回 0。插件这时每 10ms 推送 480 个静音样本。

### 主引擎接入示例（audio_io.cpp worker 线程）

```cpp
#include "shm_output.h"
aivc::ShmWriter g_shm;            // 启动时调用 g_shm.create()；失败不影响原有声卡输出
// worker 每产出一块 48k 单声道变声结果：
g_shm.write(out48k, frames);      // 顺带刷新心跳
// 暂停或没有输出时，也要每 ≤200ms 调一次 g_shm.heartbeat()，否则 OBS 端 1 秒后显示"未连接"
// 退出时调用 g_shm.close()（或依赖析构），读端会立刻看到断开
```
单次 `write()` 最好不超过 0.5 秒的数据（正常每块 10–20ms）。同一时间只能有一个写端。
