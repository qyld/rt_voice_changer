# AI变声器 虚拟声卡驱动（阶段 4b）

让变声器的输出直接变成一个“麦克风”，抖音直播伴侣、OBS、腾讯会议等任何软件都能选到，**不再需要另装 VB-CABLE**。

```
 变声器引擎 ──播放到──▶ [AI变声器 内部输出]  (渲染端点)
                              │  内核环形缓冲（48kHz/立体声/int32，1 秒容量）
                              ▼
 直播软件  ◀──录制自── [AI变声器 麦克风]     (采集端点)
```

- 设备：`AI变声器 虚拟声卡`，硬件 ID `ROOT\AIVoiceChanger`（类：声音、视频和游戏控制器 MEDIA）
- 渲染端点：**AI变声器 内部输出** —— 变声器（主会话的引擎）播放到这里
- 采集端点：**AI变声器 麦克风** —— 直播软件选它
- 格式：44100 / 48000 Hz，单 / 双声道，16 / 24 / 24-in-32 / 32 bit 整数 PCM（默认 48000Hz 立体声 16bit）；
  两端格式可以不同，驱动内部自动转换声道、位深并做 44.1k↔48k 线性插值重采样
- 无数据（变声器没在播放）时采集端输出静音；开始/中断时有约 1.3ms 淡入淡出，避免爆音
- 采集端支持多个程序同时打开（每个读者独立读指针）；渲染端同一时间 1 个写入者（共享模式下系统音频引擎只开 1 个流）
- 延迟：预缓冲 30ms（驱动内部，另加 Windows 音频引擎自身约 10~20ms）；积压超过 150ms 自动追赶
- 音量：两个端点都没有硬件音量节点，Windows 的音量滑块由系统音频引擎做软件音量，可正常调节/静音

## 目录

| 路径 | 说明 |
|---|---|
| `src\` | 驱动源码（基于 Windows-driver-samples `audio/sysvad` 的 TabletAudioSample 精简，MS-PL） |
| `src\aivcloopback.h/.cpp` | **新增**：渲染→采集环形缓冲、格式转换、重采样、预缓冲/淡入淡出 |
| `src\EndpointsCommon\aivcformats.h` | **新增**：两端共用的格式表 |
| `src\EndpointsCommon\speakerwavtable.h` | 渲染端 wave 表（改写：系统 pin + 桥接 pin，无 offload/硬件环回） |
| `src\AIVirtualAudio\micinwavtable.h`、`micintoptable.h` | 采集端 wave/拓扑表（改写：新格式表，去掉硬件音量节点） |
| `src\AIVirtualAudio\minipairs.h` | 只保留一对端点 |
| `src\AIVirtualAudio\AIVirtualAudio.inx` | INF 模板（UTF-16 LE，中文友好名），编译时 stampinf 生成 `AIVirtualAudio.inf` |
| `src\AIVirtualAudio\AIVirtualAudio.vcxproj` | 驱动工程（x64 Debug/Release） |
| `build.ps1` | 一键：NuGet 还原 WDK → 编译 → InfVerif → Inf2Cat → 测试证书签名 |
| `install_test.ps1` / `uninstall.ps1` | 测试签名模式下的安装 / 卸载脚本（用户电脑上运行） |
| `scripts\AivcSetupApi.ps1` | SetupAPI 封装：创建/删除 ROOT 设备节点（替代 devcon，用户电脑无需 WDK） |
| `tests\loopback\` | 环形缓冲的用户态单元测试（`run_test.ps1`） |
| `package\` | 构建输出的驱动包：`AIVirtualAudio.sys/.inf/.cat/.pdb` + `AIVoiceChangerTest.cer` |
| `SIGNING.md` | 正式发布：EV 证书 → 合作伙伴中心证明签名；HLK 可选说明 |
| `packages\`、`build\` | NuGet 包与中间文件（已 gitignore） |

## 构建

### 环境获取记录（本机：Windows Server 2025，VS 2026 Enterprise 18.x，未装 WDK MSI）

1. sysvad 源码：`git clone --depth 1 --filter=blob:none --sparse https://github.com/microsoft/Windows-driver-samples.git`
   然后 `git sparse-checkout set audio/sysvad`（提交 `d49bb7d7`，2026-11-05）。github release 下载不需要。
2. WDK：**不安装 MSI**，用 NuGet 包（与 Windows-driver-samples 仓库的 `packages.config` 同版本）：
   `Microsoft.Windows.WDK.x64`、`Microsoft.Windows.SDK.CPP`、`Microsoft.Windows.SDK.CPP.x64`，版本 `10.0.28000.2526`。
   `driver\packages.config` + `driver\Directory.Build.props` 负责还原和导入，nuget.exe 已在 PATH（没有的话 build.ps1 会自动下载）。
   包内自带 stampinf、InfVerif、Inf2Cat、signtool、devgen，无需 VS 的 WDK 扩展。
3. VS 需要“使用 C++ 的桌面开发”工作负载（MSBuild + MSVC）。

### 命令

```powershell
cd E:\tts\driver
.\build.ps1                 # Release，输出到 .\package\
.\build.ps1 -Configuration Debug
.\tests\loopback\run_test.ps1   # 环形缓冲单元测试
```

首次运行会在当前用户证书库生成测试证书 `CN=AIVoiceChanger Test Signing (DO NOT TRUST)`（有效期 5 年），之后复用。

## 本机已完成的验证（2026-11-21）

| 项目 | 结果 |
|---|---|
| 编译 Release x64 / Debug x64（`/W4 /WX` 警告即错误） | ✅ 0 错误 0 警告 |
| InfVerif `/w`（Windows Driver）、`/h`（WHQL）、`/u` | ✅ 均通过 |
| Inf2Cat（10_VB/CO/NI/GE_X64、ServerFE_X64） | ✅ 生成 `AIVirtualAudio.cat`，无错误无警告 |
| signtool 测试签名 `.sys` + `.cat`（SHA256） | ✅ 已签名；`.sys` 哈希已登记在 `.cat` 中 |
| 导入表检查 | ✅ 只用到 Win10 2004 (19041) 起可用的内核 API（ExAllocatePool2 等），与 INF 的 `10.0...19041` 一致；KMDF 1.15 |
| 环形缓冲用户态单元测试（同格式、单→双声道、44.1k→48k、48k→44.1k、渲染停止、多读者、非法格式） | ✅ 帧数精确、首包延迟 28ms、无掉音、无断点跳变、停止后完全静音 |
| 安装脚本：PowerShell 7 与 Windows PowerShell 5.1 语法解析、SetupAPI 封装编译、只读设备查询 | ✅ |

**未能在本机验证**（本机是云服务器，开测试签名需要重启、会断开会话，按要求没有执行 bcdedit/重启/安装）：
- 驱动实际加载、端点出现、真实播放→录制的音质与延迟、爆音情况
- `install_test.ps1` / `uninstall.ps1` 的实际安装/卸载流程
- Driver Verifier 下的稳定性、睡眠/唤醒、长时间运行
→ 请按下面的清单在自己的电脑上测试。

## 安装测试清单（在你自己的 Win10 2004+/Win11 x64 电脑上）

> 建议先建系统还原点。测试签名模式会让桌面右下角显示“测试模式”水印，测完可关闭。

**准备**
1. 把整个 `driver\` 目录（至少 `package\`、`scripts\`、`install_test.ps1`、`uninstall.ps1`）拷到测试电脑。
2. 若开启了 Secure Boot：重启进 BIOS/UEFI 关闭 Secure Boot（`msinfo32` → “安全启动状态”可查看）。
3. 管理员 PowerShell：
   ```powershell
   Set-ExecutionPolicy -Scope Process Bypass
   cd <拷贝到的目录>
   .\install_test.ps1 -EnableTestSigning   # 未开测试模式时：执行 bcdedit /set testsigning on，然后提示重启
   ```
4. 重启电脑。

**安装**
5. 管理员 PowerShell 再次运行 `.\install_test.ps1`。预期：
   - [ ] 输出“测试签名模式：已开启”、证书导入成功、pnputil 成功、“已创建设备节点 ROOT\MEDIA\000x”
   - [ ] 设备状态 `OK`；设备管理器“声音、视频和游戏控制器”下出现 **AI变声器 虚拟声卡**，无黄色感叹号
   - [ ] “声音设置 → 输出”出现 **AI变声器 内部输出**；“输入”出现 **AI变声器 麦克风**

**功能**
6. [ ] 系统声音面板 → 录制 → AI变声器 麦克风 → 属性 → 高级：默认格式可选 44100/48000Hz、16/24/32 位、单/双声道
7. [ ] 把任意播放器（如浏览器放音乐）的输出切到“AI变声器 内部输出”，录制面板里“AI变声器 麦克风”的电平条跟着跳动
8. [ ] 用“录音机”或 Audacity 从“AI变声器 麦克风”录 1 分钟：声音完整、无爆音、无断续、音调正常（未变快变慢）
9. [ ] 停止播放：录音变为完全静音，停止瞬间无“啪”声；重新播放能自动恢复
10. [ ] 把播放端格式改为 44100Hz、录制端保持 48000Hz（以及反过来），重复 8：音调正常
11. [ ] 同时用两个程序（如 OBS + 录音机）录“AI变声器 麦克风”，两边都正常
12. [ ] 调节两个端点的音量滑块/静音按钮，录到的音量随之变化
13. [ ] 抖音直播伴侣：麦克风选“AI变声器 麦克风”，开测试直播/录制，确认对方听到变声后的声音
14. [ ] 变声器（主会话完成引擎改动后）自动选择“AI变声器 内部输出”作为输出设备，端到端延迟主观可接受
15. [ ] 睡眠→唤醒、重启后，两个端点仍在且正常工作

**卸载**
16. 管理员 PowerShell：`.\uninstall.ps1 -RemoveCertificate`
    - [ ] 设备与端点消失，`pnputil /enum-drivers` 中不再有 AIVirtualAudio.inf
17. （可选）`bcdedit /set testsigning off` 并重启，恢复 Secure Boot。

**出问题时请收集**：install_test.ps1 的完整输出、设备管理器中设备的错误代码、
`C:\Windows\INF\setupapi.dev.log` 末尾与 AIVoiceChanger 相关的段落、事件查看器 → Windows 日志 → 系统中的相关错误。
如蓝屏，请保留 `C:\Windows\MEMORY.DMP` / `C:\Windows\Minidump\*.dmp` 与 `package\AIVirtualAudio.pdb`。

## 给引擎（主会话）的集成说明

- 输出设备选择：在 WASAPI / PortAudio 设备列表中按名称匹配 **`AI变声器 内部输出`**（Windows 显示为
  `AI变声器 内部输出 (AI变声器 虚拟声卡)`，用“包含 `AI变声器 内部输出`”匹配即可）。
- 引擎输出 48000Hz 单声道即可：共享模式下 Windows 音频引擎会转换成端点格式（默认 48k 立体声 16bit），驱动内部再
  按需转换，单声道会复制到左右声道；直播软件录到的是 48k。
- 独占模式也可用（端点支持 16/24/32bit、1/2 声道、44.1k/48k）。

## 相对 sysvad 的主要改动

- 删除蓝牙 HFP/A2DP、USB 耳机、HDMI、SPDIF、麦克风阵列、耳机端点、APO、关键字检测等全部无关代码与 INF 段；
  只保留 TabletAudioSample 的 Speaker + MicIn 两对 miniport
- 渲染端去掉硬件 offload/硬件环回 pin（改用 2-pin 布局），系统 pin 限 1 个实例，只支持 DEFAULT 模式
- 采集端不再生成正弦测试音，改为从环形缓冲读取；拓扑去掉硬件音量/静音/峰值节点（交给系统软件音量）
- `minwavertstream.cpp`：位置推进时渲染流把“已播放”的字节写入环形缓冲，采集流从环形缓冲填充 DMA 缓冲
- `adapter.cpp`：DriverEntry 分配环形缓冲、卸载时释放；驱动参数键缺失时用默认值而不是加载失败
- 关闭电源空闲（不写 PowerSettings），避免设备进入 D3 后开流延迟
- INF：中文友好名、端点名称（MediaCategories）、`ROOT\AIVoiceChanger`、Win10 19041+ 修饰、`DoNotCreateDataFiles=1`

## 已知限制 / 后续可改进

- 重采样是线性插值（44.1k↔48k 时高频会有轻微滚降/混叠，语音直播可接受；两端都用 48k 时无重采样）
- 只支持整数 PCM（不含 32bit float 格式；共享模式不受影响，系统音频引擎内部就是 float 再转换）
- 渲染端同一时间只允许 1 个流；独占模式占用时其他程序无法播放到“内部输出”（与普通声卡一致）
- 仅 x64；如需 ARM64，在 packages.config 加 `Microsoft.Windows.WDK.arm64` 并在 vcxproj 加 ARM64 配置
