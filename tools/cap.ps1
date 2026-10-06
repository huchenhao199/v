# cap.ps1 -- 抓 fp.exe 窗口的图（GPU 交换链只能从屏幕抓，PrintWindow 拿不到）
# 用法: powershell -ExecutionPolicy Bypass -File cap.ps1 -Out C:\...\shots\x.png [-Wait 3] [-Keys "{RIGHT}"]
param(
    [string]$Out = "shot.png",
    [string]$Class = "fpclass",
    [int]$X = 40,
    [int]$Y = 40,
    [double]$Wait = 0,
    [string]$Keys = "",
    [switch]$NoMove
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -ReferencedAssemblies System.Drawing, System.Windows.Forms -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public class Cap2 {
    [DllImport("user32.dll")] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public static IntPtr H(string cls) {
        IntPtr h = FindWindow(cls, null);
        if (h == IntPtr.Zero) throw new Exception("no window of class: " + cls);
        return h;
    }
    public static void Place(string cls, int x, int y) {
        IntPtr h = H(cls);
        ShowWindow(h, 9);                                // SW_RESTORE
        SetWindowPos(h, IntPtr.Zero, x, y, 0, 0, 0x0001 | 0x0010);  // NOSIZE | NOZORDER
        SetForegroundWindow(h);
        System.Threading.Thread.Sleep(700);
    }
    public static RECT WinRect(string cls) {
        IntPtr h = H(cls);
        RECT r;
        if (!GetWindowRect(h, out r)) throw new Exception("GetWindowRect failed");
        SetForegroundWindow(h);
        System.Threading.Thread.Sleep(300);
        return r;
    }
    public static void Shot(RECT r, string path) {
        int w = r.R - r.L, h = r.B - r.T;
        using (Bitmap bmp = new Bitmap(w, h)) {
            using (Graphics g = Graphics.FromImage(bmp)) {
                g.CopyFromScreen(r.L, r.T, 0, 0, new Size(w, h));
            }
            bmp.Save(path, ImageFormat.Png);
        }
    }
    public static void Keys(string s) { System.Windows.Forms.SendKeys.SendWait(s); }
}
"@

$full = $Out
if (-not [System.IO.Path]::IsPathRooted($full)) {
    $full = [System.IO.Path]::Combine((Get-Location).ProviderPath, $Out)
}
$dir = [System.IO.Path]::GetDirectoryName($full)
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }

if ($Wait -gt 0) { Start-Sleep -Seconds $Wait }
if (-not $NoMove) { [Cap2]::Place($Class, $X, $Y) }
$r = [Cap2]::WinRect($Class)
if ($Keys -ne "") { [Cap2]::Keys($Keys); Start-Sleep -Milliseconds 400 }
[Cap2]::Shot($r, $full)
Write-Output ("saved: " + $full + "  (" + ($r.R - $r.L) + "x" + ($r.B - $r.T) + " at " + $r.L + "," + $r.T + ")")
