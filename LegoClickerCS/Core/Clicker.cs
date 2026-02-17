using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Diagnostics;

namespace LegoClickerCS.Core;

public class Clicker : INotifyPropertyChanged
{
    private static Clicker? _instance;
    public static Clicker Instance => _instance ??= new Clicker();
    
    // P/Invoke declarations
    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);
    
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
    private const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    private const uint MOUSEEVENTF_LEFTUP = 0x0004;
    private const uint MOUSEEVENTF_RIGHTDOWN = 0x0008;
    private const uint MOUSEEVENTF_RIGHTUP = 0x0010;
    
    // State
    private bool _isArmed;
    private bool _isClicking;
    private bool _useLeftButton = true;
    private CancellationTokenSource? _clickCts;
    
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
    }
    
    private bool _clickInChests = false;

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
                    
                    // Always block in these specific GUIs
                    bool alwaysBlock = screen.Contains("GuiInventory") || 
                                       screen.Contains("GuiCrafting") ||
                                       screen.Contains("GuiFurnace") ||
                                       screen.Contains("GuiRepair"); // Anvil

                    // Conditionally block in Chests/Containers
                    bool isChest = screen.Contains("GuiChest") || 
                                   screen.Contains("GuiContainer");

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
