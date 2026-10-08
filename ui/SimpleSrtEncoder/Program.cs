using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Velopack;

namespace SimpleSrtEncoder;

/// <summary>
/// Custom entry point (DISABLE_XAML_GENERATED_MAIN in the csproj suppresses the WinUI
/// templates' generated one), required so VelopackApp.Build().Run() can run before any
/// WinAppSDK/XAML initialization. Velopack's Setup.exe/Update.exe re-launch this same exe
/// with special arguments for install/update/uninstall lifecycle events; Run() intercepts
/// those and exits before reaching Application.Start, and is a no-op on a normal launch.
/// </summary>
public static class Program
{
    [STAThread]
    private static void Main(string[] args)
    {
        VelopackApp.Build().Run();

        Application.Start(_ =>
        {
            var context = new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread());
            System.Threading.SynchronizationContext.SetSynchronizationContext(context);
            new App();
        });
    }
}
