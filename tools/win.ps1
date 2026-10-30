# Window helper. Usage:
#   win.ps1 list [procname]            -> list visible top-level windows (hwnd, pid, title, rect)
#   win.ps1 shot <hwnd> <out.png>      -> capture window via PrintWindow (works when display is off)
#   win.ps1 burst <hwnd> <outprefix> [count=12] [ms=80]  -> numbered PNGs, for motion glitches
#   win.ps1 keys <hwnd> <sendkeys>     -> focus window and send keys (System.Windows.Forms.SendKeys syntax)
param([string]$Cmd, [string]$A, [string]$B, [int]$C = 12, [int]$D = 80)
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text; using System.Collections.Generic;
public class W {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public struct RECT { public int L,T,R,B; }
  public static List<IntPtr> All() { var l=new List<IntPtr>(); EnumWindows((h,p)=>{ if(IsWindowVisible(h)) l.Add(h); return true; }, IntPtr.Zero); return l; }
}
"@
[void][W]::SetProcessDPIAware()
switch ($Cmd) {
 'list' {
   foreach ($h in [W]::All()) {
     $sb = New-Object System.Text.StringBuilder 256; [void][W]::GetWindowText($h,$sb,256)
     $p=0; [void][W]::GetWindowThreadProcessId($h,[ref]$p); $pn=(Get-Process -Id $p -EA SilentlyContinue).ProcessName
     if ($A -and $pn -notlike "*$A*") { continue }
     $r=New-Object W+RECT; [void][W]::GetWindowRect($h,[ref]$r)
     if ($sb.Length -gt 0 -or $A) { "{0}`t{1}`t{2}`t{3}`t{4}x{5}" -f $h,$pn,$p,$sb,($r.R-$r.L),($r.B-$r.T) }
   } }
 'shot' {
   $h=[IntPtr][long]$A; $r=New-Object W+RECT; [void][W]::GetWindowRect($h,[ref]$r)
   $w=[Math]::Max(1,$r.R-$r.L); $hh=[Math]::Max(1,$r.B-$r.T)
   $bmp=New-Object System.Drawing.Bitmap $w,$hh; $g=[System.Drawing.Graphics]::FromImage($bmp); $dc=$g.GetHdc()
   [void][W]::PrintWindow($h,$dc,2); $g.ReleaseHdc($dc); $bmp.Save($B,[System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose(); $B }
 'burst' {
   $h=[IntPtr][long]$A; $r=New-Object W+RECT; [void][W]::GetWindowRect($h,[ref]$r)
   $w=[Math]::Max(1,$r.R-$r.L); $hh=[Math]::Max(1,$r.B-$r.T)
   for ($i=0; $i -lt $C; $i++) {
     $bmp=New-Object System.Drawing.Bitmap $w,$hh; $g=[System.Drawing.Graphics]::FromImage($bmp); $dc=$g.GetHdc()
     [void][W]::PrintWindow($h,$dc,2); $g.ReleaseHdc($dc); $f=('{0}_{1:D2}.png' -f $B,$i); $bmp.Save($f,[System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose(); $f
     Start-Sleep -Milliseconds $D } }
 'keys' { $h=[IntPtr][long]$A; [void][W]::ShowWindow($h,9); [void][W]::SetForegroundWindow($h); Start-Sleep -Milliseconds 300; [System.Windows.Forms.SendKeys]::SendWait($B) }
}
