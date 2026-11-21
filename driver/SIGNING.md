# AI变声器 虚拟声卡 —— 驱动签名与正式发布流程

> 当前仓库里的驱动包只用**自签名测试证书**签过名，只能在开启了“测试签名模式”的电脑上安装
> （见 [README.md](README.md) 的测试清单）。要让普通 Windows 10/11 用户**双击即装、无需改系统设置**，
> 必须走下面的“EV 证书 + 微软证明签名（Attestation Signing）”流程。

## 为什么需要微软签名

从 Windows 10 1607 起，在开启了安全启动（Secure Boot）的电脑上，**内核驱动必须由微软签名**
（通过硬件开发者中心提交），厂商自己的代码签名证书（哪怕是 EV 证书）签的 `.sys` 也会被拒绝加载
（设备管理器错误代码 52）。厂商证书的作用只是：**证明“是谁提交的”**，让微软给驱动包加签。

| 方式 | 谁签名 | 用户需要做什么 | 适用 |
|---|---|---|---|
| 测试签名（当前） | 自签测试证书 | 关闭 Secure Boot + `bcdedit /set testsigning on` + 重启，桌面出现“测试模式”水印 | 开发自测 |
| **证明签名 Attestation** | 微软 | 什么都不用做，直接安装 | **正式发布 Win10/11 客户端（推荐）** |
| HLK 认证（WHQL） | 微软 | 什么都不用做；还可通过 Windows Update 分发 | 需要 Windows Server 支持、或想进 Windows Update / 拿“Windows 兼容”徽标 |

## 正式发布流程（证明签名）

### 1. 购买 EV 代码签名证书（一次性，约 1~2 周）

- 向微软认可的 CA 购买 **EV（扩展验证）代码签名证书**：DigiCert、Sectigo、GlobalSign、SSL.com、Entrust 等。
  国内也可通过这些 CA 的代理商购买（如沃通、天威诚信代理的 DigiCert/GlobalSign 产品——以微软
  [Partner Center 支持的 CA 列表](https://learn.microsoft.com/windows-hardware/drivers/dashboard/code-signing-reqs) 为准）。
- 申请主体需要是**公司/组织**（EV 证书要求营业执照、邓白氏编码 DUNS 或等效的组织验证、电话核验）。
- 证书私钥存放在 CA 寄送的**硬件令牌（USB Key）**或云 HSM 中，无法导出；签名需插着令牌操作。
- 价格参考：每年约 300~600 美元（视 CA 和年限）。

### 2. 注册微软合作伙伴中心 — 硬件开发者计划（一次性，免费）

1. 打开 <https://partner.microsoft.com/dashboard/registration/hardware>，用公司的 Microsoft Entra ID（工作账号）登录，
   没有的话按页面提示新建（免费）。
2. 选择注册“硬件开发者计划 / Hardware program”。
3. **用 EV 证书签名验证身份**：页面会提供一个 `SignableFile.bin`，用 EV 证书签名后上传：
   ```powershell
   signtool sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 /a SignableFile.bin
   ```
4. 填写公司信息、签署《Windows 硬件兼容性计划协议》等法律协议。

### 3. 准备要提交的驱动包（每次发版）

1. 修改版本号：`src\AIVirtualAudio\AIVirtualAudio.vcxproj` 里 `<Inf><TimeStamp>` 设为新版本（如 `1.0.1.0`），
   `AIVirtualAudio.rc` 里 `VER_PRODUCTVERSION` 同步。
2. 正式发布建议改用自己公司的信息：`AIVirtualAudio.inx` 的 `ProviderName`/`MfgName`。
   **硬件 ID `ROOT\AIVoiceChanger` 发布后不要再改**（否则升级时会变成两个设备）。
3. 编译 Release 并生成 `.cat`（不需要测试签名）：
   ```powershell
   .\build.ps1 -SkipSign            # 只编译+InfVerif，不签名
   # 然后单独生成 cat：
   & .\packages\Microsoft.Windows.WDK.x64.10.0.28000.2526\c\bin\10.0.28000.0\x86\Inf2Cat.exe `
       /driver:.\package /os:10_VB_X64,10_CO_X64,10_NI_X64,10_GE_X64
   ```
4. 用 **EV 证书**签名 `.sys` 和 `.cat`（插好 USB Key）：
   ```powershell
   signtool sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 /a package\AIVirtualAudio.sys
   signtool sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 /a package\AIVirtualAudio.cat
   ```
5. 打成 CAB（只放驱动文件，不要放 `.cer`/`.pdb` 以外的无关文件；`.pdb` 可选，用于微软的崩溃分析）：
   ```text
   ; aivc.ddf
   .OPTION EXPLICIT
   .Set CabinetFileCountThreshold=0
   .Set FolderFileCountThreshold=0
   .Set FolderSizeThreshold=0
   .Set MaxCabinetSize=0
   .Set MaxDiskFileCount=0
   .Set MaxDiskSize=0
   .Set CompressionType=MSZIP
   .Set Cabinet=on
   .Set Compress=on
   .Set CabinetNameTemplate=AIVirtualAudio.cab
   .Set DestinationDir=AIVirtualAudio
   package\AIVirtualAudio.inf
   package\AIVirtualAudio.sys
   package\AIVirtualAudio.cat
   package\AIVirtualAudio.pdb
   ```
   ```powershell
   makecab /f aivc.ddf
   signtool sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 /a disk1\AIVirtualAudio.cab
   ```
   **CAB 本身也必须用 EV 证书签名**，否则提交会被拒。

### 4. 在硬件开发者中心提交证明签名

1. 合作伙伴中心 → 硬件 → **提交新硬件（Submit new hardware）**。
2. 产品名称填 “AI变声器 虚拟声卡 1.0.x”，上传签好名的 `AIVirtualAudio.cab`。
3. **不要**勾选需要 HLK 测试结果的选项；“请求的签名”勾选：
   - `Windows 10 Client version 2004 (Vibranium) x64` 及以上各版本（与 INF 中 `NTamd64.10.0...19041` 一致）
   - `Windows 11 Client x64`（各版本）
4. 提交，等待处理（通常几十分钟到几小时）。
5. 完成后下载签名后的驱动包：里面的 `.sys` 和 `.cat` 已被替换为**微软签名**（签名者 “Microsoft Windows Hardware
   Compatibility Publisher”）。

### 5. 分发与安装

- 把微软签名后的 `AIVirtualAudio.inf/.sys/.cat` 放进变声器安装包。
- 安装程序以管理员权限执行（与 `install_test.ps1` 相同的步骤，但**不需要**测试签名模式、**不需要**导入证书）：
  1. `pnputil /add-driver AIVirtualAudio.inf`
  2. 创建 `ROOT\AIVoiceChanger` 设备节点并安装驱动（见 `scripts\AivcSetupApi.ps1` 的 `CreateRootDevice` +
     `UpdateDriver`，可原样移植到安装程序里，或者用 C++ 调同样的 SetupAPI 函数）。
- 卸载：同 `uninstall.ps1`（删除设备节点 + `pnputil /delete-driver oemNN.inf /uninstall`）。
- 发布前请在一台**开启 Secure Boot、未开测试模式**的干净 Win10 22H2 与 Win11 24H2 上各装一遍确认。

### 证明签名的限制

- 只对 **Windows 10/11 客户端**有效；Windows Server 不接受证明签名的驱动（需要 HLK）。
- 不能通过 Windows Update 自动推送（只能随自己的安装包分发）。
- 微软会对提交的驱动做恶意软件扫描；驱动必须是自己公司开发/拥有分发权的。sysvad 示例以 MS-PL 许可发布，
  允许修改后分发，需保留许可证声明（见 `src\SYSVAD_LICENSE.txt`）。

## 可选：HLK 认证（WHQL）

只有在以下情况才需要：要支持 Windows Server、想通过 Windows Update 分发、或要“Windows 认证”徽标。

1. 准备测试环境：一台装 **HLK Controller**（Windows Server）+ 一台或多台**被测机**（每个要认证的 Windows 版本一台，
   版本需与 HLK 版本匹配，如 HLK for Windows 11 24H2）。
2. 在被测机安装驱动（测试签名即可），HLK Studio 中选中 “AI变声器 虚拟声卡” 设备，运行 **Device.Audio** 相关测试
   （音频端点、格式、延迟、功耗、PnP 卸载/重装、DevFund 可靠性测试等）。虚拟声卡没有真实硬件，部分测试可能需要
   申请豁免（errata / playlist 例外）。
3. 导出 `.hlkx` 测试包，用 EV 证书签名后在硬件开发者中心提交“HLK 认证”。
4. 通过后得到 WHQL 签名驱动，可选择发布到 Windows Update。

投入较大（测试机、数天到数周的测试时间），对“直播用虚拟声卡”通常**没有必要**，证明签名即可满足普通用户安装。

## 常见问题

- **设备管理器错误代码 52**：签名不被信任。测试版：确认已开测试签名模式且 `.cer` 已导入 Root/TrustedPublisher；
  正式版：确认装的是微软签名后的包。
- **`bcdedit /set testsigning on` 报“受安全启动策略保护”**：需要先在 BIOS 关闭 Secure Boot。正式版不需要这一步。
- **测试证书安全性**：`AIVoiceChangerTest.cer` 只有公钥；私钥只在构建机 `CurrentUser\My` 中。不要把测试证书导入到
  普通用户电脑长期使用，测试完运行 `uninstall.ps1 -RemoveCertificate`。
