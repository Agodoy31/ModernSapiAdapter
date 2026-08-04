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
            var installerWin = new InstallerWindow(InstallerMode.Uninstall);
            installerWin.Show();
            return;
        }

        string currentBaseDir = AppDomain.CurrentDomain.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        string? installLocation = SapiManager.Services.RegistryManager.GetInstallLocation();
        string? installedVersionStr = SapiManager.Services.RegistryManager.GetInstalledVersion();
        
        bool isRunningFromInstallLocation = false;

        if (!string.IsNullOrEmpty(installLocation))
        {
            isRunningFromInstallLocation = string.Equals(currentBaseDir, installLocation.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar), StringComparison.OrdinalIgnoreCase);
        }

        if (isRunningFromInstallLocation)
        {
            var mainWin = new MainWindow();
            mainWin.Show();
            return;
        }

        // We are NOT running from the install location.
        // Check if we have CoreEngine.dll in our current directory.
        string coreEnginePath = Path.Combine(currentBaseDir, "CoreEngine.dll");
        if (!File.Exists(coreEnginePath))
        {
            MessageBox.Show($"Cannot start ModernSapiAdapter Setup.\n\n'CoreEngine.dll' was not found in the source directory:\n{currentBaseDir}\n\nPlease ensure CoreEngine.dll is placed alongside SapiManager.exe before running setup.", "Setup Error - CoreEngine.dll Missing", MessageBoxButton.OK, MessageBoxImage.Error);
            Shutdown();
            return;
        }

        if (string.IsNullOrEmpty(installedVersionStr))
        {
            // Not installed at all, run standard install
            var installerWin = new InstallerWindow(InstallerMode.Install);
            installerWin.Show();
        }
        else
        {
            // Already installed. Compare versions.
            Version currentVersion = System.Reflection.Assembly.GetExecutingAssembly().GetName().Version ?? new Version(1, 0, 0);
            Version.TryParse(installedVersionStr, out Version? installedVersion);
            installedVersion ??= new Version(1, 0, 0);

            if (currentVersion > installedVersion)
            {
                // Current executable is newer than the installed one.
                var installerWin = new InstallerWindow(InstallerMode.Upgrade);
                installerWin.Show();
            }
            else if (currentVersion < installedVersion)
            {
                // Current executable is older than the installed one.
                MessageBox.Show($"A newer version ({installedVersion}) of ModernSapiAdapter is already installed.\n\nPlease launch the installed version from the Start menu or install an even newer version.", "Newer Version Detected", MessageBoxButton.OK, MessageBoxImage.Warning);
                Shutdown();
            }
            else
            {
                // Same version. Offer repair/reinstall. (Using Install mode text is fine, or we could add a Repair mode, but Install mode essentially overwrites).
                MessageBox.Show($"ModernSapiAdapter version {currentVersion} is already installed on this machine.\n\nRunning setup again will overwrite the existing installation.", "Already Installed", MessageBoxButton.OK, MessageBoxImage.Information);
                var installerWin = new InstallerWindow(InstallerMode.Install);
                installerWin.Show();
            }
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
