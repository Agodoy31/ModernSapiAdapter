using System;
using System.IO;
using System.Linq;
using System.Windows;
using SapiManager.Views;

namespace SapiManager;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        bool isUninstallArg = e.Args.Any(arg => string.Equals(arg, "/uninstall", StringComparison.OrdinalIgnoreCase));

        if (isUninstallArg)
        {
            var installerWin = new InstallerWindow(isUninstallMode: true);
            installerWin.Show();
            return;
        }

        string currentBaseDir = AppDomain.CurrentDomain.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        string programFilesDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "ModernSapiAdapter")
                                     .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);

        bool isRunningInProgramFiles = string.Equals(currentBaseDir, programFilesDir, StringComparison.OrdinalIgnoreCase);

        if (!isRunningInProgramFiles)
        {
            var installerWin = new InstallerWindow(isUninstallMode: false);
            installerWin.Show();
        }
        else
        {
            var mainWin = new MainWindow();
            mainWin.Show();
        }
    }
}
