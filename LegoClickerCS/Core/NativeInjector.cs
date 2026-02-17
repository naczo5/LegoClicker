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

    private static void Log(string message)
    {
        try
        {
            File.AppendAllText(@"C:\Users\rafal\Desktop\legoclickerC\injector_debug.log", $"[{DateTime.Now:HH:mm:ss}] [NativeInjector] {message}{Environment.NewLine}");
        }
        catch { }
    }

    public static bool Inject(int pid, string dllPath)
    {
        Log($"Starting injection into PID {pid} with DLL: {dllPath}");

        if (!File.Exists(dllPath))
        {
            Log($"DLL not found: {dllPath}");
            return false;
        }

        IntPtr hProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, false, pid);

        if (hProcess == IntPtr.Zero)
        {
            Log($"Failed to open process {pid}. Error: {Marshal.GetLastWin32Error()}");
            return false;
        }

        try
        {
            // 1. Allocate memory for DLL path
            byte[] pathBytes = Encoding.ASCII.GetBytes(dllPath + "\0");
            IntPtr pRemotePath = VirtualAllocEx(hProcess, IntPtr.Zero, (uint)pathBytes.Length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

            if (pRemotePath == IntPtr.Zero)
            {
                Log($"VirtualAllocEx failed. Error: {Marshal.GetLastWin32Error()}");
                return false;
            }
            Log($"Allocated memory at: {pRemotePath}");

            // 2. Write DLL path to memory
            if (!WriteProcessMemory(hProcess, pRemotePath, pathBytes, (uint)pathBytes.Length, out _))
            {
                Log($"WriteProcessMemory failed. Error: {Marshal.GetLastWin32Error()}");
                return false;
            }
            Log("Wrote DLL path to memory.");

            // 3. Get LoadLibraryA address
            IntPtr hKernel32 = GetModuleHandle("kernel32.dll");
            IntPtr pLoadLibrary = GetProcAddress(hKernel32, "LoadLibraryA");

            if (pLoadLibrary == IntPtr.Zero)
            {
                Log("Failed to find LoadLibraryA.");
                return false;
            }
            Log($"Found LoadLibraryA at: {pLoadLibrary}");

            // 4. Create Remote Thread
            IntPtr hThread = CreateRemoteThread(hProcess, IntPtr.Zero, 0, pLoadLibrary, pRemotePath, 0, IntPtr.Zero);

            if (hThread == IntPtr.Zero)
            {
                Log($"CreateRemoteThread failed. Error: {Marshal.GetLastWin32Error()}");
                return false;
            }
            Log($"Remote thread created. Handle: {hThread}");

            // Wait for thread to finish (optional, but good for debugging)
            // WaitForSingleObject(hThread, 5000); 

            CloseHandle(hThread);
            return true;
        }
        finally
        {
            CloseHandle(hProcess);
        }
    }
}
