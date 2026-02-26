using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using LegoClickerCS.Core;

namespace LegoClickerCS;

public partial class MainWindow : Window
{
    private bool _controlMode;
    private string? _pendingKeybindModuleId;
    private static readonly Dictionary<string, string> ModuleTitles = new()
    {
        ["autoclicker"] = "AutoClicker",
        ["rightclick"] = "Right Click",
        ["jitter"] = "Jitter",
        ["clickinchests"] = "Click in Chests",
        ["breakblocks"] = "Break Blocks",
        ["nametags"] = "Nametags",
        ["chestesp"] = "Chest ESP",
        ["closestplayer"] = "Closest Player"
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

        DataContext = Clicker.Instance;
        
        // Subscribe to connection state changes
        GameStateClient.Instance.StateUpdated += UpdateGameStateUI;
        GameStateClient.Instance.PropertyChanged += (s, e) => UpdateGameStateUI();
        InputHooks.OnStateChanged += InputHooks_OnStateChanged;
        InputHooks.OnKeyCaptured += InputHooks_OnKeyCaptured;
        
        // Load Config
        var profile = ProfileManager.LoadProfile("config");
        if (profile != null)
        {
            ProfileManager.ApplyToClicker(profile);
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

        LoaderPanel.Visibility = Visibility.Collapsed;
        ControlPanel.Visibility = Visibility.Visible;

        ShowInTaskbar = true;
        Show();
        Activate();
    }

    private void EnsureControlModeIfNeeded(GameStateClient gs)
    {
        bool injected121 = Is121Version(gs.InjectedVersion);
        if (gs.IsConnected && injected121)
        {
            Dispatcher.Invoke(EnterControlMode);
        }
    }

    private static bool Is121Version(string? version)
        => !string.IsNullOrWhiteSpace(version) && version.StartsWith("1.21", StringComparison.OrdinalIgnoreCase);
    
    private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ClickCount == 1)
            DragMove();
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

        if (success && Is121Version(GameStateClient.Instance.InjectedVersion))
            EnterControlMode();
        
        InjectButton.Content = success ? "Connected" : "Inject / Connect";
        InjectButton.IsEnabled = !success;
        
        // Force UI update immediately
        UpdateGameStateUI();
    }

    private void UpdateGameStateUI()
    {
        Dispatcher.BeginInvoke(() =>
        {
            var gs = GameStateClient.Instance;
            
            if (gs.IsConnected)
            {
                InjectionStatusText.Text = "Status: Connected & Injected";
                InjectionStatusText.Foreground = new SolidColorBrush(Color.FromRgb(100, 255, 150));
                InjectionProgressBar.Visibility = Visibility.Collapsed;
                InjectionProgressBar.Value = 100;
                InjectButton.Content = "Connected";
                InjectButton.IsEnabled = false;

                EnsureControlModeIfNeeded(gs);

                // (Control-mode entry is handled by EnsureControlModeIfNeeded)
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
                    InjectButton.Content = "Inject / Connect";
                }
            }
        });
    }

    protected override void OnClosed(EventArgs e)
    {
        InputHooks.OnStateChanged -= InputHooks_OnStateChanged;
        InputHooks.OnKeyCaptured -= InputHooks_OnKeyCaptured;
        base.OnClosed(e);
    }

    private void KeybindButton_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button btn || btn.Tag is not string moduleId) return;
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
        SetKeybindButtonContent(KeybindNametagsButton, "nametags");
        SetKeybindButtonContent(KeybindChestEspButton, "chestesp");
        SetKeybindButtonContent(KeybindClosestPlayerButton, "closestplayer");
    }

    private void SetKeybindButtonContent(Button btn, string moduleId)
    {
        string title = ModuleTitles.TryGetValue(moduleId, out string? n) ? n : moduleId;
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
