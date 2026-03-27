using System;
using System.ComponentModel;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using LegoClickerCS.Core;

namespace LegoClickerCS;

public partial class MainWindow : Window
{
    private sealed class GuiPalette
    {
        public required Color Background { get; init; }
        public required Color Panel { get; init; }
        public required Color SliderBackground { get; init; }
        public required Color SliderForeground { get; init; }
        public required Color Accent { get; init; }
        public required Color Text { get; init; }
        public required Color DimText { get; init; }
        public required Color TabSelected { get; init; }
        public required Color TabHover { get; init; }
    }

    private const int DwmaUseImmersiveDarkMode = 20;
    private const int DwmaBorderColor = 34;
    private const int DwmaCaptionColor = 35;
    private const int DwmaTextColor = 36;

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int dwAttribute, ref int pvAttribute, int cbAttribute);

    private bool _controlMode;
    private string? _pendingKeybindModuleId;
    private int _uiUpdateQueued;
    private static readonly Dictionary<string, string> ModuleTitles = new()
    {
        ["autoclicker"] = "AutoClicker",
        ["rightclick"] = "Right Click",
        ["jitter"] = "Jitter",
        ["clickinchests"] = "Click in Chests",
        ["breakblocks"] = "Break Blocks",
        ["aimassist"] = "Aim Assist",
        ["triggerbot"] = "Triggerbot",
        ["gtbhelper"] = "GTB Helper",
        ["nametags"] = "Nametags",
        ["chestesp"] = "Chest ESP",
        ["closestplayer"] = "Closest Player",
        ["reach"] = "Reach",
        ["velocity"] = "Velocity"
    };

    private static readonly Dictionary<string, GuiPalette> GuiPalettes = new(StringComparer.OrdinalIgnoreCase)
    {
        ["Default"] = new GuiPalette
        {
            Background = (Color)ColorConverter.ConvertFromString("#17131F"),
            Panel = (Color)ColorConverter.ConvertFromString("#241C33"),
            SliderBackground = (Color)ColorConverter.ConvertFromString("#312744"),
            SliderForeground = (Color)ColorConverter.ConvertFromString("#7C65B0"),
            Accent = (Color)ColorConverter.ConvertFromString("#A77DFF"),
            Text = (Color)ColorConverter.ConvertFromString("#DCDCDC"),
            DimText = (Color)ColorConverter.ConvertFromString("#969696"),
            TabSelected = (Color)ColorConverter.ConvertFromString("#7C65B0"),
            TabHover = (Color)ColorConverter.ConvertFromString("#4E3D72")
        },
        ["Dark Blue"] = new GuiPalette
        {
            Background = (Color)ColorConverter.ConvertFromString("#0F172A"),
            Panel = (Color)ColorConverter.ConvertFromString("#1E293B"),
            SliderBackground = (Color)ColorConverter.ConvertFromString("#334155"),
            SliderForeground = (Color)ColorConverter.ConvertFromString("#2563EB"),
            Accent = (Color)ColorConverter.ConvertFromString("#3B82F6"),
            Text = (Color)ColorConverter.ConvertFromString("#E2E8F0"),
            DimText = (Color)ColorConverter.ConvertFromString("#94A3B8"),
            TabSelected = (Color)ColorConverter.ConvertFromString("#1D4ED8"),
            TabHover = (Color)ColorConverter.ConvertFromString("#1E40AF")
        },
        ["Crimson"] = new GuiPalette
        {
            Background = (Color)ColorConverter.ConvertFromString("#2A0F15"),
            Panel = (Color)ColorConverter.ConvertFromString("#3B1E24"),
            SliderBackground = (Color)ColorConverter.ConvertFromString("#5B2433"),
            SliderForeground = (Color)ColorConverter.ConvertFromString("#BE123C"),
            Accent = (Color)ColorConverter.ConvertFromString("#F43F5E"),
            Text = (Color)ColorConverter.ConvertFromString("#FFE4E8"),
            DimText = (Color)ColorConverter.ConvertFromString("#FEB2BE"),
            TabSelected = (Color)ColorConverter.ConvertFromString("#BE123C"),
            TabHover = (Color)ColorConverter.ConvertFromString("#9F1239")
        },
        ["Emerald"] = new GuiPalette
        {
            Background = (Color)ColorConverter.ConvertFromString("#08211E"),
            Panel = (Color)ColorConverter.ConvertFromString("#12352F"),
            SliderBackground = (Color)ColorConverter.ConvertFromString("#1D4D44"),
            SliderForeground = (Color)ColorConverter.ConvertFromString("#059669"),
            Accent = (Color)ColorConverter.ConvertFromString("#10B981"),
            Text = (Color)ColorConverter.ConvertFromString("#D1FAE5"),
            DimText = (Color)ColorConverter.ConvertFromString("#86EFAC"),
            TabSelected = (Color)ColorConverter.ConvertFromString("#047857"),
            TabHover = (Color)ColorConverter.ConvertFromString("#065F46")
        },
        ["Amber"] = new GuiPalette
        {
            Background = (Color)ColorConverter.ConvertFromString("#231704"),
            Panel = (Color)ColorConverter.ConvertFromString("#3A260A"),
            SliderBackground = (Color)ColorConverter.ConvertFromString("#533914"),
            SliderForeground = (Color)ColorConverter.ConvertFromString("#D97706"),
            Accent = (Color)ColorConverter.ConvertFromString("#F59E0B"),
            Text = (Color)ColorConverter.ConvertFromString("#FFEDD5"),
            DimText = (Color)ColorConverter.ConvertFromString("#FCD34D"),
            TabSelected = (Color)ColorConverter.ConvertFromString("#B45309"),
            TabHover = (Color)ColorConverter.ConvertFromString("#92400E")
        },
        ["Slate"] = new GuiPalette
        {
            Background = (Color)ColorConverter.ConvertFromString("#101418"),
            Panel = (Color)ColorConverter.ConvertFromString("#1B232D"),
            SliderBackground = (Color)ColorConverter.ConvertFromString("#2B3440"),
            SliderForeground = (Color)ColorConverter.ConvertFromString("#64748B"),
            Accent = (Color)ColorConverter.ConvertFromString("#94A3B8"),
            Text = (Color)ColorConverter.ConvertFromString("#E2E8F0"),
            DimText = (Color)ColorConverter.ConvertFromString("#94A3B8"),
            TabSelected = (Color)ColorConverter.ConvertFromString("#475569"),
            TabHover = (Color)ColorConverter.ConvertFromString("#334155")
        },
        ["Sunset"] = new GuiPalette
        {
            Background = (Color)ColorConverter.ConvertFromString("#211125"),
            Panel = (Color)ColorConverter.ConvertFromString("#3A1F3F"),
            SliderBackground = (Color)ColorConverter.ConvertFromString("#5D2A48"),
            SliderForeground = (Color)ColorConverter.ConvertFromString("#DB2777"),
            Accent = (Color)ColorConverter.ConvertFromString("#FB7185"),
            Text = (Color)ColorConverter.ConvertFromString("#FCE7F3"),
            DimText = (Color)ColorConverter.ConvertFromString("#F9A8D4"),
            TabSelected = (Color)ColorConverter.ConvertFromString("#BE185D"),
            TabHover = (Color)ColorConverter.ConvertFromString("#9D174D")
        },
        ["Frost"] = new GuiPalette
        {
            Background = (Color)ColorConverter.ConvertFromString("#0D1624"),
            Panel = (Color)ColorConverter.ConvertFromString("#16263A"),
            SliderBackground = (Color)ColorConverter.ConvertFromString("#243D5A"),
            SliderForeground = (Color)ColorConverter.ConvertFromString("#0EA5E9"),
            Accent = (Color)ColorConverter.ConvertFromString("#38BDF8"),
            Text = (Color)ColorConverter.ConvertFromString("#E0F2FE"),
            DimText = (Color)ColorConverter.ConvertFromString("#7DD3FC"),
            TabSelected = (Color)ColorConverter.ConvertFromString("#0284C7"),
            TabHover = (Color)ColorConverter.ConvertFromString("#0369A1")
        }
    };

    private static void LogUi(string msg)
    {
        try
        {
            string path = System.IO.Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "loader_ui_debug.log");
            System.IO.File.AppendAllText(path, $"[{DateTime.Now:HH:mm:ss.fff}] {msg}\r\n");
        }
        catch { }
    }

    public MainWindow()
    {
        InitializeComponent();
        SourceInitialized += (_, _) => ApplyNativeTitleBarTheme();

        DataContext = Clicker.Instance;
        
        // Subscribe to connection state changes
        GameStateClient.Instance.StateUpdated += OnGameStateUpdated;
        GameStateClient.Instance.PropertyChanged += OnGameStateClientPropertyChanged;
        InputHooks.OnStateChanged += InputHooks_OnStateChanged;
        InputHooks.OnKeyCaptured += InputHooks_OnKeyCaptured;
        
        // Load Config
        var profile = ProfileManager.LoadProfile("config");
        if (profile != null)
        {
            ProfileManager.ApplyToClicker(profile);
        }

        if (DataContext is Clicker clicker)
        {
            ApplyGuiTheme(clicker.GuiTheme);
            clicker.ModuleListStyle = NormalizeModuleListStyle(clicker.ModuleListStyle);
        }

        // Initial UI state
        UpdateGameStateUI();
        UpdateKeybindButtons();
    }

    private void ToggleArmed_Click(object sender, RoutedEventArgs e)
    {
        Clicker.Instance.ToggleArmed();
    }

    internal void ShowControlCenterFromBridge()
    {
        // Window should be present at all times; this just brings it to front.
        if (!_controlMode)
            EnterControlMode();

        if (WindowState == WindowState.Minimized)
            WindowState = WindowState.Normal;

        Show();
        Activate();
    }

    private void EnterControlMode()
    {
        if (_controlMode) return;
        _controlMode = true;

        LogUi("EnterControlMode()");

        Title = "LegoClicker";
        Width = 1020;
        Height = 760;
        ResizeMode = ResizeMode.CanResizeWithGrip;
        ApplyNativeTitleBarTheme();

        LoaderPanel.Visibility = Visibility.Collapsed;
        ControlPanel.Visibility = Visibility.Visible;

        ShowInTaskbar = true;
        Show();
        Activate();
    }

    private void ApplyNativeTitleBarTheme()
    {
        IntPtr hwnd = new WindowInteropHelper(this).Handle;
        if (hwnd == IntPtr.Zero) return;

        int darkMode = 1;
        _ = DwmSetWindowAttribute(hwnd, DwmaUseImmersiveDarkMode, ref darkMode, sizeof(int));

        if (TryFindResource("PanelColor") is Color panelColor)
        {
            int caption = ToColorRef(panelColor);
            _ = DwmSetWindowAttribute(hwnd, DwmaCaptionColor, ref caption, sizeof(int));
            int border = ToColorRef(panelColor);
            _ = DwmSetWindowAttribute(hwnd, DwmaBorderColor, ref border, sizeof(int));
        }

        if (TryFindResource("TextColor") is Color textColor)
        {
            int text = ToColorRef(textColor);
            _ = DwmSetWindowAttribute(hwnd, DwmaTextColor, ref text, sizeof(int));
        }

    }

    private static int ToColorRef(Color color)
    {
        return color.R | (color.G << 8) | (color.B << 16);
    }

    private void EnsureControlModeIfNeeded(GameStateClient gs)
    {
        if (gs.IsConnected)
        {
            Dispatcher.Invoke(EnterControlMode);
        }
    }

    private static bool IsModuleSupported(string moduleId)
        => GameStateClient.Instance.SupportsModule(moduleId);

    private void UpdateVersionAvailabilityUi()
    {
        bool aimAssistSupported = IsModuleSupported("aimassist");
        bool triggerbotSupported = IsModuleSupported("triggerbot");
        bool gtbSupported = IsModuleSupported("gtbhelper");
        bool reachSupported = IsModuleSupported("reach");
        bool velocitySupported = IsModuleSupported("velocity");

        string available = "Available";
        string unavailable = "Unavailable on current bridge";

        AimAssistCard.IsEnabled = aimAssistSupported;
        TriggerbotCard.IsEnabled = triggerbotSupported;
        GtbHelperCard.IsEnabled = gtbSupported;
        ReachCard.IsEnabled = reachSupported;
        VelocityCard.IsEnabled = velocitySupported;

        var clicker = Clicker.Instance;
        if (!aimAssistSupported && clicker.AimAssistEnabled) clicker.AimAssistEnabled = false;
        if (!triggerbotSupported && clicker.TriggerbotEnabled) clicker.TriggerbotEnabled = false;
        if (!gtbSupported && clicker.GtbHelperEnabled) clicker.GtbHelperEnabled = false;
        if (!reachSupported && clicker.ReachEnabled) clicker.ReachEnabled = false;
        if (!velocitySupported && clicker.VelocityEnabled) clicker.VelocityEnabled = false;

        AimAssistAvailabilityText.Text = aimAssistSupported ? available : unavailable;
        TriggerbotAvailabilityText.Text = triggerbotSupported ? available : unavailable;
        ReachAvailabilityText.Text = reachSupported ? available : unavailable;
        VelocityAvailabilityText.Text = velocitySupported ? available : unavailable;
        GtbHelperAvailabilityText.Text = gtbSupported
            ? "Hypixel Guess The Build helper using action-bar hints."
            : unavailable;

        KeybindAimAssistButton.IsEnabled = aimAssistSupported;
        KeybindTriggerbotButton.IsEnabled = triggerbotSupported;
        KeybindGtbHelperButton.IsEnabled = gtbSupported;
        KeybindReachButton.IsEnabled = reachSupported;
        KeybindVelocityButton.IsEnabled = velocitySupported;
    }
    
    private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ClickCount == 1)
            DragMove();
    }


    private void GuiTheme_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (DataContext is Clicker clicker)
        {
            ApplyGuiTheme(clicker.GuiTheme);
        }
    }

    private void GuiPaletteButton_Click(object sender, RoutedEventArgs e)
    {
        if (DataContext is not Clicker clicker || sender is not Button { Tag: string themeName })
            return;

        clicker.GuiTheme = themeName;
        ApplyGuiTheme(themeName);
    }

    private void ModuleStyleButton_Click(object sender, RoutedEventArgs e)
    {
        if (DataContext is not Clicker clicker || sender is not Button { Tag: string styleName })
            return;

        clicker.ModuleListStyle = NormalizeModuleListStyle(styleName);
    }

    private static string NormalizeModuleListStyle(string? styleName)
    {
        if (string.IsNullOrWhiteSpace(styleName))
            return "Default";

        return styleName.Trim().ToLowerInvariant() switch
        {
            "default" => "Default",
            "minimal" => "Minimal",
            "outlined" => "Outlined",
            "glass" => "Glass",
            "bold" => "Bold",
            _ => "Default"
        };
    }

    private static string NormalizeThemeName(string? themeName)
    {
        if (string.IsNullOrWhiteSpace(themeName))
            return "Default";

        foreach (var name in GuiPalettes.Keys)
        {
            if (name.Equals(themeName.Trim(), StringComparison.OrdinalIgnoreCase))
                return name;
        }

        return "Default";
    }

    private void ApplyGuiTheme(string? themeName)
    {
        string normalized = NormalizeThemeName(themeName);
        if (!GuiPalettes.TryGetValue(normalized, out GuiPalette? palette))
            palette = GuiPalettes["Default"];

        SetColorResource("BgColor", palette.Background);
        SetColorResource("PanelColor", palette.Panel);
        SetColorResource("SliderBgColor", palette.SliderBackground);
        SetColorResource("SliderFgColor", palette.SliderForeground);
        SetColorResource("AccentColor", palette.Accent);
        SetColorResource("TextColor", palette.Text);
        SetColorResource("DimTextColor", palette.DimText);

        SetBrushColor("BgBrush", palette.Background);
        SetBrushColor("PanelBrush", palette.Panel);
        SetBrushColor("SliderBgBrush", palette.SliderBackground);
        SetBrushColor("SliderFgBrush", palette.SliderForeground);
        SetBrushColor("AccentBrush", palette.Accent);
        SetBrushColor("TextBrush", palette.Text);
        SetBrushColor("DimTextBrush", palette.DimText);
        SetBrushColor("ControlCenterTabSelectedBrush", palette.TabSelected, this.Resources);
        SetBrushColor("ControlCenterTabHoverBrush", palette.TabHover, this.Resources);

        if (DataContext is Clicker clicker)
            clicker.GuiTheme = normalized;

        ApplyNativeTitleBarTheme();
        UpdateGameStateUI();
    }

    private static void SetColorResource(string key, Color value)
    {
        var app = Application.Current;
        if (app == null) return;
        app.Resources[key] = value;
    }

    private static void SetBrushColor(string key, Color value, ResourceDictionary? dictionary = null)
    {
        var target = dictionary ?? Application.Current?.Resources;
        if (target == null) return;

        if (target[key] is SolidColorBrush brush)
        {
            if (!brush.IsFrozen)
            {
                brush.Color = value;
                return;
            }

            target[key] = new SolidColorBrush(value);
            return;
        }

        target[key] = new SolidColorBrush(value);
    }

    private void CloseButton_Click(object sender, RoutedEventArgs e)
    {
        // Save Config
        var profile = ProfileManager.CreateFromClicker();
        profile.Name = "config";
        ProfileManager.SaveProfile(profile);

        Application.Current.Shutdown();
    }
    
    private async void InjectButton_Click(object sender, RoutedEventArgs e)
    {
        InjectButton.IsEnabled = false;
        InjectButton.Content = "...";
        InjectionStatusText.Text = "Status: Connecting...";
        
        LogUi("InjectButton_Click: version=auto");
        bool success = await GameStateClient.Instance.InjectAsync();

        LogUi($"InjectAsync returned: success={success} IsConnected={GameStateClient.Instance.IsConnected} InjectedVersion={GameStateClient.Instance.InjectedVersion}");

        if (success)
            EnterControlMode();
        
        InjectButton.Content = success ? "Connected" : "Inject";
        InjectButton.IsEnabled = !success;
        
        // Force UI update immediately
        UpdateGameStateUI();
    }

    private void UpdateGameStateUI()
    {
        if (Dispatcher.HasShutdownStarted || Dispatcher.HasShutdownFinished) return;
        if (Interlocked.Exchange(ref _uiUpdateQueued, 1) != 0) return;

        try
        {
            Dispatcher.BeginInvoke(() =>
            {
                try
                {
                    var gs = GameStateClient.Instance;

                    if (gs.IsConnected)
                    {
                        InjectionStatusText.Text = "Status: Connected & Injected";
                        InjectionStatusText.Foreground = (Brush)(TryFindResource("AccentBrush") ?? new SolidColorBrush(Color.FromRgb(167, 125, 255)));
                        InjectionProgressBar.Visibility = Visibility.Collapsed;
                        InjectionProgressBar.Value = 100;
                        InjectButton.Content = "Connected";
                        InjectButton.IsEnabled = false;

                        EnsureControlModeIfNeeded(gs);
                        UpdateVersionAvailabilityUi();
                    }
                    else
                    {
                        InjectionStatusText.Text = $"Status: {gs.StatusMessage}";
                        InjectionStatusText.Foreground = new SolidColorBrush(Color.FromRgb(200, 200, 200));
                        InjectionProgressBar.Visibility = gs.IsInjectionInProgress ? Visibility.Visible : Visibility.Collapsed;
                        InjectionProgressBar.Value = gs.IsInjectionInProgress ? gs.InjectionProgress : 0;

                        if (!InjectButton.IsEnabled && !gs.IsInjected)
                        {
                            InjectButton.IsEnabled = true;
                            InjectButton.Content = "Inject";
                        }
                    }

                    if (!gs.IsConnected)
                    {
                        UpdateVersionAvailabilityUi();
                    }
                }
                finally
                {
                    Interlocked.Exchange(ref _uiUpdateQueued, 0);
                }
            });
        }
        catch
        {
            Interlocked.Exchange(ref _uiUpdateQueued, 0);
            throw;
        }
    }

    private void OnGameStateUpdated()
    {
        UpdateGameStateUI();
    }

    private void OnGameStateClientPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(GameStateClient.IsConnected) ||
            e.PropertyName == nameof(GameStateClient.StatusMessage) ||
            e.PropertyName == nameof(GameStateClient.IsInjectionInProgress) ||
            e.PropertyName == nameof(GameStateClient.InjectionProgress) ||
            e.PropertyName == nameof(GameStateClient.IsInjected) ||
            e.PropertyName == nameof(GameStateClient.InjectedVersion) ||
            e.PropertyName == nameof(GameStateClient.Capabilities))
        {
            UpdateGameStateUI();
        }
    }

    protected override void OnClosed(EventArgs e)
    {
        var profile = ProfileManager.CreateFromClicker();
        profile.Name = "config";
        ProfileManager.SaveProfile(profile);

        InputHooks.OnStateChanged -= InputHooks_OnStateChanged;
        InputHooks.OnKeyCaptured -= InputHooks_OnKeyCaptured;
        GameStateClient.Instance.StateUpdated -= OnGameStateUpdated;
        GameStateClient.Instance.PropertyChanged -= OnGameStateClientPropertyChanged;
        base.OnClosed(e);
    }

    private void KeybindButton_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button btn || btn.Tag is not string moduleId) return;
        if (!IsModuleSupported(moduleId)) return;
        _pendingKeybindModuleId = moduleId;
        InputHooks.StartKeyCapture();
        UpdateKeybindButtons();
    }

    private void InputHooks_OnKeyCaptured(int vkCode)
    {
        string? moduleId = _pendingKeybindModuleId;
        if (string.IsNullOrWhiteSpace(moduleId)) return;

        int finalVk = (vkCode == 0x1B) ? 0 : vkCode; // ESC unbinds.
        InputHooks.SetModuleKey(moduleId, finalVk);
        _pendingKeybindModuleId = null;
        Dispatcher.BeginInvoke(UpdateKeybindButtons);
    }

    private void InputHooks_OnStateChanged()
    {
        Dispatcher.BeginInvoke(UpdateKeybindButtons);
    }

    private void UpdateKeybindButtons()
    {
        SetKeybindButtonContent(KeybindAutoclickerButton, "autoclicker");
        SetKeybindButtonContent(KeybindRightClickButton, "rightclick");
        SetKeybindButtonContent(KeybindJitterButton, "jitter");
        SetKeybindButtonContent(KeybindClickInChestsButton, "clickinchests");
        SetKeybindButtonContent(KeybindBreakBlocksButton, "breakblocks");
        SetKeybindButtonContent(KeybindAimAssistButton, "aimassist");
        SetKeybindButtonContent(KeybindTriggerbotButton, "triggerbot");
        SetKeybindButtonContent(KeybindGtbHelperButton, "gtbhelper");
        SetKeybindButtonContent(KeybindNametagsButton, "nametags");
        SetKeybindButtonContent(KeybindChestEspButton, "chestesp");
        SetKeybindButtonContent(KeybindClosestPlayerButton, "closestplayer");
        SetKeybindButtonContent(KeybindReachButton, "reach");
        SetKeybindButtonContent(KeybindVelocityButton, "velocity");
    }

    private void SetKeybindButtonContent(Button btn, string moduleId)
    {
        string title = ModuleTitles.TryGetValue(moduleId, out string? n) ? n : moduleId;
        if (!IsModuleSupported(moduleId))
        {
            btn.Content = $"{title}: Unavailable on current bridge";
            return;
        }

        if (_pendingKeybindModuleId == moduleId)
            btn.Content = $"{title}: [Press key...]";
        else
            btn.Content = $"{title}: {FormatVirtualKey(InputHooks.GetModuleKey(moduleId))}";
    }

    private static string FormatVirtualKey(int vk)
    {
        if (vk <= 0) return "Unbound";
        if (vk >= 0x70 && vk <= 0x7B) return $"F{vk - 0x6F}";
        if (vk == 0xC0) return "`";
        if (vk >= 0x30 && vk <= 0x39) return ((char)vk).ToString();
        if (vk >= 0x41 && vk <= 0x5A) return ((char)vk).ToString();
        return ((Key)KeyInterop.KeyFromVirtualKey(vk)).ToString();
    }
}
