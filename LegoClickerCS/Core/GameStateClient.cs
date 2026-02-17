using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;

namespace LegoClickerCS.Core;

/// <summary>
/// TCP client that connects to the injected Java agent running inside Minecraft.
/// Receives game state updates at ~20Hz and exposes them to the rest of the app.
/// </summary>
public class GameStateClient : INotifyPropertyChanged
{
    private static GameStateClient? _instance;
    public static GameStateClient Instance => _instance ??= new GameStateClient();

    private TcpClient? _client;
    private CancellationTokenSource? _cts;
    private readonly int _port = 25590;

    private GameState _currentState = new();
    private bool _isConnected;
    private bool _isInjected;
    private string _statusMessage = "Not injected";

    public event PropertyChangedEventHandler? PropertyChanged;
    public event Action? StateUpdated;

    private GameStateClient() { }

    // === Properties ===

    public GameState CurrentState
    {
        get => _currentState;
        private set
        {
            _currentState = value;
            OnPropertyChanged(nameof(CurrentState));
            StateUpdated?.Invoke();
        }
    }

    public bool IsConnected
    {
        get => _isConnected;
        private set
        {
            if (_isConnected != value)
            {
                _isConnected = value;
                OnPropertyChanged(nameof(IsConnected));
                OnPropertyChanged(nameof(StatusMessage));
            }
        }
    }

    public bool IsInjected
    {
        get => _isInjected;
        private set
        {
            if (_isInjected != value)
            {
                _isInjected = value;
                OnPropertyChanged(nameof(IsInjected));
                OnPropertyChanged(nameof(StatusMessage));
            }
        }
    }

    public string StatusMessage
    {
        get
        {
            if (IsConnected) return "Connected — receiving game state";
            if (IsInjected) return "Injected — waiting for connection...";
            return _statusMessage;
        }
        private set
        {
            _statusMessage = value;
            OnPropertyChanged(nameof(StatusMessage));
        }
    }

    // === Injection ===

    /// <summary>
    /// Injects the agent into the running Minecraft/Lunar Client process.
    /// </summary>
    /// <summary>
    /// Connects to the agent, which should be loaded at startup via -javaagent.
    /// Uses the same method name to keep compatibility with existing UI calls,
    /// but functionally it's now a "Connect" operation.
    /// </summary>
    public async Task<bool> InjectAsync()
    {
        if (IsInjected || IsConnected)
        {
            StatusMessage = "Already connected/injected";
            return true;
        }

        StatusMessage = "Connecting...";

        // 1. Try to connect directly (assuming already injected)
        await ConnectAsync();

        if (IsConnected)
        {
            IsInjected = true;
            return true;
        }

        // 2. Inject Native Bridge
        StatusMessage = "Injecting bridge...";
        var mcProcess = FindMinecraftProcess();
        if (mcProcess == null)
        {
            StatusMessage = "ERROR: Minecraft/Lunar not running.";
            return false;
        }
        
        string baseDir = AppDomain.CurrentDomain.BaseDirectory;
        string dllPath = Path.Combine(baseDir, "bridge.dll");
        
        Log($"Attempting to inject: {dllPath} into PID {mcProcess.Id}");

        if (!File.Exists(dllPath))
        {
             StatusMessage = "ERROR: bridge.dll not found.";
             Log("bridge.dll not found at " + dllPath);
             return false;
        }

        bool injected = NativeInjector.Inject(mcProcess.Id, dllPath);
        if (!injected)
        {
             StatusMessage = "ERROR: Injection failed. Check logs.";
             Log("NativeInjector.Inject returned false.");
             return false;
        }
        
        Log("Injection successful (ostensibly). Waiting for bridge...");
        
        // 4. Connect
        Log("Attempting to connect to bridge...");
        await ConnectAsync();
        
        if (IsConnected)
        {
            IsInjected = true;
            Log("Connected successfully!");
            return true;
        }
        else
        {
             StatusMessage = "ERROR: Connectivity failed after injection.";
             Log("Failed to connect to bridge TCP server.");
             return false;
        }
    }

    private void Log(string message)
    {
        // Avoid file I/O on UI thread or frequent calls
        Debug.WriteLine($"[{DateTime.Now:HH:mm:ss}] [GameStateClient] {message}");
    }

    // === TCP Connection ===

    public async Task ConnectAsync()
    {
        _cts?.Cancel();
        _cts = new CancellationTokenSource();
        var token = _cts.Token;

        // Retry connection for up to 10 seconds
        for (int attempt = 0; attempt < 20; attempt++)
        {
            if (token.IsCancellationRequested) return;

            try
            {
                _client = new TcpClient();
                await _client.ConnectAsync("127.0.0.1", _port, token);
                IsConnected = true;
                StatusMessage = "Connected!";
                break;
            }
            catch
            {
                _client?.Dispose();
                _client = null;
                await Task.Delay(500, token);
            }
        }

        if (!IsConnected)
        {
            StatusMessage = "ERROR: Could not connect to agent on port " + _port;
            return;
        }

        // Start config sender task
        _ = Task.Run(() => ConfigSenderLoop(token), token);

        // Read loop
        try
        {
            using var stream = _client!.GetStream();
            using var reader = new StreamReader(stream, Encoding.UTF8);

            while (!token.IsCancellationRequested && _client.Connected)
            {
                string? line = await reader.ReadLineAsync(token);
                if (line == null) break;

                // Debug log occasionally
                if (line.Contains("\"entities\":[{\"name\"")) 
                {
                    Log("Received Entity Data: " + line);
                }

                try
                {
                    // Check if it's a command from ClickGUI
                    if (line.Contains("\"type\":\"cmd\""))
                    {
                        HandleBridgeCommand(line);
                        continue;
                    }

                    var state = JsonSerializer.Deserialize<GameState>(line);
                    if (state != null)
                    {
                        state.IsConnected = true;
                        state.LastUpdate = DateTime.Now;

                        // Distance is now calculated by bridge and sent in JSON

                        CurrentState = state;
                    }
                }
                catch (JsonException)
                {
                    // Skip malformed lines
                }
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception ex)
        {
            Debug.WriteLine($"[GameStateClient] Read error: {ex.Message}");
        }
        finally
        {
            IsConnected = false;
            _client?.Dispose();
            _client = null;
            StatusMessage = "Disconnected from agent.";
        }
    }


    // P/Invoke for Detach
    [System.Runtime.InteropServices.DllImport("bridge.dll", EntryPoint = "Detach", CallingConvention = System.Runtime.InteropServices.CallingConvention.Cdecl)]
    private static extern void DetachBridge();

    public async Task DetachAsync()
    {
        if (!_isInjected) return;
        
        await Task.Run(() =>
        {
             try
             {
                 // Close socket first
                 if (_client != null)
                 {
                     _client.Close();
                     _client = null;
                 }
                 IsConnected = false;
                 
                 // Call native detach
                 try { DetachBridge(); } catch { } 
                 
                 IsInjected = false;
                 StatusMessage = "Detached";
             }
             catch
             {
                 StatusMessage = "Error detaching";
             }
        });
    }

    public void Disconnect()
    {
        _cts?.Cancel();
        _client?.Dispose();
        _client = null;
        IsConnected = false;
        IsInjected = false;
        StatusMessage = "Not injected";
    }

    // === Helpers ===

    /// <summary>
    /// Finds the Minecraft/Lunar Client Java process via OS process list.
    /// This is more reliable than VirtualMachine.list() which requires same-JDK compatibility.
    /// </summary>
    private Process? FindMinecraftProcess()
    {
        string[] keywords = { ".lunarclient", "lunar", "minecraft" };

        try
        {
            var javaProcesses = Process.GetProcesses()
                .Where(p => p.ProcessName.Equals("javaw", StringComparison.OrdinalIgnoreCase)
                         || p.ProcessName.Equals("java", StringComparison.OrdinalIgnoreCase));

            foreach (var proc in javaProcesses)
            {
                try
                {
                    string? path = proc.MainModule?.FileName?.ToLower();
                    if (path != null)
                    {
                        foreach (string kw in keywords)
                        {
                            if (path.Contains(kw))
                            {
                                Debug.WriteLine($"[FindMinecraftProcess] Found: PID={proc.Id} Path={proc.MainModule?.FileName}");
                                return proc;
                            }
                        }
                    }
                }
                catch
                {
                    // Can't access MainModule for some processes (access denied)
                }
            }

            // Fallback: any javaw that isn't us
            foreach (var proc in javaProcesses)
            {
                try
                {
                    if (proc.Id != Environment.ProcessId)
                    {
                        Debug.WriteLine($"[FindMinecraftProcess] Fallback: PID={proc.Id} Name={proc.ProcessName}");
                        return proc;
                    }
                }
                catch { }
            }
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"[FindMinecraftProcess] Error: {ex.Message}");
        }

        return null;
    }


    private string? FindJava()
    {
        // Check known JDK locations
        string[] jdkPaths = {
            @"C:\Program Files\Java\jdk-21.0.10\bin\java.exe",
            @"C:\Program Files\Java\jdk-17\bin\java.exe",
            @"C:\Program Files\Common Files\Oracle\Java\javapath\java.exe",
        };

        foreach (string path in jdkPaths)
        {
            if (File.Exists(path)) return path;
        }

        // Try PATH
        try
        {
            var psi = new ProcessStartInfo("java", "-version")
            {
                UseShellExecute = false,
                RedirectStandardError = true,
                CreateNoWindow = true
            };
            var p = Process.Start(psi);
            p?.WaitForExit(3000);
            if (p is { ExitCode: 0 }) return "java";
        }
        catch { }

        return null;
    }

    /// <summary>
    /// Quick check: is a GUI currently open in-game?
    /// </summary>
    public bool IsGuiOpen => IsConnected && CurrentState.GuiOpen;

    /// <summary>
    /// Quick check: current player health.
    /// </summary>
    public float PlayerHealth => IsConnected ? CurrentState.Health : -1;

    // === Config Sending (C# -> Bridge for HUD display) ===

    private async Task ConfigSenderLoop(CancellationToken token)
    {
        while (!token.IsCancellationRequested && _client?.Connected == true)
        {
            try
            {
                var clicker = Clicker.Instance;
                var config = new
                {
                    type = "config",
                    armed = clicker.IsArmed,
                    clicking = clicker.IsClicking,
                    minCPS = clicker.MinCPS,
                    maxCPS = clicker.MaxCPS,
                    left = clicker.LeftClickEnabled,
                    right = clicker.RightClickEnabled,
                    rightMinCPS = clicker.RightMinCPS,
                    rightMaxCPS = clicker.RightMaxCPS,
                    rightBlock = clicker.RightClickOnlyBlock,
                    jitter = clicker.JitterEnabled,
                    clickInChests = clicker.ClickInChests,
                    nametags = clicker.NametagsEnabled,
                    closestPlayerInfo = clicker.ClosestPlayerInfoEnabled,
                    nametagShowHealth = clicker.NametagShowHealth,
                    nametagShowArmor = clicker.NametagShowArmor,
                    chestEsp = clicker.ChestEspEnabled,
                    // Per-module keybinds
                    keybindAutoclicker   = InputHooks.GetModuleKey("autoclicker"),
                    keybindNametags      = InputHooks.GetModuleKey("nametags"),
                    keybindClosestPlayer = InputHooks.GetModuleKey("closestplayer"),
                    keybindChestEsp      = InputHooks.GetModuleKey("chestesp")
                };

                string json = JsonSerializer.Serialize(config) + "\n";
                byte[] data = Encoding.UTF8.GetBytes(json);

                if (_client?.Connected == true)
                {
                    var stream = _client.GetStream();
                    await stream.WriteAsync(data, 0, data.Length, token);
                }
            }
            catch (Exception)
            {
                break;
            }

            await Task.Delay(200, token);
        }
    }

    // === ClickGUI Command Handler ===

    private void HandleBridgeCommand(string json)
    {
        try
        {
            var node = JsonNode.Parse(json);
            string? action = node?["action"]?.GetValue<string>();
            if (action == null) return;

            var clicker = Clicker.Instance;
            switch (action)
            {
                case "toggleArmed":
                    clicker.ToggleArmed();
                    break;
                case "toggleLeft":
                    clicker.LeftClickEnabled = !clicker.LeftClickEnabled;
                    break;
                case "toggleRight":
                    clicker.RightClickEnabled = !clicker.RightClickEnabled;
                    break;
                case "toggleJitter":
                    clicker.JitterEnabled = !clicker.JitterEnabled;
                    break;

                case "toggleClickInChests":
                    clicker.ClickInChests = !clicker.ClickInChests;
                    break;
                case "toggleNametags":
                    clicker.NametagsEnabled = !clicker.NametagsEnabled;
                    break;
                case "toggleClosestPlayerInfo":
                    clicker.ClosestPlayerInfoEnabled = !clicker.ClosestPlayerInfoEnabled;
                    break;
                case "toggleNametagHealth":
                    clicker.NametagShowHealth = !clicker.NametagShowHealth;
                    break;
                case "toggleNametagArmor":
                    clicker.NametagShowArmor = !clicker.NametagShowArmor;
                    break;
                case "toggleChestEsp":
                    clicker.ChestEspEnabled = !clicker.ChestEspEnabled;
                    break;
                case "setKeybind":
                    string? moduleId = node?["module"]?.GetValue<string>();
                    int vkCode = node?["key"]?.GetValue<int>() ?? 0;
                    if (moduleId != null)
                        InputHooks.SetModuleKey(moduleId, vkCode);
                    break;
                case "setMinCPS":
                    float minVal = node?["value"]?.GetValue<float>() ?? 8;
                    clicker.MinCPS = minVal;
                    break;
                case "setMaxCPS":
                    float maxVal = node?["value"]?.GetValue<float>() ?? 12;
                    clicker.MaxCPS = maxVal;
                    break;
                case "setRightMinCPS":
                    float rMinVal = node?["value"]?.GetValue<float>() ?? 10;
                    clicker.RightMinCPS = rMinVal;
                    break;
                case "setRightMaxCPS":
                    float rMaxVal = node?["value"]?.GetValue<float>() ?? 14;
                    clicker.RightMaxCPS = rMaxVal;
                    break;
                case "toggleRightBlockOnly":
                    clicker.RightClickOnlyBlock = !clicker.RightClickOnlyBlock;
                    break;
            }

            Log($"Bridge command: {action}");
        }
        catch (Exception ex)
        {
            Log($"Error handling bridge command: {ex.Message}");
        }
    }

    private void OnPropertyChanged(string name)
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    }
}
