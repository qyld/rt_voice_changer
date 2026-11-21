# AivcSetupApi.ps1 —— 给 install_test.ps1 / uninstall.ps1 使用的 SetupAPI 封装。
# 作用等同于 devcon install / devcon remove：创建或删除 ROOT 枚举的虚拟设备，
# 这样用户电脑上无需安装 WDK（devcon/devgen 不随系统提供）。

if (-not ('AivcSetupApi' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class AivcSetupApi
{
    const int DIF_REMOVE         = 0x00000005;
    const int DIF_REGISTERDEVICE = 0x00000019;
    const int DICD_GENERATE_ID   = 0x00000001;
    const int SPDRP_HARDWAREID   = 0x00000001;
    const int DIGCF_ALLCLASSES   = 0x00000004;
    const int INSTALLFLAG_FORCE  = 0x00000001;
    const int ERROR_NO_MORE_ITEMS = 259;
    static readonly IntPtr INVALID_HANDLE_VALUE = new IntPtr(-1);

    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVINFO_DATA
    {
        public int    cbSize;
        public Guid   ClassGuid;
        public int    DevInst;
        public IntPtr Reserved;
    }

    [DllImport("setupapi.dll", SetLastError = true)]
    static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid ClassGuid, IntPtr hwndParent);

    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr SetupDiGetClassDevsW(IntPtr ClassGuid, string Enumerator, IntPtr hwndParent, int Flags);

    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiCreateDeviceInfoW(IntPtr DeviceInfoSet, string DeviceName, ref Guid ClassGuid,
        string DeviceDescription, IntPtr hwndParent, int CreationFlags, ref SP_DEVINFO_DATA DeviceInfoData);

    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData,
        int Property, byte[] PropertyBuffer, int PropertyBufferSize);

    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiGetDeviceRegistryPropertyW(IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData,
        int Property, out int PropertyRegDataType, byte[] PropertyBuffer, int PropertyBufferSize, out int RequiredSize);

    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiGetDeviceInstanceIdW(IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData,
        StringBuilder DeviceInstanceId, int DeviceInstanceIdSize, out int RequiredSize);

    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiEnumDeviceInfo(IntPtr DeviceInfoSet, int MemberIndex, ref SP_DEVINFO_DATA DeviceInfoData);

    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiCallClassInstaller(int InstallFunction, IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData);

    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);

    [DllImport("newdev.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwndParent, string HardwareId, string FullInfPath,
        int InstallFlags, out bool bRebootRequired);

    static Exception LastError(string what)
    {
        return new Win32Exception(Marshal.GetLastWin32Error(), what);
    }

    static SP_DEVINFO_DATA NewData()
    {
        SP_DEVINFO_DATA d = new SP_DEVINFO_DATA();
        d.cbSize = Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
        return d;
    }

    // 创建 ROOT\<ClassName>\NNNN 设备节点并设置硬件 ID，返回设备实例 ID。
    public static string CreateRootDevice(string className, Guid classGuid, string hardwareId, string description)
    {
        IntPtr set = SetupDiCreateDeviceInfoList(ref classGuid, IntPtr.Zero);
        if (set == INVALID_HANDLE_VALUE) throw LastError("SetupDiCreateDeviceInfoList");
        try
        {
            SP_DEVINFO_DATA d = NewData();
            if (!SetupDiCreateDeviceInfoW(set, className, ref classGuid, description, IntPtr.Zero, DICD_GENERATE_ID, ref d))
                throw LastError("SetupDiCreateDeviceInfo");

            byte[] multiSz = Encoding.Unicode.GetBytes(hardwareId + "\0\0");
            if (!SetupDiSetDeviceRegistryPropertyW(set, ref d, SPDRP_HARDWAREID, multiSz, multiSz.Length))
                throw LastError("SetupDiSetDeviceRegistryProperty(HardwareID)");

            if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, ref d))
                throw LastError("SetupDiCallClassInstaller(DIF_REGISTERDEVICE)");

            return GetInstanceId(set, ref d);
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(set);
        }
    }

    // 为所有匹配该硬件 ID 的设备安装/更新指定 INF 的驱动。返回是否需要重启。
    public static bool UpdateDriver(string hardwareId, string infPath)
    {
        bool reboot;
        if (!UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero, hardwareId, infPath, INSTALLFLAG_FORCE, out reboot))
            throw LastError("UpdateDriverForPlugAndPlayDevices");
        return reboot;
    }

    static string GetInstanceId(IntPtr set, ref SP_DEVINFO_DATA d)
    {
        StringBuilder sb = new StringBuilder(512);
        int req;
        if (!SetupDiGetDeviceInstanceIdW(set, ref d, sb, sb.Capacity, out req))
            throw LastError("SetupDiGetDeviceInstanceId");
        return sb.ToString();
    }

    static bool HasHardwareId(IntPtr set, ref SP_DEVINFO_DATA d, string hardwareId)
    {
        byte[] buf = new byte[4096];
        int type, req;
        if (!SetupDiGetDeviceRegistryPropertyW(set, ref d, SPDRP_HARDWAREID, out type, buf, buf.Length, out req))
            return false;
        string all = Encoding.Unicode.GetString(buf, 0, Math.Min(req, buf.Length));
        foreach (string id in all.Split('\0'))
        {
            if (string.Equals(id, hardwareId, StringComparison.OrdinalIgnoreCase)) return true;
        }
        return false;
    }

    // 列出（包括未连接的）所有带该硬件 ID 的设备实例 ID。
    public static string[] FindDevices(string hardwareId)
    {
        List<string> result = new List<string>();
        IntPtr set = SetupDiGetClassDevsW(IntPtr.Zero, null, IntPtr.Zero, DIGCF_ALLCLASSES);
        if (set == INVALID_HANDLE_VALUE) throw LastError("SetupDiGetClassDevs");
        try
        {
            for (int i = 0; ; i++)
            {
                SP_DEVINFO_DATA d = NewData();
                if (!SetupDiEnumDeviceInfo(set, i, ref d))
                {
                    if (Marshal.GetLastWin32Error() == ERROR_NO_MORE_ITEMS) break;
                    throw LastError("SetupDiEnumDeviceInfo");
                }
                if (HasHardwareId(set, ref d, hardwareId)) result.Add(GetInstanceId(set, ref d));
            }
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(set);
        }
        return result.ToArray();
    }

    // 删除所有带该硬件 ID 的设备节点，返回删除数量。
    public static int RemoveDevices(string hardwareId)
    {
        int removed = 0;
        IntPtr set = SetupDiGetClassDevsW(IntPtr.Zero, null, IntPtr.Zero, DIGCF_ALLCLASSES);
        if (set == INVALID_HANDLE_VALUE) throw LastError("SetupDiGetClassDevs");
        try
        {
            for (int i = 0; ; i++)
            {
                SP_DEVINFO_DATA d = NewData();
                if (!SetupDiEnumDeviceInfo(set, i, ref d))
                {
                    if (Marshal.GetLastWin32Error() == ERROR_NO_MORE_ITEMS) break;
                    throw LastError("SetupDiEnumDeviceInfo");
                }
                if (!HasHardwareId(set, ref d, hardwareId)) continue;
                if (!SetupDiCallClassInstaller(DIF_REMOVE, set, ref d))
                    throw LastError("SetupDiCallClassInstaller(DIF_REMOVE)");
                removed++;
            }
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(set);
        }
        return removed;
    }
}
'@
}

$script:AivcHardwareId = 'ROOT\AIVoiceChanger'
$script:AivcMediaClassGuid = [Guid]'4d36e96c-e325-11ce-bfc1-08002be10318'
$script:AivcCertSubject = 'CN=AIVoiceChanger Test Signing (DO NOT TRUST)'

function Test-AivcAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# 在 %windir%\INF\oem*.inf 中查找本驱动包（按硬件 ID 匹配，不依赖 pnputil 的本地化输出）
function Get-AivcOemInf {
    Get-ChildItem (Join-Path $env:windir 'INF') -Filter 'oem*.inf' -ErrorAction SilentlyContinue |
        Where-Object { Select-String -Path $_.FullName -Pattern 'ROOT\AIVoiceChanger' -SimpleMatch -Quiet } |
        ForEach-Object { $_.Name }
}
