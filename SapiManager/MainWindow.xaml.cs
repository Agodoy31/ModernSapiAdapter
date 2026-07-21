using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using SapiManager.Models;
using SapiManager.Services;

namespace SapiManager;

public partial class MainWindow : Window
{
    private readonly List<ProviderManifest> _manifests = new();
    private ProviderManifest? _currentManifest;
    private ProviderUserConfig _currentConfig = new();
    private readonly Dictionary<string, FrameworkElement> _dynamicControlMap = new();
    private readonly Dictionary<string, (CheckBox CheckEnabled, TextBox TxtAlias)> _voiceControlMap = new();

    public MainWindow()
    {
        InitializeComponent();
        Loaded += MainWindow_Loaded;
    }

    private void MainWindow_Loaded(object sender, RoutedEventArgs e)
    {
        ScanProviders();
    }

    private void BtnRefresh_Click(object sender, RoutedEventArgs e)
    {
        ScanProviders();
    }

    private void ScanProviders()
    {
        _manifests.Clear();
        LstProviders.ItemsSource = null;

        string providersDir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "providers");
        if (!Directory.Exists(providersDir))
        {
            try
            {
                Directory.CreateDirectory(providersDir);
            }
            catch { }
        }

        if (Directory.Exists(providersDir))
        {
            foreach (string subDir in Directory.GetDirectories(providersDir))
            {
                string folderName = Path.GetFileName(subDir);
                string manifestPath = Path.Combine(subDir, $"{folderName}_voices.json");
                if (File.Exists(manifestPath))
                {
                    try
                    {
                        string json = File.ReadAllText(manifestPath);
                        var manifest = JsonSerializer.Deserialize<ProviderManifest>(json);
                        if (manifest != null)
                        {
                            manifest.FolderPath = subDir;
                            manifest.ManifestFilePath = manifestPath;
                            manifest.ConfigFilePath = Path.Combine(subDir, $"{folderName}_config.json");
                            _manifests.Add(manifest);
                        }
                    }
                    catch (Exception ex)
                    {
                        System.Diagnostics.Debug.WriteLine($"Error parsing manifest {manifestPath}: {ex.Message}");
                    }
                }
            }
        }

        LstProviders.ItemsSource = _manifests;
        if (_manifests.Count > 0)
        {
            LstProviders.SelectedIndex = 0;
        }
        else
        {
            TxtSelectedProvider.Text = "No Providers Found";
            TxtProviderDetails.Text = $"No valid provider folders found in {providersDir}. Drop provider folders there to begin.";
            PnlVoiceList.Children.Clear();
            PnlDynamicSettings.Children.Clear();
        }
    }

    private void LstProviders_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (LstProviders.SelectedItem is ProviderManifest manifest)
        {
            LoadProvider(manifest);
        }
    }

    private void LoadProvider(ProviderManifest manifest)
    {
        _currentManifest = manifest;
        TxtSelectedProvider.Text = manifest.ProviderName;
        TxtProviderDetails.Text = $"Version: {manifest.Version} | Folder: {manifest.FolderPath}";

        // Load or create _config.json
        if (File.Exists(manifest.ConfigFilePath))
        {
            try
            {
                string json = File.ReadAllText(manifest.ConfigFilePath);
                _currentConfig = JsonSerializer.Deserialize<ProviderUserConfig>(json) ?? new ProviderUserConfig();
            }
            catch
            {
                _currentConfig = new ProviderUserConfig();
            }
        }
        else
        {
            _currentConfig = new ProviderUserConfig();
        }

        RenderVoiceList(manifest);
        RenderDynamicSettings(manifest);
    }

    private void RenderVoiceList(ProviderManifest manifest)
    {
        PnlVoiceList.Children.Clear();
        _voiceControlMap.Clear();

        foreach (var voice in manifest.Voices)
        {
            var voiceConfig = _currentConfig.VoicesConfig.TryGetValue(voice.VoiceId, out var vc)
                ? vc
                : new VoiceUserConfigItem { Enabled = true, CustomAlias = null };

            var border = new Border
            {
                Background = Brushes.White,
                BorderBrush = new SolidColorBrush(Color.FromRgb(226, 232, 240)),
                BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(6),
                Padding = new Thickness(12),
                Margin = new Thickness(0, 0, 0, 8)
            };

            var grid = new Grid();
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(220) });

            var infoStack = new StackPanel();
            var chkEnabled = new CheckBox
            {
                Content = voice.SapiAttributes.Name,
                IsChecked = voiceConfig.Enabled,
                FontWeight = FontWeights.Bold,
                FontSize = 14,
                Foreground = new SolidColorBrush(Color.FromRgb(30, 41, 59))
            };
            System.Windows.Automation.AutomationProperties.SetName(chkEnabled, $"Enable voice {voice.SapiAttributes.Name}");

            var txtSub = new TextBlock
            {
                Text = $"ID: {voice.VoiceId} | Language: {voice.SapiAttributes.Language} | Gender: {voice.SapiAttributes.Gender}",
                FontSize = 11,
                Foreground = new SolidColorBrush(Color.FromRgb(100, 116, 139)),
                Margin = new Thickness(20, 2, 0, 0)
            };
            infoStack.Children.Add(chkEnabled);
            infoStack.Children.Add(txtSub);
            Grid.SetColumn(infoStack, 0);

            var aliasStack = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
            var lblAlias = new TextBlock { Text = "Alias:", VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 6, 0), FontSize = 12 };
            var txtAlias = new TextBox
            {
                Text = voiceConfig.CustomAlias ?? string.Empty,
                Width = 170,
                Height = 28,
                VerticalContentAlignment = VerticalAlignment.Center
            };
            System.Windows.Automation.AutomationProperties.SetName(txtAlias, $"Custom alias for {voice.SapiAttributes.Name}");

            aliasStack.Children.Add(lblAlias);
            aliasStack.Children.Add(txtAlias);
            Grid.SetColumn(aliasStack, 1);

            grid.Children.Add(infoStack);
            grid.Children.Add(aliasStack);
            border.Child = grid;

            PnlVoiceList.Children.Add(border);
            _voiceControlMap[voice.VoiceId] = (chkEnabled, txtAlias);
        }
    }

    private void RenderDynamicSettings(ProviderManifest manifest)
    {
        PnlDynamicSettings.Children.Clear();
        _dynamicControlMap.Clear();

        if (manifest.ConfigSchema == null || manifest.ConfigSchema.Count == 0)
        {
            PnlDynamicSettings.Children.Add(new TextBlock
            {
                Text = "This provider does not require any additional configuration parameters.",
                FontStyle = FontStyles.Italic,
                Foreground = new SolidColorBrush(Color.FromRgb(100, 116, 139))
            });
            return;
        }

        foreach (var item in manifest.ConfigSchema)
        {
            var panel = new StackPanel { Margin = new Thickness(0, 0, 0, 16) };
            var lblTitle = new TextBlock
            {
                Text = item.DisplayName,
                FontWeight = FontWeights.SemiBold,
                FontSize = 14,
                Foreground = new SolidColorBrush(Color.FromRgb(30, 41, 59))
            };
            var lblDesc = new TextBlock
            {
                Text = item.Description,
                FontSize = 12,
                Foreground = new SolidColorBrush(Color.FromRgb(100, 116, 139)),
                Margin = new Thickness(0, 2, 0, 6),
                TextWrapping = TextWrapping.Wrap
            };
            panel.Children.Add(lblTitle);
            panel.Children.Add(lblDesc);

            string key = item.Key;
            string existingValue = string.Empty;

            // Attempt primary load from Credential Manager if securestring
            if (string.Equals(item.Type, "securestring", StringComparison.OrdinalIgnoreCase))
            {
                string? credVal = CredentialManager.ReadUserCredential(manifest.ProviderName, key);
                if (credVal != null)
                {
                    existingValue = credVal;
                }
                else if (_currentConfig.ProviderWideConfig.TryGetValue(key, out var encVal))
                {
                    existingValue = CredentialManager.DecryptMachineDpapi(encVal) ?? string.Empty;
                }
            }
            else
            {
                if (_currentConfig.ProviderWideConfig.TryGetValue(key, out string? val))
                {
                    existingValue = val;
                }
            }

            if (string.Equals(item.Type, "boolean", StringComparison.OrdinalIgnoreCase))
            {
                var chk = new CheckBox
                {
                    Content = $"Enable {item.DisplayName}",
                    IsChecked = bool.TryParse(existingValue, out bool bVal) && bVal
                };
                System.Windows.Automation.AutomationProperties.SetName(chk, item.DisplayName);
                System.Windows.Automation.AutomationProperties.SetHelpText(chk, item.Description);
                panel.Children.Add(chk);
                _dynamicControlMap[key] = chk;
            }
            else
            {
                var txt = new TextBox
                {
                    Text = existingValue,
                    Height = 32,
                    VerticalContentAlignment = VerticalAlignment.Center,
                    Padding = new Thickness(6, 0, 6, 0)
                };
                System.Windows.Automation.AutomationProperties.SetName(txt, item.DisplayName);
                System.Windows.Automation.AutomationProperties.SetHelpText(txt, item.Description);
                panel.Children.Add(txt);
                _dynamicControlMap[key] = txt;
            }

            PnlDynamicSettings.Children.Add(panel);
        }
    }

    private void BtnSaveConfig_Click(object sender, RoutedEventArgs e)
    {
        if (_currentManifest == null) return;

        try
        {
            // 1. Save dynamic schema values
            foreach (var schemaItem in _currentManifest.ConfigSchema)
            {
                string key = schemaItem.Key;
                if (!_dynamicControlMap.TryGetValue(key, out var control)) continue;

                string rawVal = string.Empty;
                if (control is TextBox txt) rawVal = txt.Text;
                else if (control is CheckBox chk) rawVal = (chk.IsChecked == true).ToString();

                if (string.Equals(schemaItem.Type, "securestring", StringComparison.OrdinalIgnoreCase))
                {
                    // Dual-tier storage: Save to Credential Manager & DPAPI fallback in config
                    CredentialManager.SaveUserCredential(_currentManifest.ProviderName, key, rawVal);
                    string encDpapi = CredentialManager.EncryptMachineDpapi(rawVal);
                    _currentConfig.ProviderWideConfig[key] = encDpapi;
                }
                else
                {
                    _currentConfig.ProviderWideConfig[key] = rawVal;
                }
            }

            // 2. Save voice configurations & register/unregister SAPI tokens
            string providerDllPath = Path.Combine(_currentManifest.FolderPath, $"{_currentManifest.ProviderName}.dll");

            foreach (var voice in _currentManifest.Voices)
            {
                if (_voiceControlMap.TryGetValue(voice.VoiceId, out var tuple))
                {
                    bool isEnabled = tuple.CheckEnabled.IsChecked == true;
                    string alias = tuple.TxtAlias.Text.Trim();

                    _currentConfig.VoicesConfig[voice.VoiceId] = new VoiceUserConfigItem
                    {
                        Enabled = isEnabled,
                        CustomAlias = string.IsNullOrWhiteSpace(alias) ? null : alias
                    };

                    if (isEnabled)
                    {
                        RegistryManager.RegisterVoiceToken(_currentManifest, voice, providerDllPath, alias);
                    }
                    else
                    {
                        RegistryManager.UnregisterVoiceToken(_currentManifest.ProviderName, voice.VoiceId);
                    }
                }
            }

            // Write _config.json to disk
            string configJson = JsonSerializer.Serialize(_currentConfig, new JsonSerializerOptions { WriteIndented = true });
            File.WriteAllText(_currentManifest.ConfigFilePath, configJson);

            MessageBox.Show($"Configuration saved and SAPI 5 tokens updated for {_currentManifest.ProviderName}!", "Success", MessageBoxButton.OK, MessageBoxImage.Information);
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Failed to save configuration:\n{ex.Message}", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }
}