using System;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using LegoClickerCS.Core;

namespace LegoClickerCS;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        
        // Subscribe to connection state changes
        GameStateClient.Instance.StateUpdated += UpdateGameStateUI;
        GameStateClient.Instance.PropertyChanged += (s, e) => UpdateGameStateUI();
        
        // Load Config
        var profile = ProfileManager.LoadProfile("config");
        if (profile != null)
        {
            ProfileManager.ApplyToClicker(profile);
        }

        // Initial UI state
        UpdateGameStateUI();
    }
    
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
        
        bool success = await GameStateClient.Instance.InjectAsync();
        
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
                InjectButton.Content = "Connected";
                InjectButton.IsEnabled = false;
            }
            else
            {
                InjectionStatusText.Text = $"Status: {gs.StatusMessage}";
                InjectionStatusText.Foreground = new SolidColorBrush(Color.FromRgb(200, 200, 200));
                
                if (!InjectButton.IsEnabled && !gs.IsInjected)
                {
                    InjectButton.IsEnabled = true;
                    InjectButton.Content = "Inject / Connect";
                }
            }
        });
    }
}
