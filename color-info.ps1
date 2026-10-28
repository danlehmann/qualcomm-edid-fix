# Prints each active display path's mode, bits per channel and colour encoding
# (DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO).
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class ColorInfo {
    [StructLayout(LayoutKind.Sequential)] struct LUID { public uint Low; public int High; }
    [StructLayout(LayoutKind.Sequential)] struct PATH_SOURCE { public LUID adapterId; public uint id; public uint modeInfoIdx; public uint statusFlags; }
    [StructLayout(LayoutKind.Sequential)] struct RATIONAL { public uint Numerator; public uint Denominator; }
    [StructLayout(LayoutKind.Sequential)] struct PATH_TARGET { public LUID adapterId; public uint id; public uint modeInfoIdx; public uint outputTechnology; public uint rotation; public uint scaling; public RATIONAL refreshRate; public uint scanLineOrdering; public int targetAvailable; public uint statusFlags; }
    [StructLayout(LayoutKind.Sequential)] struct PATH_INFO { public PATH_SOURCE sourceInfo; public PATH_TARGET targetInfo; public uint flags; }
    [StructLayout(LayoutKind.Sequential, Size = 64)] struct MODE_INFO { public uint infoType; public uint id; public LUID adapterId; }
    [StructLayout(LayoutKind.Sequential)] struct HEADER { public uint type; public uint size; public LUID adapterId; public uint id; }
    [StructLayout(LayoutKind.Sequential)] struct ADV_COLOR { public HEADER header; public uint value; public uint colorEncoding; public uint bitsPerColorChannel; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct TARGET_NAME { public HEADER header; public uint flags; public uint outputTechnology; public ushort edidManufactureId; public ushort edidProductCodeId; public uint connectorInstance; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string monitorFriendlyDeviceName; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string monitorDevicePath; }
    [DllImport("user32.dll")] static extern int GetDisplayConfigBufferSizes(uint flags, out uint numPaths, out uint numModes);
    [DllImport("user32.dll")] static extern int QueryDisplayConfig(uint flags, ref uint numPaths, [Out] PATH_INFO[] paths, ref uint numModes, [Out] MODE_INFO[] modes, IntPtr topology);
    [DllImport("user32.dll")] static extern int DisplayConfigGetDeviceInfo(ref ADV_COLOR info);
    [DllImport("user32.dll")] static extern int DisplayConfigGetDeviceInfo(ref TARGET_NAME info);
    public static List<string> Run() {
        var r = new List<string>();
        uint np, nm; GetDisplayConfigBufferSizes(2, out np, out nm);
        var paths = new PATH_INFO[np]; var modes = new MODE_INFO[nm];
        int err = QueryDisplayConfig(2, ref np, paths, ref nm, modes, IntPtr.Zero);
        if (err != 0) { r.Add("QueryDisplayConfig error " + err); return r; }
        string[] enc = { "RGB", "YCbCr444", "YCbCr422", "YCbCr420", "Intensity" };
        for (int i = 0; i < np; i++) {
            var t = paths[i].targetInfo;
            var name = new TARGET_NAME(); name.header.type = 2; name.header.size = (uint)Marshal.SizeOf(name); name.header.adapterId = t.adapterId; name.header.id = t.id;
            DisplayConfigGetDeviceInfo(ref name);
            var c = new ADV_COLOR(); c.header.type = 9; c.header.size = (uint)Marshal.SizeOf(c); c.header.adapterId = t.adapterId; c.header.id = t.id;
            int e2 = DisplayConfigGetDeviceInfo(ref c);
            double hz = t.refreshRate.Denominator == 0 ? 0 : (double)t.refreshRate.Numerator / t.refreshRate.Denominator;
            r.Add(string.Format("{0,-12} target {1}: {2:F2} Hz, {3} bpc, {4}, advanced colour flags 0x{5:X} (err {6})",
                name.monitorFriendlyDeviceName, t.id, hz, c.bitsPerColorChannel,
                c.colorEncoding < enc.Length ? enc[c.colorEncoding] : c.colorEncoding.ToString(), c.value, e2));
        }
        return r;
    }
}
'@
[ColorInfo]::Run()
