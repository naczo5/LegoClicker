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
using LegoClickerCS;

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
    private string _injectedVersion = "1.8.9";
    private int _injectionProgress;
    private bool _isInjectionInProgress;

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
                if (value)
                {
                    _isInjectionInProgress = false;
                    _injectionProgress = 100;
                    OnPropertyChanged(nameof(IsInjectionInProgress));
                    OnPropertyChanged(nameof(InjectionProgress));
                }
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

    public string InjectedVersion
    {
        get => _injectedVersion;
        private set
        {
            if (_injectedVersion != value)
            {
                _injectedVersion = value;
                OnPropertyChanged(nameof(InjectedVersion));
            }
        }
    }

    public int InjectionProgress
    {
        get => _injectionProgress;
        private set
        {
            int clamped = Math.Clamp(value, 0, 100);
            if (_injectionProgress != clamped)
            {
                _injectionProgress = clamped;
                OnPropertyChanged(nameof(InjectionProgress));
            }
        }
    }

    public bool IsInjectionInProgress
    {
        get => _isInjectionInProgress;
        private set
        {
            if (_isInjectionInProgress != value)
            {
                _isInjectionInProgress = value;
                OnPropertyChanged(nameof(IsInjectionInProgress));
            }
        }
    }

    private void SetInjectionStage(int progress, string stageText)
    {
        IsInjectionInProgress = true;
        InjectionProgress = progress;
        StatusMessage = $"{stageText} ({InjectionProgress}%)";
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
    public async Task<bool> InjectAsync(string version = "auto")
    {
        if (IsInjected || IsConnected)
        {
            StatusMessage = "Already connected/injected";
            IsInjectionInProgress = false;
            InjectionProgress = 100;
            return true;
        }

        var mcProcess = FindMinecraftProcess();
        string resolvedVersion = ResolveInjectionVersion(version, mcProcess);

        SetInjectionStage(5, "Checking existing bridge");

        // 1. Try to connect directly (assuming already injected)
        await ConnectAsync(
            maxAttempts: 8,
            onAttempt: (attempt, total) =>
            {
                int mapped = 5 + (attempt * 15 / total);
                SetInjectionStage(mapped, $"Checking existing bridge ({attempt}/{total})");
            },
            reportFailure: false);

        if (IsConnected)
        {
            IsInjected = true;
            InjectedVersion = resolvedVersion;
            IsInjectionInProgress = false;
            InjectionProgress = 100;
            return true;
        }

        // 2. Inject Native Bridge
        SetInjectionStage(20, "Injecting bridge");
        if (mcProcess == null)
        {
            StatusMessage = "ERROR: Minecraft/Lunar not running.";
            IsInjectionInProgress = false;
            return false;
        }
        
        string baseDir = AppDomain.CurrentDomain.BaseDirectory;
        string dllName = resolvedVersion == "1.21" ? "bridge_121.dll" : "bridge.dll";
        string dllPath = Path.Combine(baseDir, dllName);
        
        Log($"Attempting to inject: {dllPath} into PID {mcProcess.Id}");

        if (!File.Exists(dllPath))
        {
             StatusMessage = $"ERROR: {dllName} not found.";
             Log($"{dllName} not found at " + dllPath);
             IsInjectionInProgress = false;
             return false;
        }

        bool injected = await Task.Run(() =>
            NativeInjector.Inject(mcProcess.Id, dllPath, (pct, msg) =>
            {
                int mapped = 20 + (pct * 60 / 100);
                SetInjectionStage(mapped, msg);
            }));
        if (!injected)
        {
             StatusMessage = "ERROR: Injection failed. Check logs.";
             Log("NativeInjector.Inject returned false.");
             IsInjectionInProgress = false;
             return false;
        }
        
        Log("Injection successful (ostensibly). Waiting for bridge...");
        SetInjectionStage(85, "Bridge injected, waiting for connection");
        
        // 4. Connect
        Log("Attempting to connect to bridge...");
        await ConnectAsync(
            maxAttempts: 30,
            onAttempt: (attempt, total) =>
            {
                int mapped = 85 + (attempt * 14 / total);
                SetInjectionStage(mapped, $"Waiting for bridge startup ({attempt}/{total})");
            });
        
        if (IsConnected)
        {
            IsInjected = true;
            InjectedVersion = resolvedVersion;
            Log("Connected successfully!");
            IsInjectionInProgress = false;
            InjectionProgress = 100;
            return true;
        }
        else
        {
             StatusMessage = "ERROR: Connectivity failed after injection.";
             Log("Failed to connect to bridge TCP server.");
             IsInjectionInProgress = false;
             return false;
        }
    }

    private void Log(string message)
    {
        // Avoid file I/O on UI thread or frequent calls
        Debug.WriteLine($"[{DateTime.Now:HH:mm:ss}] [GameStateClient] {message}");
    }

    // === TCP Connection ===

    public async Task ConnectAsync(int maxAttempts = 20, Action<int, int>? onAttempt = null, bool reportFailure = true)
    {
        _cts?.Cancel();
        _cts = new CancellationTokenSource();
        var token = _cts.Token;

        // Retry connection with configurable attempt count (500ms delay between attempts)
        for (int attempt = 0; attempt < maxAttempts; attempt++)
        {
            if (token.IsCancellationRequested) return;
            onAttempt?.Invoke(attempt + 1, maxAttempts);

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
                if (attempt + 1 < maxAttempts)
                    await Task.Delay(500, token);
            }
        }

        if (!IsConnected)
        {
            if (reportFailure)
                StatusMessage = "ERROR: Could not connect to agent on port " + _port;
            return;
        }

        // Start config sender task
        _ = Task.Run(() => ConfigSenderLoop(token), token);
        _ = Task.Run(() => ReadLoop(token), token);
    }

    private async Task ReadLoop(CancellationToken token)
    {
        try
        {
            if (_client == null) return;
            using var stream = _client.GetStream();
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
                        CurrentState = state;
                        bool is121 = InjectedVersion.StartsWith("1.21", StringComparison.OrdinalIgnoreCase);
                        string actionBar = is121 ? state.ActionBar : "";
                        System.Windows.Application.Current?.Dispatcher.BeginInvoke(new Action(() =>
                        {
                            Clicker.Instance.UpdateGtbFromActionBar(actionBar);
                        }));
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
        IsInjectionInProgress = false;
        InjectionProgress = 0;
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

    private static string ResolveInjectionVersion(string requestedVersion, Process? process)
    {
        if (string.Equals(requestedVersion, "1.21", StringComparison.OrdinalIgnoreCase))
            return "1.21";
        if (string.Equals(requestedVersion, "1.8.9", StringComparison.OrdinalIgnoreCase))
            return "1.8.9";

        string title = process?.MainWindowTitle?.ToLowerInvariant() ?? string.Empty;
        if (title.Contains("1.21.11") || title.Contains("1.21")) return "1.21";
        if (title.Contains("1.8.9")) return "1.8.9";

        return "1.21";
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
                    breakBlocks = clicker.BreakBlocksEnabled,
                    jitter = clicker.JitterEnabled,
                    clickInChests = clicker.ClickInChests,
                    aimAssist = clicker.AimAssistEnabled,
                    aimAssistFov = clicker.AimAssistFov,
                    aimAssistRange = clicker.AimAssistRange,
                    aimAssistStrength = clicker.AimAssistStrength,
                    gtbHelper = clicker.GtbHelperEnabled,
                    gtbHint = clicker.GtbCurrentHint,
                    gtbCount = clicker.GtbMatchCount,
                    gtbPreview = clicker.GtbMatchesPreview,
                    nametags = clicker.NametagsEnabled,
                    closestPlayerInfo = clicker.ClosestPlayerInfoEnabled,
                    nametagShowHealth = clicker.NametagShowHealth,
                    nametagShowArmor = clicker.NametagShowArmor,
                    nametagShowHeldItem = clicker.NametagShowHeldItem,
                    nametagMaxCount = clicker.NametagMaxCount,
                    chestEsp = clicker.ChestEspEnabled,
                    chestEspMaxCount = clicker.ChestEspMaxCount,
                    // Per-module keybinds
                    keybindAutoclicker   = InputHooks.GetModuleKey("autoclicker"),
                    keybindRightClick    = InputHooks.GetModuleKey("rightclick"),
                    keybindJitter        = InputHooks.GetModuleKey("jitter"),
                    keybindClickInChests = InputHooks.GetModuleKey("clickinchests"),
                    keybindBreakBlocks   = InputHooks.GetModuleKey("breakblocks"),
                    keybindAimAssist     = InputHooks.GetModuleKey("aimassist"),
                    keybindGtbHelper     = InputHooks.GetModuleKey("gtbhelper"),
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
                case "toggleExternalGui":
                    if (InjectedVersion.StartsWith("1.21", StringComparison.OrdinalIgnoreCase))
                    {
                        var mw = System.Windows.Application.Current?.MainWindow as MainWindow;
                        if (mw != null)
                            mw.Dispatcher.Invoke(mw.ShowControlCenterFromBridge);
                    }
                    break;
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
                case "toggleAimAssist":
                    clicker.AimAssistEnabled = !clicker.AimAssistEnabled;
                    break;
                case "toggleGtbHelper":
                    clicker.GtbHelperEnabled = !clicker.GtbHelperEnabled;
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
                case "toggleNametagHeldItem":
                    clicker.NametagShowHeldItem = !clicker.NametagShowHeldItem;
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
                case "toggleBreakBlocks":
                    clicker.BreakBlocksEnabled = !clicker.BreakBlocksEnabled;
                    break;
                case "setAimAssistFov":
                    clicker.AimAssistFov = node?["value"]?.GetValue<float>() ?? 30;
                    break;
                case "setAimAssistRange":
                    clicker.AimAssistRange = node?["value"]?.GetValue<float>() ?? 4.5f;
                    break;
                case "setAimAssistStrength":
                    clicker.AimAssistStrength = node?["value"]?.GetValue<int>() ?? 40;
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
