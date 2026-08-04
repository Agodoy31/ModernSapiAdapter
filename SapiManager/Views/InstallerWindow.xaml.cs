using System;
using System.Diagnostics;
using System.IO;
using System.Windows;
using SapiManager.Services;

namespace SapiManager.Views;

public enum InstallerMode
{
    Install,
    Uninstall,
    Upgrade
}

public partial class InstallerWindow : Window
{
    private readonly InstallerMode _mode;
    private static readonly string TargetDirectory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "ModernSapiAdapter");

    public InstallerWindow(InstallerMode mode)
    {
        InitializeComponent();
        _mode = mode;

        if (_mode == InstallerMode.Uninstall)
        {
            Title = "ModernSapiAdapter Uninstaller";
            TxtTitle.Text = "Uninstall ModernSapiAdapter";
            TxtSubtitle.Text = "Remove ModernSapiAdapter SAPI 5 Integration";
            TxtDescription.Text = "This action will remove all registered ModernSapiAdapter SAPI 5 voice tokens from the Windows Registry, unregister CoreEngine.dll, and remove the application files from Program Files.";
            BtnAction.Content = "Uninstall";
            BtnAction.Background = System.Windows.Media.Brushes.DarkRed;
            System.Windows.Automation.AutomationProperties.SetName(BtnAction, "Uninstall");
            System.Windows.Automation.AutomationProperties.SetHelpText(BtnAction, "Uninstalls ModernSapiAdapter and removes all registered SAPI tokens.");
        }
        else if (_mode == InstallerMode.Upgrade)
        {
            Title = "ModernSapiAdapter Upgrade";
            TxtTitle.Text = "Upgrade ModernSapiAdapter";
            TxtSubtitle.Text = "Upgrade ModernSapiAdapter SAPI 5 Engine";
            TxtDescription.Text = $"This will upgrade the ModernSapiAdapter binaries in:\n{TargetDirectory}\n\nIt will re-register CoreEngine.dll with COM and update the application.";
            BtnAction.Content = "Upgrade";
            System.Windows.Automation.AutomationProperties.SetName(BtnAction, "Upgrade");
            System.Windows.Automation.AutomationProperties.SetHelpText(BtnAction, "Upgrades ModernSapiAdapter in Program Files and updates COM components.");
        }
        else
        {
            Title = "ModernSapiAdapter Setup";
            TxtTitle.Text = "Install ModernSapiAdapter";
            TxtSubtitle.Text = "Register ModernSapiAdapter SAPI 5 Engine";
            TxtDescription.Text = $"This setup wizard will copy the ModernSapiAdapter binaries to:\n{TargetDirectory}\n\nIt will register CoreEngine.dll with COM and enable SAPI 5 voice management.";
            BtnAction.Content = "Install";
            System.Windows.Automation.AutomationProperties.SetName(BtnAction, "Install");
            System.Windows.Automation.AutomationProperties.SetHelpText(BtnAction, "Installs ModernSapiAdapter to Program Files and registers COM components.");
        }
    }

    private void Window_Loaded(object sender, RoutedEventArgs e)
    {
        System.Windows.Automation.AutomationProperties.SetName(this, Title);
        TxtTitle.Focus();
    }

    private void BtnAction_Click(object sender, RoutedEventArgs e)
    {
        BtnAction.IsEnabled = false;
        BtnCancel.IsEnabled = false;

        if (_mode == InstallerMode.Uninstall)
        {
            PerformUninstall();
        }
        else
        {
            PerformInstall();
        }
    }

    private void PerformInstall()
    {
        try
        {
            string sourceDir = AppDomain.CurrentDomain.BaseDirectory;
            string sourceCoreEngine = Path.Combine(sourceDir, "CoreEngine.dll");
            if (!File.Exists(sourceCoreEngine))
            {
                TxtStatus.Text = "Installation aborted: CoreEngine.dll missing.";
                MessageBox.Show($"Installation aborted.\n\n'CoreEngine.dll' was not found in the source directory:\n{sourceDir}\n\nPlease place CoreEngine.dll in the folder alongside SapiManager.exe before running setup.", "Setup Error - CoreEngine.dll Missing", MessageBoxButton.OK, MessageBoxImage.Error);
                BtnAction.IsEnabled = true;
                BtnCancel.IsEnabled = true;
                return;
            }

            TxtStatus.Text = "Creating installation directory...";
            if (!Directory.Exists(TargetDirectory))
            {
                Directory.CreateDirectory(TargetDirectory);
            }

            TxtStatus.Text = "Copying application binaries...";

            // Copy all files in source folder to Program Files target directory
            foreach (string file in Directory.GetFiles(sourceDir, "*.*", SearchOption.AllDirectories))
            {
                string relativePath = Path.GetRelativePath(sourceDir, file);
                string destFile = Path.Combine(TargetDirectory, relativePath);
                string? destDir = Path.GetDirectoryName(destFile);
                if (destDir != null && !Directory.Exists(destDir))
                {
                    Directory.CreateDirectory(destDir);
                }
                File.Copy(file, destFile, true);
            }

            // Register COM DLL if CoreEngine.dll exists
            string coreEnginePath = Path.Combine(TargetDirectory, "CoreEngine.dll");
            if (File.Exists(coreEnginePath))
            {
                TxtStatus.Text = "Registering CoreEngine.dll with COM...";
                ComRegistrar.RegisterComDll(coreEnginePath);
            }

            // Add Uninstall key & Start Menu shortcut
            TxtStatus.Text = "Writing Windows Uninstall entry and Start Menu shortcut...";
            string targetExe = Path.Combine(TargetDirectory, "SapiManager.exe");
            RegistryManager.RegisterUninstallInfo(TargetDirectory);
            ShortcutManager.CreateStartMenuShortcut(targetExe);

            TxtStatus.Text = "Installation completed! Launching installed app...";
            MessageBox.Show("ModernSapiAdapter installed successfully!", "Setup Complete", MessageBoxButton.OK, MessageBoxImage.Information);

            // Launch installed process
            Process.Start(new ProcessStartInfo
            {
                FileName = targetExe,
                UseShellExecute = true
            });

            Application.Current.Shutdown();
        }
        catch (Exception ex)
        {
            TxtStatus.Text = $"Installation failed: {ex.Message}";
            MessageBox.Show($"Installation failed:\n{ex.Message}", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            BtnAction.IsEnabled = true;
            BtnCancel.IsEnabled = true;
        }
    }

    private void PerformUninstall()
    {
        try
        {
            TxtStatus.Text = "Unregistering SAPI 5 tokens, COM server, and shortcuts...";

            string coreEnginePath = Path.Combine(TargetDirectory, "CoreEngine.dll");
            if (File.Exists(coreEnginePath))
            {
                ComRegistrar.UnregisterComDll(coreEnginePath);
            }

            RegistryManager.RemoveAllAdapterRegistryKeys();
            ShortcutManager.RemoveStartMenuShortcut();

            // Spawn detached background process to delete target directory after SapiManager exits
            Process.Start(new ProcessStartInfo
            {
                FileName = "cmd.exe",
                Arguments = $"/c choice /C Y /N /D Y /T 2 & rmdir /s /q \"{TargetDirectory}\"",
                CreateNoWindow = true,
                UseShellExecute = false
            });

            TxtStatus.Text = "Uninstallation complete. ModernSapiAdapter removed.";
            MessageBox.Show("ModernSapiAdapter has been uninstalled successfully.", "Uninstalled", MessageBoxButton.OK, MessageBoxImage.Information);

            Application.Current.Shutdown();
        }
        catch (Exception ex)
        {
            TxtStatus.Text = $"Uninstall failed: {ex.Message}";
            MessageBox.Show($"Uninstallation error:\n{ex.Message}", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            BtnAction.IsEnabled = true;
            BtnCancel.IsEnabled = true;
        }
    }

    private void BtnCancel_Click(object sender, RoutedEventArgs e)
    {
        Application.Current.Shutdown();
    }
}
