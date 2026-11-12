// SPDX-License-Identifier: MIT
// The entry point (instead of the one the XAML compiler generates: DISABLE_XAML_GENERATED_MAIN). "--efi-helper ..." is
// the elevated helper, the same exe started with "runas" by the EFI page for an EFI partition: it opens no window, does
// one mount + read/plan or mount + backup + change + unmount, writes its answer to the --out file and exits. Anything
// else starts the app.
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;

namespace IntelXe.App;

public static class Program
{
    [STAThread]
    static int Main(string[] args)
    {
        if (args.Length > 0 && args[0] == Core.Efi.EfiHelperRequest.Switch) return HelperMain.Run(args);

        WinRT.ComWrappersSupport.InitializeComWrappers();
        Application.Start(p =>
        {
            var context = new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread());
            SynchronizationContext.SetSynchronizationContext(context);
            _ = new App();
        });
        return 0;
    }
}
