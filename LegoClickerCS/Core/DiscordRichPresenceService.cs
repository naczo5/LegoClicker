using System;
using System.ComponentModel;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;

namespace LegoClickerCS.Core;

public sealed class DiscordRichPresenceService
{
    private const string RepoUrl = "https://github.com/naczo5/LegoClicker";
    public const string DefaultApplicationId = "1488996793374212272";

    private static readonly string[] PipeNames =
    {
        "discord-ipc-0",
        "discord-ipc-1",
        "discord-ipc-2",
        "discord-ipc-3",
        "discord-ipc-4",
        "discord-ipc-5",
        "discord-ipc-6",
        "discord-ipc-7",
        "discord-ipc-8",
        "discord-ipc-9"
    };

    private static DiscordRichPresenceService? _instance;
    public static DiscordRichPresenceService Instance => _instance ??= new DiscordRichPresenceService();

    private readonly object _gate = new();
    private DiscordIpcClient? _client;
    private CancellationTokenSource? _cts;
    private Task? _loopTask;
    private int _refreshRequested;
    private bool _isRunning;
    private string _lastActivitySignature = string.Empty;
    private DateTimeOffset _lastPublishAt = DateTimeOffset.MinValue;
    private string _connectedApplicationId = string.Empty;

    private DiscordRichPresenceService() { }

    public void Start()
    {
        lock (_gate)
        {
            if (_isRunning)
                return;

            _isRunning = true;
            Clicker.Instance.StateChanged += OnClickerStateChanged;
            GameStateClient.Instance.PropertyChanged += OnGameStateClientPropertyChanged;
            GameStateClient.Instance.StateUpdated += OnGameStateUpdated;
            _cts = new CancellationTokenSource();
            _loopTask = Task.Run(() => RunAsync(_cts.Token));
        }

        SetStatus("Discord RPC ready");
        RequestRefresh();
    }

    public void Stop()
    {
        CancellationTokenSource? cts;
        Task? loopTask;

        lock (_gate)
        {
            if (!_isRunning)
                return;

            _isRunning = false;
            Clicker.Instance.StateChanged -= OnClickerStateChanged;
            GameStateClient.Instance.PropertyChanged -= OnGameStateClientPropertyChanged;
            GameStateClient.Instance.StateUpdated -= OnGameStateUpdated;
            cts = _cts;
            _cts = null;
            loopTask = _loopTask;
            _loopTask = null;
        }

        try
        {
            cts?.Cancel();
        }
        catch
        {
        }

        _client?.Disconnect();
        _client = null;
        _lastActivitySignature = string.Empty;
        _lastPublishAt = DateTimeOffset.MinValue;
        _connectedApplicationId = string.Empty;
        SetStatus("Discord RPC stopped");

        try
        {
            loopTask?.Wait(1000);
        }
        catch
        {
        }
    }

    private void OnClickerStateChanged()
        => RequestRefresh();

    private void OnGameStateUpdated()
        => RequestRefresh();

    private void OnGameStateClientPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(GameStateClient.IsConnected) or
            nameof(GameStateClient.IsInjected) or
            nameof(GameStateClient.InjectedVersion) or
            nameof(GameStateClient.StatusMessage) or
            nameof(GameStateClient.IsInjectionInProgress) or
            nameof(GameStateClient.InjectionProgress) or
            nameof(GameStateClient.Capabilities))
        {
            RequestRefresh();
        }
    }

    private void RequestRefresh()
    {
        Interlocked.Exchange(ref _refreshRequested, 1);
    }

    private async Task RunAsync(CancellationToken token)
    {
        try
        {
            while (!token.IsCancellationRequested)
            {
                var clicker = Clicker.Instance;
                var gameStateClient = GameStateClient.Instance;

                if (!IsConfigured(clicker))
                {
                    DisconnectClient();
                    SetStatus("Discord RPC disabled");
                    await WaitForRefreshAsync(5000, token).ConfigureAwait(false);
                    continue;
                }

                if (_client == null || !_client.IsConnected || !string.Equals(_connectedApplicationId, DefaultApplicationId, StringComparison.Ordinal))
                {
                    DisconnectClient();
                    SetStatus("Connecting to Discord...");
                    if (!await TryConnectAsync(DefaultApplicationId, token).ConfigureAwait(false))
                    {
                        DisconnectClient();
                        SetStatus("Discord unavailable");
                        await WaitForRefreshAsync(10000, token).ConfigureAwait(false);
                        continue;
                    }
                }

                PresenceSnapshot snapshot = BuildPresence(clicker, gameStateClient);
                TimeSpan sinceLastPublish = DateTimeOffset.UtcNow - _lastPublishAt;
                bool signatureChanged = !string.Equals(snapshot.Signature, _lastActivitySignature, StringComparison.Ordinal);
                bool shouldPublish =
                    (signatureChanged && sinceLastPublish >= TimeSpan.FromSeconds(5)) ||
                    sinceLastPublish >= TimeSpan.FromSeconds(45);

                if (shouldPublish)
                {
                    if (!await _client!.TrySetActivityAsync(snapshot, token).ConfigureAwait(false))
                    {
                        DisconnectClient();
                        SetStatus("Discord disconnected");
                        await WaitForRefreshAsync(2000, token).ConfigureAwait(false);
                        continue;
                    }

                    _lastActivitySignature = snapshot.Signature;
                    _lastPublishAt = DateTimeOffset.UtcNow;
                    SetStatus(snapshot.StatusText);
                }

                await WaitForRefreshAsync(5000, token).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception ex)
        {
            SetStatus($"Discord RPC error: {ex.Message}");
        }
        finally
        {
            DisconnectClient();
        }
    }

    private static bool IsConfigured(Clicker clicker)
        => clicker.DiscordRpcEnabled;

    private async Task<bool> TryConnectAsync(string clientId, CancellationToken token)
    {
        DisconnectClient();

        foreach (string pipeName in PipeNames)
        {
            var client = new DiscordIpcClient(pipeName);
            if (!await client.TryConnectAsync(clientId, token).ConfigureAwait(false))
            {
                client.Disconnect();
                continue;
            }

            _client = client;
            _connectedApplicationId = clientId;
            SetStatus("Discord connected");
            return true;
        }

        return false;
    }

    private void DisconnectClient()
    {
        try
        {
            _client?.Disconnect();
        }
        catch
        {
        }
        finally
        {
            _client = null;
            _connectedApplicationId = string.Empty;
        }
    }

    private async Task WaitForRefreshAsync(int durationMs, CancellationToken token)
    {
        const int sliceMs = 250;
        int elapsed = 0;

        while (elapsed < durationMs && !token.IsCancellationRequested)
        {
            if (Interlocked.Exchange(ref _refreshRequested, 0) == 1)
                return;

            int delay = Math.Min(sliceMs, durationMs - elapsed);
            await Task.Delay(delay, token).ConfigureAwait(false);
            elapsed += delay;
        }
    }

    private static PresenceSnapshot BuildPresence(Clicker clicker, GameStateClient gameStateClient)
    {
        bool connected = gameStateClient.IsConnected;
        bool injected = gameStateClient.IsInjected;
        string version = NormalizeVersionLabel(gameStateClient.InjectedVersion);
        string details;
        string stateLine;

        if (!connected && !injected)
        {
            details = $"LegoClicker • {version}";
            stateLine = "Waiting for Lunar Client";
        }
        else if (!connected)
        {
            details = $"LegoClicker • {version}";
            stateLine = "Injected • waiting for bridge link";
        }
        else
        {
            int enabledModules = CountEnabledModules(clicker);
            details = $"LegoClicker • {version} • {enabledModules} modules on";
            stateLine = BuildGameStateLine(clicker, gameStateClient.CurrentState);
        }

        details = Truncate(details, 128);
        string mode = BuildModeLabel(clicker);
        string state = Truncate($"{mode} • {stateLine}", 128);

        string signature = string.Join("|",
            connected ? "1" : "0",
            injected ? "1" : "0",
            version,
            clicker.StatusText,
            CountEnabledModules(clicker).ToString(),
            gameStateClient.CurrentState.GuiOpen ? "1" : "0",
            gameStateClient.CurrentState.ScreenName,
            gameStateClient.CurrentState.Health.ToString("0.0"),
            gameStateClient.CurrentState.ActionBar);

        return new PresenceSnapshot(details, state, signature);
    }

    private static string BuildModeLabel(Clicker clicker)
    {
        if (clicker.IsClicking)
            return "Autoclicking";
        if (clicker.IsArmed)
            return "Armed";
        return "Idle";
    }

    private static string BuildGameStateLine(Clicker clicker, GameState state)
    {
        if (state.GuiOpen)
        {
            string menu = MapScreenName(state.ScreenName);
            return $"Menu: {menu}";
        }

        if (state.Health >= 0)
            return $"In game • HP {state.Health:0.#}";

        if (!string.IsNullOrWhiteSpace(state.ActionBar) && clicker.GtbHelperEnabled)
            return Truncate($"In game • {state.ActionBar.Trim()}", 80);

        return "In game";
    }

    private static string MapScreenName(string? rawScreenName)
    {
        if (string.IsNullOrWhiteSpace(rawScreenName))
            return "Open";

        string v = rawScreenName.Trim().ToLowerInvariant();
        if (v.Contains("ingamemenu") || v.Contains("pause"))
            return "Pause";
        if (v.Contains("chat"))
            return "Chat";
        if (v.Contains("inventory"))
            return "Inventory";
        if (v.Contains("container"))
            return "Container";
        if (v.Contains("chest"))
            return "Chest";
        if (v.Contains("anvil"))
            return "Anvil";
        if (v.Contains("craft"))
            return "Crafting";
        if (v.Contains("clickgui"))
            return "Click GUI";

        return "Open";
    }

    private static int CountEnabledModules(Clicker clicker)
    {
        int count = 0;

        if (clicker.LeftClickEnabled) count++;
        if (clicker.RightClickEnabled) count++;
        if (clicker.JitterEnabled) count++;
        if (clicker.ClickInChests) count++;
        if (clicker.BreakBlocksEnabled) count++;
        if (clicker.AimAssistEnabled) count++;
        if (clicker.TriggerbotEnabled) count++;
        if (clicker.GtbHelperEnabled) count++;
        if (clicker.NametagsEnabled) count++;
        if (clicker.ClosestPlayerInfoEnabled) count++;
        if (clicker.ChestEspEnabled) count++;
        if (clicker.ReachEnabled) count++;
        if (clicker.VelocityEnabled) count++;

        return count;
    }

    private static string NormalizeVersionLabel(string? version)
    {
        if (string.IsNullOrWhiteSpace(version))
            return "Unknown";

        string trimmed = version.Trim();
        if (trimmed.StartsWith("1.21", StringComparison.OrdinalIgnoreCase))
            return "1.21";
        if (trimmed.StartsWith("26.1", StringComparison.OrdinalIgnoreCase))
            return "26.1";
        if (trimmed.StartsWith("1.8", StringComparison.OrdinalIgnoreCase))
            return "1.8.9";

        return trimmed;
    }

    private static string Truncate(string text, int maxLength)
        => text.Length <= maxLength ? text : text[..maxLength];

    private void SetStatus(string status)
        => DispatcherBeginInvoke(() => Clicker.Instance.DiscordRpcStatusText = status);

    private static void DispatcherBeginInvoke(Action action)
    {
        var dispatcher = Application.Current?.Dispatcher;
        if (dispatcher == null || dispatcher.CheckAccess())
        {
            action();
            return;
        }

        dispatcher.BeginInvoke(action);
    }

    private sealed record PresenceSnapshot(string Details, string State, string Signature)
    {
        public string StatusText => "Discord RPC active";
    }

    private sealed class DiscordIpcClient
    {
        private const int MaxPayloadBytes = 1024 * 1024;
        private readonly string _pipeName;
        private NamedPipeClientStream? _pipe;
        private Stream? _stream;

        public DiscordIpcClient(string pipeName)
        {
            _pipeName = pipeName;
        }

        public bool IsConnected => _pipe?.IsConnected == true;

        public async Task<bool> TryConnectAsync(string clientId, CancellationToken token)
        {
            try
            {
                _pipe = new NamedPipeClientStream(".", _pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
                await _pipe.ConnectAsync(1000, token).ConfigureAwait(false);
                _stream = _pipe;

                await SendFrameAsync(0, new
                {
                    v = 1,
                    client_id = clientId
                }, token).ConfigureAwait(false);

                await ReadFrameAsync(token).ConfigureAwait(false);
                return true;
            }
            catch
            {
                Disconnect();
                return false;
            }
        }

        public async Task<bool> TrySetActivityAsync(PresenceSnapshot snapshot, CancellationToken token)
        {
            try
            {
                if (!IsConnected || _stream == null)
                    return false;

                await SendFrameAsync(1, new
                {
                    cmd = "SET_ACTIVITY",
                    args = new
                    {
                        pid = Environment.ProcessId,
                        activity = new
                        {
                            details = snapshot.Details,
                            state = snapshot.State,
                            type = 0,
                            buttons = new[]
                            {
                                new
                                {
                                    label = "Repository",
                                    url = RepoUrl
                                }
                            }
                        }
                    },
                    nonce = Guid.NewGuid().ToString("N")
                }, token).ConfigureAwait(false);

                return true;
            }
            catch
            {
                Disconnect();
                return false;
            }
        }

        public void Disconnect()
        {
            try
            {
                _stream?.Dispose();
            }
            catch
            {
            }

            try
            {
                _pipe?.Dispose();
            }
            catch
            {
            }

            _stream = null;
            _pipe = null;
        }

        private async Task SendFrameAsync(int opCode, object payload, CancellationToken token)
        {
            if (_stream == null)
                throw new IOException("Discord IPC stream is not connected.");

            byte[] json = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(payload));
            byte[] op = BitConverter.GetBytes(opCode);
            byte[] len = BitConverter.GetBytes(json.Length);

            await _stream.WriteAsync(op, token).ConfigureAwait(false);
            await _stream.WriteAsync(len, token).ConfigureAwait(false);
            await _stream.WriteAsync(json, token).ConfigureAwait(false);
            await _stream.FlushAsync(token).ConfigureAwait(false);
        }

        private async Task ReadFrameAsync(CancellationToken token)
        {
            if (_stream == null)
                throw new IOException("Discord IPC stream is not connected.");

            byte[] header = new byte[8];
            await _stream.ReadExactlyAsync(header, token).ConfigureAwait(false);

            int length = BitConverter.ToInt32(header, 4);
            if (length < 0 || length > MaxPayloadBytes)
                throw new IOException($"Invalid Discord IPC payload length: {length}.");

            byte[] payload = new byte[length];
            if (length > 0)
                await _stream.ReadExactlyAsync(payload, token).ConfigureAwait(false);
        }
    }
}
