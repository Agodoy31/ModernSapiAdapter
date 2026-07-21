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

        DispatcherUnhandledException += App_DispatcherUnhandledException;
        AppDomain.CurrentDomain.UnhandledException += CurrentDomain_UnhandledException;

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

    private void App_DispatcherUnhandledException(object sender, System.Windows.Threading.DispatcherUnhandledExceptionEventArgs e)
    {
        LogException(e.Exception);
        MessageBox.Show($"An unhandled error occurred in SapiManager:\n\n{e.Exception.Message}\n\nDetails written to LocalAppData log.", "SapiManager Error", MessageBoxButton.OK, MessageBoxImage.Error);
        e.Handled = true;
    }

    private void CurrentDomain_UnhandledException(object sender, UnhandledExceptionEventArgs e)
    {
        if (e.ExceptionObject is Exception ex)
        {
            LogException(ex);
        }
    }

    private static void LogException(Exception ex)
    {
        try
        {
            string localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            string logDir = Path.Combine(localAppData, "ModernSapiAdapter", "Logs");
            Directory.CreateDirectory(logDir);
            string logFile = Path.Combine(logDir, "crash.log");
            string logMsg = $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}] Unhandled Exception: {ex}\n\n";
            File.AppendAllText(logFile, logMsg);
        }
        catch { }
    }
}
