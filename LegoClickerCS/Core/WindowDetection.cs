using System;
using System.Runtime.InteropServices;
using System.Text;

namespace LegoClickerCS.Core;

public static class WindowDetection
{
    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();
    
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int count);
    
    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
    
    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hWnd);
    
    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    
    private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }
    
    private static readonly string[] GameTitles = { "Minecraft", "Lunar Client", "Badlion" };
    
    private static IntPtr _foundWindow = IntPtr.Zero;
    
    public static bool IsMinecraftActive()
    {
        IntPtr hwnd = GetForegroundWindow();
        if (hwnd == IntPtr.Zero) return false;
        
        StringBuilder title = new StringBuilder(256);
        GetWindowText(hwnd, title, 256);
        string windowTitle = title.ToString();
        
        foreach (string gameTitle in GameTitles)
        {
            if (windowTitle.Contains(gameTitle, StringComparison.OrdinalIgnoreCase))
                return true;
        }
        
        return false;
    }
    
    /// <summary>
    /// Checks if Minecraft window is the FOREGROUND window (not just visible).
    /// This means no other window is on top of it.
    /// </summary>
    public static bool IsMinecraftForeground()
    {
        IntPtr foreground = GetForegroundWindow();
        if (foreground == IntPtr.Zero) return false;
        
        IntPtr minecraft = FindMinecraftWindow();
        if (minecraft == IntPtr.Zero) return false;
        
        // Must be the exact same window handle
        return foreground == minecraft;
    }
    
    public static IntPtr FindMinecraftWindow()
    {
        _foundWindow = IntPtr.Zero;
        EnumWindows(EnumWindowCallback, IntPtr.Zero);
        return _foundWindow;
    }
    
    private static bool EnumWindowCallback(IntPtr hWnd, IntPtr lParam)
    {
        if (!IsWindowVisible(hWnd)) return true;
        
        StringBuilder title = new StringBuilder(256);
        GetWindowText(hWnd, title, 256);
        string windowTitle = title.ToString();
        
        foreach (string gameTitle in GameTitles)
        {
            if (windowTitle.Contains(gameTitle, StringComparison.OrdinalIgnoreCase))
            {
                _foundWindow = hWnd;
                return false; // Stop enumeration
            }
        }
        
        return true; // Continue enumeration
    }
    
    public static RECT? GetMinecraftWindowRect()
    {
        IntPtr hwnd = FindMinecraftWindow();
        if (hwnd == IntPtr.Zero) return null;
        
        if (GetWindowRect(hwnd, out RECT rect))
            return rect;
        
        return null;
    }
    [DllImport("user32.dll")]
    private static extern bool GetCursorInfo(ref CURSORINFO pci);

    [StructLayout(LayoutKind.Sequential)]
    private struct CURSORINFO
    {
        public int cbSize;
        public int flags;
        public IntPtr hCursor;
        public POINT ptScreenPos;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT
    {
        public int x;
        public int y;
    }

    private const int CURSOR_SHOWING = 0x00000001;

    public static bool IsCursorVisible()
    {
        CURSORINFO pci = new CURSORINFO();
        pci.cbSize = Marshal.SizeOf(typeof(CURSORINFO));
        
        if (GetCursorInfo(ref pci))
        {
            return (pci.flags & CURSOR_SHOWING) != 0;
        }
        
        return false;
    }
}
