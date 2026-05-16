using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using Aoko.Core;

namespace Aoko;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        RenderOptions.ProcessRenderMode = RenderMode.SoftwareOnly;

        base.OnStartup(e);

        // Install global hooks
        InputHooks.Install();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        DiscordRichPresenceService.Instance.Stop();
        // Cleanup hooks
        InputHooks.Uninstall();
        Clicker.Instance.Stop();

        base.OnExit(e);
    }
}
