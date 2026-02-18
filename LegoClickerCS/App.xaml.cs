using System.Windows;
using LegoClickerCS.Core;

namespace LegoClickerCS;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        
        // Install global hooks
        InputHooks.Install();
    }
    
    protected override void OnExit(ExitEventArgs e)
    {
        // Cleanup hooks
        InputHooks.Uninstall();
        Clicker.Instance.Stop();
        
        base.OnExit(e);
    }
}
