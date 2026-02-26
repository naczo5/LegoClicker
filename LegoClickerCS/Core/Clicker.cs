using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Diagnostics;
using System.Linq;

namespace LegoClickerCS.Core;

public class Clicker : INotifyPropertyChanged
{
    private static Clicker? _instance;
    public static Clicker Instance => _instance ??= new Clicker();
    
    // P/Invoke declarations
    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);
    [DllImport("user32.dll")]
    private static extern short GetAsyncKeyState(int vKey);
    
    [StructLayout(LayoutKind.Sequential)]
    private struct INPUT
    {
        public uint Type;
        public MOUSEINPUT Mi;
    }
    
    [StructLayout(LayoutKind.Sequential)]
    private struct MOUSEINPUT
    {
        public int Dx;
        public int Dy;
        public uint MouseData;
        public uint DwFlags;
        public uint Time;
        public IntPtr DwExtraInfo;
    }
    
    private const uint INPUT_MOUSE = 0;
    private const uint MOUSEEVENTF_MOVE = 0x0001;
    private const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    private const uint MOUSEEVENTF_LEFTUP = 0x0004;
    private const uint MOUSEEVENTF_RIGHTDOWN = 0x0008;
    private const uint MOUSEEVENTF_RIGHTUP = 0x0010;
    private const int VK_LBUTTON = 0x01;
    
    // State
    private bool _isArmed;
    private bool _isClicking;
    private bool _useLeftButton = true;
    private CancellationTokenSource? _clickCts;
    private CancellationTokenSource? _aimAssistCts;
    
    // Settings
    private float _minCPS = 8.0f;
    private float _maxCPS = 12.0f;
    private bool _leftClickEnabled = true;
    private bool _rightClickEnabled = false;
    private bool _jitterEnabled = true;

    
    // Right Click Settings
    private float _rightMinCPS = 10.0f;
    private float _rightMaxCPS = 14.0f;
    private bool _rightClickOnlyBlock = false;
    private bool _breakBlocksEnabled = false;
    private bool _isMiningIntent = false;
    
    private readonly Random _random = new();
    
    public event PropertyChangedEventHandler? PropertyChanged;
    public event Action? StateChanged;
    
    private Clicker() { }
    
    public bool IsArmed
    {
        get => _isArmed;
        private set
        {
            if (_isArmed != value)
            {
                _isArmed = value;
                OnPropertyChanged(nameof(IsArmed));
                OnPropertyChanged(nameof(StatusText));
                StateChanged?.Invoke();
            }
        }
    }
    
    public bool IsClicking
    {
        get => _isClicking;
        private set
        {
            if (_isClicking != value)
            {
                _isClicking = value;
                OnPropertyChanged(nameof(IsClicking));
                OnPropertyChanged(nameof(StatusText));
                StateChanged?.Invoke();
            }
        }
    }
    
    public float MinCPS
    {
        get => _minCPS;
        set
        {
            if (value > 0 && value <= 25)
            {
                _minCPS = value;
                OnPropertyChanged(nameof(MinCPS));
                StateChanged?.Invoke();
            }
        }
    }
    
    public float MaxCPS
    {
        get => _maxCPS;
        set
        {
            if (value > 0 && value <= 25)
            {
                _maxCPS = value;
                OnPropertyChanged(nameof(MaxCPS));
                StateChanged?.Invoke();
            }
        }
    }
    
    public bool LeftClickEnabled
    {
        get => _leftClickEnabled;
        set
        {
            _leftClickEnabled = value;
            OnPropertyChanged(nameof(LeftClickEnabled));
            StateChanged?.Invoke();
        }
    }
    
    public bool RightClickEnabled
    {
        get => _rightClickEnabled;
        set
        {
            _rightClickEnabled = value;
            OnPropertyChanged(nameof(RightClickEnabled));
            StateChanged?.Invoke();
        }
    }
    
    public bool JitterEnabled
    {
        get => _jitterEnabled;
        set
        {
            _jitterEnabled = value;
            OnPropertyChanged(nameof(JitterEnabled));
            StateChanged?.Invoke();
        }
    }
    
    public float RightMinCPS
    {
        get => _rightMinCPS;
        set
        {
            if (value > 0 && value <= 25)
            {
                _rightMinCPS = value;
                OnPropertyChanged(nameof(RightMinCPS));
                StateChanged?.Invoke();
            }
        }
    }

    public float RightMaxCPS
    {
        get => _rightMaxCPS;
        set
        {
            if (value > 0 && value <= 25)
            {
                _rightMaxCPS = value;
                OnPropertyChanged(nameof(RightMaxCPS));
                StateChanged?.Invoke();
            }
        }
    }

    public bool RightClickOnlyBlock
    {
        get => _rightClickOnlyBlock;
        set
        {
            _rightClickOnlyBlock = value;
            OnPropertyChanged(nameof(RightClickOnlyBlock));
            StateChanged?.Invoke();
        }
    }

    public bool BreakBlocksEnabled
    {
        get => _breakBlocksEnabled;
        set
        {
            _breakBlocksEnabled = value;
            OnPropertyChanged(nameof(BreakBlocksEnabled));
            StateChanged?.Invoke();
        }
    }
    
    public bool IsMiningIntent
    {
        get => _isMiningIntent;
        set
        {
            _isMiningIntent = value;
            OnPropertyChanged(nameof(IsMiningIntent));
            StateChanged?.Invoke();
        }
    }
    
    public string StatusText
    {
        get
        {
            if (IsClicking) return "Clicking";
            if (IsArmed) return "Armed";
            return "Disabled";
        }
    }
    
    public void ToggleArmed()
    {
        if (IsArmed)
            Disarm();
        else
            Arm();
    }
    
    public void Arm()
    {
        IsArmed = true;
    }
    
    public void Disarm()
    {
        IsArmed = false;
        StopClicking();
    }
    
    public void StartClicking(bool leftButton)
    {
        if (!IsArmed) return;
        if (leftButton && !LeftClickEnabled) return;
        if (!leftButton && !RightClickEnabled) return;
        if (IsClicking) return;
        
        _useLeftButton = leftButton;
        IsClicking = true;
        
        _clickCts = new CancellationTokenSource();
        Task.Run(() => ClickLoop(_clickCts.Token));
    }
    
    public void StopClicking()
    {
        _clickCts?.Cancel();
        IsClicking = false;
    }
    
    public void Stop()
    {
        StopClicking();
        Disarm();
        StopAimAssistLoop();
    }

    private void StartAimAssistLoop()
    {
        if (_aimAssistCts != null) return;
        _aimAssistCts = new CancellationTokenSource();
        Task.Run(() => AimAssistLoop(_aimAssistCts.Token));
    }

    private void StopAimAssistLoop()
    {
        _aimAssistCts?.Cancel();
        _aimAssistCts = null;
    }
    
    private bool _clickInChests = false;
    private bool _aimAssistEnabled = false;
    private float _aimAssistFov = 30.0f;
    private float _aimAssistRange = 4.5f;
    private int _aimAssistStrength = 40;
    private bool _gtbHelperEnabled = false;
    private string _gtbCurrentHint = "-";
    private int _gtbMatchCount = 0;
    private string _gtbMatchesPreview = "-";

    public bool ClickInChests
    {
        get => _clickInChests;
        set
        {
            _clickInChests = value;
            OnPropertyChanged(nameof(ClickInChests));
            StateChanged?.Invoke();
        }
    }

    public bool AimAssistEnabled
    {
        get => _aimAssistEnabled;
        set
        {
            _aimAssistEnabled = value;
            OnPropertyChanged(nameof(AimAssistEnabled));
            if (_aimAssistEnabled) StartAimAssistLoop();
            else StopAimAssistLoop();
            StateChanged?.Invoke();
        }
    }

    public float AimAssistFov
    {
        get => _aimAssistFov;
        set
        {
            float clamped = Math.Clamp(value, 1.0f, 180.0f);
            if (Math.Abs(_aimAssistFov - clamped) > float.Epsilon)
            {
                _aimAssistFov = clamped;
                OnPropertyChanged(nameof(AimAssistFov));
                StateChanged?.Invoke();
            }
        }
    }

    public float AimAssistRange
    {
        get => _aimAssistRange;
        set
        {
            float clamped = Math.Clamp(value, 1.0f, 12.0f);
            if (Math.Abs(_aimAssistRange - clamped) > float.Epsilon)
            {
                _aimAssistRange = clamped;
                OnPropertyChanged(nameof(AimAssistRange));
                StateChanged?.Invoke();
            }
        }
    }

    public int AimAssistStrength
    {
        get => _aimAssistStrength;
        set
        {
            int clamped = Math.Clamp(value, 1, 100);
            if (_aimAssistStrength != clamped)
            {
                _aimAssistStrength = clamped;
                OnPropertyChanged(nameof(AimAssistStrength));
                StateChanged?.Invoke();
            }
        }
    }

    public bool GtbHelperEnabled
    {
        get => _gtbHelperEnabled;
        set
        {
            _gtbHelperEnabled = value;
            OnPropertyChanged(nameof(GtbHelperEnabled));
            if (!value)
            {
                SetGtbState("", 0, "");
            }
            StateChanged?.Invoke();
        }
    }

    public string GtbCurrentHint
    {
        get => _gtbCurrentHint;
        private set
        {
            if (_gtbCurrentHint != value)
            {
                _gtbCurrentHint = value;
                OnPropertyChanged(nameof(GtbCurrentHint));
            }
        }
    }

    public int GtbMatchCount
    {
        get => _gtbMatchCount;
        private set
        {
            if (_gtbMatchCount != value)
            {
                _gtbMatchCount = value;
                OnPropertyChanged(nameof(GtbMatchCount));
            }
        }
    }

    public string GtbMatchesPreview
    {
        get => _gtbMatchesPreview;
        private set
        {
            if (_gtbMatchesPreview != value)
            {
                _gtbMatchesPreview = value;
                OnPropertyChanged(nameof(GtbMatchesPreview));
            }
        }
    }

    private bool _nametagsEnabled = false;
    public bool NametagsEnabled
    {
        get => _nametagsEnabled;
        set
        {
            _nametagsEnabled = value;
            OnPropertyChanged(nameof(NametagsEnabled));
            StateChanged?.Invoke();
        }
    }

    private bool _nametagShowHealth = true;
    public bool NametagShowHealth
    {
        get => _nametagShowHealth;
        set
        {
            _nametagShowHealth = value;
            OnPropertyChanged(nameof(NametagShowHealth));
            StateChanged?.Invoke();
        }
    }

    private bool _nametagShowArmor = true;
    public bool NametagShowArmor
    {
        get => _nametagShowArmor;
        set
        {
            _nametagShowArmor = value;
            OnPropertyChanged(nameof(NametagShowArmor));
            StateChanged?.Invoke();
        }
    }

    private bool _nametagShowHeldItem = true;
    public bool NametagShowHeldItem
    {
        get => _nametagShowHeldItem;
        set
        {
            _nametagShowHeldItem = value;
            OnPropertyChanged(nameof(NametagShowHeldItem));
            StateChanged?.Invoke();
        }
    }

    private int _nametagMaxCount = 8;
    public int NametagMaxCount
    {
        get => _nametagMaxCount;
        set
        {
            int clamped = Math.Clamp(value, 1, 20);
            if (_nametagMaxCount != clamped)
            {
                _nametagMaxCount = clamped;
                OnPropertyChanged(nameof(NametagMaxCount));
                StateChanged?.Invoke();
            }
        }
    }

    private bool _chestEspEnabled = false;
    public bool ChestEspEnabled
    {
        get => _chestEspEnabled;
        set
        {
            _chestEspEnabled = value;
            OnPropertyChanged(nameof(ChestEspEnabled));
            StateChanged?.Invoke();
        }
    }

    private int _chestEspMaxCount = 5;
    public int ChestEspMaxCount
    {
        get => _chestEspMaxCount;
        set
        {
            int clamped = Math.Clamp(value, 1, 20);
            if (_chestEspMaxCount != clamped)
            {
                _chestEspMaxCount = clamped;
                OnPropertyChanged(nameof(ChestEspMaxCount));
                StateChanged?.Invoke();
            }
        }
    }

    private bool _closestPlayerInfoEnabled = false;
    public bool ClosestPlayerInfoEnabled
    {
        get => _closestPlayerInfoEnabled;
        set
        {
            _closestPlayerInfoEnabled = value;
            OnPropertyChanged(nameof(ClosestPlayerInfoEnabled));
            StateChanged?.Invoke();
        }
    }

    public void UpdateGtbFromActionBar(string actionBarText)
    {
        if (!GtbHelperEnabled)
        {
            SetGtbState("", 0, "");
            return;
        }

        GtbWordSolver.TryLearnSolvedWord(actionBarText);
        var (mask, matches) = GtbWordSolver.Solve(actionBarText, maxResults: 25);
        if (string.IsNullOrWhiteSpace(mask))
        {
            SetGtbState("", 0, "");
            return;
        }

        string preview = matches.Count == 0
            ? "No matches"
            : string.Join(", ", matches.Select(m => m));
        SetGtbState(mask, matches.Count, preview);
    }

    private void SetGtbState(string hint, int count, string preview)
    {
        GtbCurrentHint = string.IsNullOrWhiteSpace(hint) ? "-" : hint;
        GtbMatchCount = count;
        GtbMatchesPreview = string.IsNullOrWhiteSpace(preview) ? "-" : preview;
    }
    
    private async Task AimAssistLoop(CancellationToken token)
    {
        try
        {
            while (!token.IsCancellationRequested)
            {
                bool supportedVersion = GameStateClient.Instance.InjectedVersion.StartsWith("1.21", StringComparison.OrdinalIgnoreCase);
                bool shouldRun =
                    AimAssistEnabled &&
                    supportedVersion &&
                    GameStateClient.Instance.IsConnected &&
                    WindowDetection.IsMinecraftActive() &&
                    !WindowDetection.IsCursorVisible();

                if (shouldRun)
                {
                    bool leftHeld = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
                    bool autoLeftClicking = IsClicking && _useLeftButton;
                    if (leftHeld || autoLeftClicking)
                        TryApplyAimAssist();
                }

                await Task.Delay(8, token).ConfigureAwait(false);
            }
        }
        catch (TaskCanceledException) { }
    }


    private async Task ClickLoop(CancellationToken token)
    {
        var stopwatch = new Stopwatch();
        
        while (!token.IsCancellationRequested)
        {
            if (!WindowDetection.IsMinecraftActive())
            {
                await Task.Delay(100, token).ConfigureAwait(false);
                continue;
            }

            // Menu & Inventory Safety Checks
            if (GameStateClient.Instance.IsConnected)
            {
                var state = GameStateClient.Instance.CurrentState;
                if (state.GuiOpen)
                {
                    string screen = state.ScreenName;
                    
                    // Always block in these specific GUIs (1.8.9 + 1.21 names + Fabric mappings)
                    bool alwaysBlock = screen.Contains("GuiInventory")   || screen.Contains("InventoryScreen")  || screen.Contains("class_490") || // Inventory
                                       screen.Contains("GuiCrafting")    || screen.Contains("CraftingScreen")   || screen.Contains("class_479") || // Crafting
                                       screen.Contains("GuiFurnace")     || screen.Contains("FurnaceScreen")    || screen.Contains("class_3871") || // Furnace
                                       screen.Contains("AbstractFurnace") ||
                                       screen.Contains("GuiRepair")      || screen.Contains("AnvilScreen")      || screen.Contains("class_471");   // Anvil
                                       
                    // Conditionally block in Chests/Containers
                    bool isChest = screen.Contains("GuiChest")     || screen.Contains("ContainerScreen") || screen.Contains("class_481") || // Chest/Generic container
                                   screen.Contains("GuiContainer")  || screen.Contains("HopperScreen")    || screen.Contains("class_488") || // Hopper
                                   screen.Contains("ShulkerBox")    || screen.Contains("class_495")       ||
                                   // Generic fallback for any container screen in 1.21 if exact matches fail:
                                   screen.Contains("HandledScreen") || screen.Contains("class_465");

                    bool shouldBlock = false;

                    // Universal Ghost UI Check:
                    // If the bridge reports a GUI is open, but the system cursor is HIDDEN,
                    // it implies we are actually in-game (Crosshair mode) and the GUI state is 'ghosted'.
                    if (WindowDetection.IsCursorVisible())
                    {
                        if (alwaysBlock)
                        {
                            shouldBlock = true;
                        }
                        else if (isChest)
                        {
                            // Block in chests UNLESS "Click In Chests" is enabled
                            if (!ClickInChests) shouldBlock = true;
                        }
                        else
                        {
                            // Menu (Escape, Settings, Chat, etc.)
                            // Always block in menus if cursor is visible
                            shouldBlock = true;
                        }
                    }

                    if (shouldBlock)
                    {
                        await Task.Delay(100, token).ConfigureAwait(false);
                        continue;
                    }
                }
            }
            else
            {
                // Fallback if not connected: Cursor check
                if (WindowDetection.IsCursorVisible())
                {
                     await Task.Delay(100, token).ConfigureAwait(false);
                     continue;
                }
            }
            
            // Right Click Logic: "Only hold block" check
            if (!_useLeftButton && RightClickOnlyBlock)
            {
                bool holdingBlock = false;
                if (GameStateClient.Instance.IsConnected)
                {
                    holdingBlock = GameStateClient.Instance.CurrentState.HoldingBlock;
                }
                
                if (!holdingBlock)
                {
                    await Task.Delay(100, token).ConfigureAwait(false);
                    continue;
                }
            }
            
            // Break Blocks Logic: 
            if (_useLeftButton && BreakBlocksEnabled)
            {
                if (GameStateClient.Instance.IsConnected)
                {
                    var state = GameStateClient.Instance.CurrentState;

                    if (GameStateClient.Instance.InjectedVersion.StartsWith("1.21", StringComparison.OrdinalIgnoreCase))
                    {
                        // 1.21: only pause when we are actually breaking a block.
                        if (!state.LookingAtBlock)
                            IsMiningIntent = false;

                        if (state.BreakingBlock || IsMiningIntent)
                        {
                            await Task.Delay(25, token).ConfigureAwait(false);
                            continue;
                        }
                    }
                    else
                    {
                        // 1.8.9: legacy behavior (intent-based).
                        if (!state.LookingAtBlock)
                            IsMiningIntent = false;

                        if (IsMiningIntent)
                        {
                            await Task.Delay(50, token).ConfigureAwait(false);
                            continue;
                        }
                    }
                }
            }
            
            stopwatch.Restart();
            
            float minCps = _useLeftButton ? MinCPS : RightMinCPS;
            float maxCps = _useLeftButton ? MaxCPS : RightMaxCPS;
            if (minCps > maxCps) minCps = maxCps;
            
            // Calculate CPS with optional jitter (gaussian distribution)
            float cps;
            if (JitterEnabled)
            {
                float midCps = (minCps + maxCps) / 2.0f;
                // Slightly widen range for Gaussian to touch edges
                float range = (maxCps - minCps) / 4.0f; 
                cps = GaussianRandom(midCps, range);
                cps = Math.Clamp(cps, minCps, maxCps);
            }
            else
            {
                cps = minCps + (float)_random.NextDouble() * (maxCps - minCps);
            }
            
            double targetInterval = 1000.0 / cps; // in milliseconds

            // Perform click
            PerformClick(_useLeftButton);
            
            // Drift Compensation
            // Stop stopwatch to see how long click + logic took
            stopwatch.Stop();
            double elapsed = stopwatch.Elapsed.TotalMilliseconds;
            
            // Calculate remaining wait time, compensating for elapsed time
            int waitTime = (int)(targetInterval - elapsed);
            if (waitTime < 1) waitTime = 1; // Always yield at least a bit
            
            try
            {
                await Task.Delay(waitTime, token).ConfigureAwait(false);
            }
            catch (TaskCanceledException)
            {
                break;
            }
        }
    }
    
    private void PerformClick(bool leftButton)
    {
        INPUT[] inputs = new INPUT[2];
        
        inputs[0].Type = INPUT_MOUSE;
        inputs[0].Mi.DwFlags = leftButton ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_RIGHTDOWN;
        
        inputs[1].Type = INPUT_MOUSE;
        inputs[1].Mi.DwFlags = leftButton ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_RIGHTUP;
        
        SendInput(2, inputs, Marshal.SizeOf<INPUT>());
    }

    private void TryApplyAimAssist()
    {
        var state = GameStateClient.Instance.CurrentState;
        if (state.Entities.Count == 0) return;

        var rect = WindowDetection.GetMinecraftWindowRect();
        if (!rect.HasValue) return;

        int width = rect.Value.Right - rect.Value.Left;
        int height = rect.Value.Bottom - rect.Value.Top;
        if (width <= 0 || height <= 0) return;

        double centerX = width * 0.5;
        double centerY = height * 0.5;
        double maxAngle = AimAssistFov * 0.5;
        double bestScore = double.MaxValue;
        double bestDx = 0;
        double bestDy = 0;

        foreach (var entity in state.Entities)
        {
            if (entity.Dist <= 0.01 || entity.Dist > AimAssistRange) continue;
            if (entity.Sx < 0 || entity.Sx > width || entity.Sy < 0 || entity.Sy > height) continue;

            double dx = entity.Sx - centerX;
            double dy = entity.Sy - centerY;
            double radial = Math.Sqrt(dx * dx + dy * dy);
            double angle = Math.Atan2(radial, centerX) * (180.0 / Math.PI);
            if (angle > maxAngle) continue;

            double score = dx * dx + dy * dy;
            if (score < bestScore)
            {
                bestScore = score;
                bestDx = dx;
                bestDy = dy;
            }
        }

        if (bestScore == double.MaxValue) return;

        double strength = AimAssistStrength / 100.0;
        int moveX = (int)Math.Round(bestDx * strength);
        int moveY = (int)Math.Round(bestDy * strength);

        if (moveX == 0 && Math.Abs(bestDx) > 1) moveX = Math.Sign(bestDx);
        if (moveY == 0 && Math.Abs(bestDy) > 1) moveY = Math.Sign(bestDy);

        moveX = Math.Clamp(moveX, -40, 40);
        moveY = Math.Clamp(moveY, -40, 40);

        if (moveX == 0 && moveY == 0) return;

        INPUT[] input = new INPUT[1];
        input[0].Type = INPUT_MOUSE;
        input[0].Mi.Dx = moveX;
        input[0].Mi.Dy = moveY;
        input[0].Mi.DwFlags = MOUSEEVENTF_MOVE;

        SendInput(1, input, Marshal.SizeOf<INPUT>());
    }
     
    private float GaussianRandom(float mean, float stddev)
    {
        // Box-Muller transform
        double u1 = 1.0 - _random.NextDouble();
        double u2 = 1.0 - _random.NextDouble();
        double z = Math.Sqrt(-2.0 * Math.Log(u1)) * Math.Cos(2.0 * Math.PI * u2);
        return (float)(mean + z * stddev);
    }
    
    private void OnPropertyChanged(string name)
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    }
}
