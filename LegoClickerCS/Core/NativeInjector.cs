using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace LegoClickerCS.Core;

public static class NativeInjector
{
    // --- P/Invoke Definitions ---

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, int dwProcessId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr VirtualAllocEx(IntPtr hProcess, IntPtr lpAddress, uint dwSize, uint flAllocationType, uint flProtect);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress, byte[] lpBuffer, uint nSize, out IntPtr lpNumberOfBytesWritten);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateRemoteThread(IntPtr hProcess, IntPtr lpThreadAttributes, uint dwStackSize, IntPtr lpStartAddress, IntPtr lpParameter, uint dwCreationFlags, IntPtr lpThreadId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr GetModuleHandle(string lpModuleName);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr hModule, string lpProcName);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr hObject);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

    // Access Rights
    private const uint PROCESS_CREATE_THREAD = 0x0002;
    private const uint PROCESS_QUERY_INFORMATION = 0x0400;
    private const uint PROCESS_VM_OPERATION = 0x0008;
    private const uint PROCESS_VM_WRITE = 0x0020;
    private const uint PROCESS_VM_READ = 0x0010;

    // Memory Allocation
    private const uint MEM_COMMIT = 0x1000;
    private const uint MEM_RESERVE = 0x2000;
    private const uint PAGE_READWRITE = 0x04;
    private const uint WAIT_OBJECT_0 = 0x00000000;
    private const uint WAIT_TIMEOUT = 0x00000102;
    private const uint INFINITE = 0xFFFFFFFF;

    private static void Log(string message)
    {
        try
        {
            File.AppendAllText(@"injector_debug.log", $"[{DateTime.Now:HH:mm:ss}] [NativeInjector] {message}{Environment.NewLine}");
        }
        catch { }
    }

    private static void ReportProgress(Action<int, string>? progress, int percent, string message)
    {
        progress?.Invoke(Math.Clamp(percent, 0, 100), message);
        Log(message);
    }

    public static bool Inject(int pid, string dllPath, Action<int, string>? progress = null)
    {
        ReportProgress(progress, 2, $"Starting injection into PID {pid}");

        if (!File.Exists(dllPath))
        {
            ReportProgress(progress, 100, $"DLL not found: {dllPath}");
            return false;
        }
        ReportProgress(progress, 8, "DLL found.");

        IntPtr hProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, false, pid);

        if (hProcess == IntPtr.Zero)
        {
            ReportProgress(progress, 100, $"Failed to open process {pid}. Error: {Marshal.GetLastWin32Error()}");
            return false;
        }
        ReportProgress(progress, 15, "Process handle opened.");

        try
        {
            // 1. Allocate memory for DLL path
            byte[] pathBytes = Encoding.ASCII.GetBytes(dllPath + "\0");
            IntPtr pRemotePath = VirtualAllocEx(hProcess, IntPtr.Zero, (uint)pathBytes.Length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

            if (pRemotePath == IntPtr.Zero)
            {
                ReportProgress(progress, 100, $"VirtualAllocEx failed. Error: {Marshal.GetLastWin32Error()}");
                return false;
            }
            ReportProgress(progress, 30, $"Allocated remote memory: {pRemotePath}");

            // 2. Write DLL path to memory
            if (!WriteProcessMemory(hProcess, pRemotePath, pathBytes, (uint)pathBytes.Length, out _))
            {
                ReportProgress(progress, 100, $"WriteProcessMemory failed. Error: {Marshal.GetLastWin32Error()}");
                return false;
            }
            ReportProgress(progress, 50, "Wrote DLL path to remote memory.");

            // 3. Get LoadLibraryA address
            IntPtr hKernel32 = GetModuleHandle("kernel32.dll");
            IntPtr pLoadLibrary = GetProcAddress(hKernel32, "LoadLibraryA");

            if (pLoadLibrary == IntPtr.Zero)
            {
                ReportProgress(progress, 100, "Failed to find LoadLibraryA.");
                return false;
            }
            ReportProgress(progress, 65, $"Found LoadLibraryA: {pLoadLibrary}");

            // 4. Create Remote Thread
            IntPtr hThread = CreateRemoteThread(hProcess, IntPtr.Zero, 0, pLoadLibrary, pRemotePath, 0, IntPtr.Zero);

            if (hThread == IntPtr.Zero)
            {
                ReportProgress(progress, 100, $"CreateRemoteThread failed. Error: {Marshal.GetLastWin32Error()}");
                return false;
            }
            ReportProgress(progress, 80, $"Remote thread created: {hThread}");

            uint wait = WaitForSingleObject(hThread, INFINITE);
            if (wait == WAIT_OBJECT_0)
            {
                ReportProgress(progress, 100, "Injection completed.");
            }
            else if (wait == WAIT_TIMEOUT)
            {
                ReportProgress(progress, 95, "Injection thread still running (timeout).");
            }
            else
            {
                ReportProgress(progress, 95, $"WaitForSingleObject result: 0x{wait:X}");
            }

            CloseHandle(hThread);
            return true;
        }
        finally
        {
            CloseHandle(hProcess);
        }
    }
}
