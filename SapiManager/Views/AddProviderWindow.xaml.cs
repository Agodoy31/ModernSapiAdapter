using System;
using System.IO;
using System.Windows;
using System.Windows.Automation;
using Microsoft.Win32;
using SapiManager.Models;
using SapiManager.Services;

namespace SapiManager.Views;

/// <summary>
/// Modal dialog for inspecting and installing speech provider ZIP packages.
/// </summary>
public partial class AddProviderWindow : Window
{
    private readonly string _providersDir;
    private readonly ZipProviderInstaller _installer;
    private PackageInspectionResult? _inspectionResult;

    public AddProviderWindow(string providersDir)
    {
        InitializeComponent();
        _providersDir = providersDir;
        _installer = new ZipProviderInstaller();
    }

    private void Window_Loaded(object sender, RoutedEventArgs e)
    {
        AutomationProperties.SetName(this, Title);
        TxtTitle.Focus();
    }

    private void BtnBrowse_Click(object sender, RoutedEventArgs e)
    {
        var openFileDialog = new OpenFileDialog
        {
            Filter = "ZIP Packages (*.zip)|*.zip|All Files (*.*)|*.*",
            Title = "Select Speech Provider ZIP Package"
        };

        if (openFileDialog.ShowDialog() == true)
        {
            TxtZipPath.Text = openFileDialog.FileName;
            InspectPackage(openFileDialog.FileName);
        }
    }

    private void InspectPackage(string zipPath)
    {
        _inspectionResult = _installer.InspectPackage(zipPath, _providersDir);

        if (!_inspectionResult.IsValid)
        {
            PnlDetails.IsEnabled = false;
            TxtPkgName.Text = "-";
            TxtPkgVersion.Text = "-";
            TxtPkgPublisher.Text = "-";
            TxtPkgExe.Text = "-";

            TxtStatus.Text = _inspectionResult.ErrorMessage;
            BdrProcessLock.Visibility = Visibility.Collapsed;
            BtnInstall.IsEnabled = false;
            return;
        }

        // Package is valid
        PnlDetails.IsEnabled = true;
        TxtPkgName.Text = !string.IsNullOrWhiteSpace(_inspectionResult.Manifest?.ProviderName)
            ? _inspectionResult.Manifest.ProviderName
            : (_inspectionResult.Manifest?.ProviderId ?? "-");
        TxtPkgVersion.Text = _inspectionResult.Manifest?.Version ?? "-";
        TxtPkgPublisher.Text = _inspectionResult.Manifest?.Publisher ?? "-";
        TxtPkgExe.Text = _inspectionResult.Manifest?.ExecutableName ?? "-";

        // Mode detection & Button text determination
        string buttonText = DetermineInstallButtonText(_inspectionResult);
        BtnInstall.Content = buttonText;
        AutomationProperties.SetName(BtnInstall, buttonText);

        // Process lock detection
        if (_inspectionResult.IsRunningProcessDetected)
        {
            BdrProcessLock.Visibility = Visibility.Visible;
            TxtLockStatus.Text = $"The provider executable '{_inspectionResult.Manifest?.ExecutableName}' (PID {_inspectionResult.RunningProcessId}) is currently running and locking files.";
            BtnInstall.IsEnabled = false;
            TxtStatus.Text = "Process locked. Close running process to proceed with installation.";
        }
        else
        {
            BdrProcessLock.Visibility = Visibility.Collapsed;
            BtnInstall.IsEnabled = true;
            TxtStatus.Text = "Package verified. Ready to install.";
        }
    }

    private static string DetermineInstallButtonText(PackageInspectionResult result)
    {
        if (string.IsNullOrWhiteSpace(result.ExistingVersion))
        {
            return "Install Provider";
        }

        string newVersionStr = result.Manifest?.Version ?? "0.0.0";
        string existingVersionStr = result.ExistingVersion;

        if (Version.TryParse(newVersionStr, out var newVer) && Version.TryParse(existingVersionStr, out var existingVer))
        {
            return newVer > existingVer ? "Upgrade Provider" : "Reinstall Provider";
        }

        // Fallback comparison if version parsing fails
        return string.Compare(newVersionStr, existingVersionStr, StringComparison.OrdinalIgnoreCase) > 0
            ? "Upgrade Provider"
            : "Reinstall Provider";
    }

    private void BtnTerminateProcess_Click(object sender, RoutedEventArgs e)
    {
        if (_inspectionResult == null || !_inspectionResult.IsRunningProcessDetected || _inspectionResult.RunningProcessId <= 0)
        {
            return;
        }

        bool terminated = _installer.TerminateProcess(_inspectionResult.RunningProcessId);
        if (terminated)
        {
            TxtStatus.Text = "Process terminated successfully. Re-inspecting package...";
            InspectPackage(TxtZipPath.Text);
        }
        else
        {
            TxtStatus.Text = "Failed to terminate process. Please close it manually in Task Manager.";
        }
    }

    private void BtnInstall_Click(object sender, RoutedEventArgs e)
    {
        if (_inspectionResult == null || !_inspectionResult.IsValid)
        {
            return;
        }

        string zipPath = TxtZipPath.Text;
        if (string.IsNullOrWhiteSpace(zipPath) || !File.Exists(zipPath))
        {
            TxtStatus.Text = "ZIP package file does not exist.";
            return;
        }

        bool success = _installer.ExtractAndInstall(zipPath, _inspectionResult.TargetInstallDir);
        if (success)
        {
            DialogResult = true;
            Close();
        }
        else
        {
            TxtStatus.Text = "Failed to extract and install provider package.";
        }
    }

    private void BtnCancel_Click(object sender, RoutedEventArgs e)
    {
        DialogResult = false;
        Close();
    }
}
