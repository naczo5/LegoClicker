using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Windows;

namespace LegoClickerCS.Core;

public static class InputHooks
{
    // P/Invoke declarations
    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SetWindowsHookEx(int idHook, LowLevelProc lpfn, IntPtr hMod, uint dwThreadId);
    
    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UnhookWindowsHookEx(IntPtr hhk);
    
    [DllImport("user32.dll")]
    private static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);
    
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr GetModuleHandle(string? lpModuleName);
    
    private delegate IntPtr LowLevelProc(int nCode, IntPtr wParam, IntPtr lParam);
    
    private const int WH_KEYBOARD_LL = 13;
    private const int WH_MOUSE_LL = 14;
    private const int WM_KEYDOWN = 0x0100;
    private const int WM_LBUTTONDOWN = 0x0201;
    private const int WM_LBUTTONUP = 0x0202;
    private const int WM_RBUTTONDOWN = 0x0204;
    private const int WM_RBUTTONUP = 0x0205;
    private const int VK_OEM_3 = 0xC0; // Backtick key
    private const uint LLMHF_INJECTED = 0x00000001;
    
    [StructLayout(LayoutKind.Sequential)]
    private struct KBDLLHOOKSTRUCT
    {
        public uint VkCode;
        public uint ScanCode;
        public uint Flags;
        public uint Time;
        public IntPtr DwExtraInfo;
    }
    
    [StructLayout(LayoutKind.Sequential)]
    private struct MSLLHOOKSTRUCT
    {
        public int X;
        public int Y;
        public uint MouseData;
        public uint Flags;
        public uint Time;
        public IntPtr DwExtraInfo;
    }
    
    private static IntPtr _keyboardHook = IntPtr.Zero;
    private static IntPtr _mouseHook = IntPtr.Zero;
    
    // Keep delegates alive to prevent GC
    private static LowLevelProc? _keyboardProc;
    private static LowLevelProc? _mouseProc;
    
    // Per-module keybinds: moduleId -> VK code (0 = unbound)
    // Autoclicker keeps backtick (0xC0) by default; all others start unbound.
    public static Dictionary<string, int> ModuleKeys { get; } = new()
    {
        ["autoclicker"]   = 0xC0,
        ["rightclick"]    = 0,
        ["jitter"]        = 0,
        ["clickinchests"] = 0,
        ["breakblocks"]   = 0,
        ["aimassist"]     = 0,
        ["gtbhelper"]     = 0,
        ["nametags"]      = 0,
        ["closestplayer"] = 0,
        ["chestesp"]      = 0,
    };

    public static void SetModuleKey(string moduleId, int vk)
    {
        ModuleKeys[moduleId] = vk;
        OnStateChanged?.Invoke();
    }

    public static int GetModuleKey(string moduleId)
        => ModuleKeys.TryGetValue(moduleId, out int vk) ? vk : 0;

    // Key capture mode for rebinding (reserved for future use)
    public static bool IsCapturingKey { get; private set; } = false;
    public static event Action<int>? OnKeyCaptured;

    public static event Action? OnToggleRequested;
    public static event Action? OnStateChanged;

    public static void StartKeyCapture()
    {
        IsCapturingKey = true;
    }

    public static void StopKeyCapture()
    {
        IsCapturingKey = false;
    }

    private static void ToggleModule(string moduleId)
    {
        var c = Clicker.Instance;
        switch (moduleId)
        {
            case "autoclicker":   c.ToggleArmed(); break;
            case "rightclick":    c.RightClickEnabled = !c.RightClickEnabled; break;
            case "jitter":        c.JitterEnabled = !c.JitterEnabled; break;
            case "clickinchests": c.ClickInChests = !c.ClickInChests; break;
            case "breakblocks":   c.BreakBlocksEnabled = !c.BreakBlocksEnabled; break;
            case "aimassist":     c.AimAssistEnabled = !c.AimAssistEnabled; break;
            case "gtbhelper":     c.GtbHelperEnabled = !c.GtbHelperEnabled; break;
            case "nametags":      c.NametagsEnabled             = !c.NametagsEnabled;             break;
            case "closestplayer": c.ClosestPlayerInfoEnabled    = !c.ClosestPlayerInfoEnabled;    break;
            case "chestesp":      c.ChestEspEnabled             = !c.ChestEspEnabled;             break;
        }
    }
    
    public static void Install()
    {
        _keyboardProc = KeyboardProc;
        _mouseProc = MouseProc;
        
        using var curProcess = Process.GetCurrentProcess();
        using var curModule = curProcess.MainModule;
        IntPtr moduleHandle = GetModuleHandle(curModule?.ModuleName);
        
        _keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, _keyboardProc, moduleHandle, 0);
        _mouseHook = SetWindowsHookEx(WH_MOUSE_LL, _mouseProc, moduleHandle, 0);
    }
    
    public static void Uninstall()
    {
        if (_keyboardHook != IntPtr.Zero)
        {
            UnhookWindowsHookEx(_keyboardHook);
            _keyboardHook = IntPtr.Zero;
        }
        
        if (_mouseHook != IntPtr.Zero)
        {
            UnhookWindowsHookEx(_mouseHook);
            _mouseHook = IntPtr.Zero;
        }
    }
    
    private static IntPtr KeyboardProc(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode >= 0 && wParam == (IntPtr)WM_KEYDOWN)
        {
            var kb = Marshal.PtrToStructure<KBDLLHOOKSTRUCT>(lParam);
            
            // Key capture mode - capture the pressed key
            if (IsCapturingKey)
            {
                IsCapturingKey = false;
                Application.Current?.Dispatcher.BeginInvoke(() =>
                {
                    OnKeyCaptured?.Invoke((int)kb.VkCode);
                });
                return (IntPtr)1; // Block the key
            }
            
            // Per-module keybinds
            foreach (var kvp in ModuleKeys)
            {
                if (kvp.Value > 0 && kb.VkCode == (uint)kvp.Value)
                {
                    string id = kvp.Key;
                    Application.Current?.Dispatcher.BeginInvoke(() =>
                    {
                        ToggleModule(id);
                        OnToggleRequested?.Invoke();
                        OnStateChanged?.Invoke();
                    });
                    return (IntPtr)1; // Block the key
                }
            }
        }
        
        return CallNextHookEx(_keyboardHook, nCode, wParam, lParam);
    }
    
    private static IntPtr MouseProc(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode >= 0 && Clicker.Instance.IsArmed)
        {
            var ms = Marshal.PtrToStructure<MSLLHOOKSTRUCT>(lParam);
            
            // Ignore injected events (our own clicks)
            if ((ms.Flags & LLMHF_INJECTED) != 0)
            {
                return CallNextHookEx(_mouseHook, nCode, wParam, lParam);
            }
            
            int msg = wParam.ToInt32();
            
            if (msg == WM_LBUTTONDOWN)
            {
                Application.Current?.Dispatcher.BeginInvoke(() =>
                {
                    if (GameStateClient.Instance.IsConnected)
                    {
                        var state = GameStateClient.Instance.CurrentState;
                        // 1.21: keep intent based on LookingAtBlock; the actual pause is gated by BreakingBlock/intent in Clicker.
                        Clicker.Instance.IsMiningIntent = state.LookingAtBlock;
                    }
                    else
                    {
                        Clicker.Instance.IsMiningIntent = false;
                    }
                    Clicker.Instance.StartClicking(true);
                    OnStateChanged?.Invoke();
                });
            }
            else if (msg == WM_LBUTTONUP)
            {
                if (Clicker.Instance.IsClicking)
                {
                    Application.Current?.Dispatcher.BeginInvoke(() =>
                    {
                        Clicker.Instance.StopClicking();
                        OnStateChanged?.Invoke();
                    });
                }
            }
            else if (msg == WM_RBUTTONDOWN)
            {
                Application.Current?.Dispatcher.BeginInvoke(() =>
                {
                    // Check if right-click-only-block is enabled
                    if (Clicker.Instance.RightClickOnlyBlock)
                    {
                        // Only start clicking if player is holding a block
                        if (!GameStateClient.Instance.IsConnected || !GameStateClient.Instance.CurrentState.HoldingBlock)
                        {
                            // Don't start clicking - player isn't holding a block
                            return;
                        }
                    }
                    
                    Clicker.Instance.StartClicking(false);
                    OnStateChanged?.Invoke();
                });
            }
            else if (msg == WM_RBUTTONUP)
            {
                if (Clicker.Instance.IsClicking)
                {
                    Application.Current?.Dispatcher.BeginInvoke(() =>
                    {
                        Clicker.Instance.StopClicking();
                        OnStateChanged?.Invoke();
                    });
                }
            }
        }
        
        return CallNextHookEx(_mouseHook, nCode, wParam, lParam);
    }
}
