using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Speech.Synthesis;
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

    private enum ConfigScope
    {
        MachineWide,
        UserWide
    }

    private ConfigScope _activeConfigScope = ConfigScope.MachineWide;
    private string _machineConfigPath = string.Empty;
    private string _userConfigPath = string.Empty;
    private bool _isUpdatingConfigScopeUi = false;

    private SpeechSynthesizer? _synthesizer;
    private readonly List<InstalledVoice> _installedVoices = new();

    public MainWindow()
    {
        InitializeComponent();
        Loaded += MainWindow_Loaded;
    }

    private void MainWindow_Loaded(object sender, RoutedEventArgs e)
    {
        InitializeSynthesizer();
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

                // 1. Scan for actual provider DLL first
                string dllPath = Path.Combine(subDir, $"{folderName}.dll");
                if (!File.Exists(dllPath))
                {
                    dllPath = Directory.GetFiles(subDir, "*.dll").FirstOrDefault() ?? string.Empty;
                }

                if (string.IsNullOrEmpty(dllPath) || !File.Exists(dllPath))
                {
                    // No provider DLL in this subfolder -> Skip completely
                    continue;
                }

                // 2. DLL is present. Now check for JSON manifest
                string manifestPath = Path.Combine(subDir, $"{folderName}_voices.json");
                if (!File.Exists(manifestPath))
                {
                    manifestPath = Directory.GetFiles(subDir, "*_voices.json").FirstOrDefault()
                        ?? Directory.GetFiles(subDir, "voices.json").FirstOrDefault()
                        ?? Directory.GetFiles(subDir, "manifest.json").FirstOrDefault()
                        ?? string.Empty;
                }

                if (string.IsNullOrEmpty(manifestPath) || !File.Exists(manifestPath))
                {
                    // Manifest JSON file missing
                    var manifest = new ProviderManifest
                    {
                        ProviderName = folderName,
                        Version = "Manifest Missing",
                        FolderPath = subDir,
                        DllFilePath = dllPath,
                        IsManifestMissing = true,
                        IsManifestInvalid = false,
                        ConfigFilePath = Path.Combine(subDir, $"{folderName}_config.json")
                    };
                    _manifests.Add(manifest);
                }
                else
                {
                    // Manifest JSON file is present -> attempt schema parsing
                    try
                    {
                        string json = File.ReadAllText(manifestPath);
                        var manifest = JsonSerializer.Deserialize<ProviderManifest>(json);

                        // Verify deserialized object has expected schema shape
                        if (manifest != null && manifest.Voices != null && !string.IsNullOrEmpty(manifest.ProviderName))
                        {
                            manifest.FolderPath = subDir;
                            manifest.ManifestFilePath = manifestPath;
                            manifest.ConfigFilePath = Path.Combine(subDir, $"{folderName}_config.json");
                            manifest.DllFilePath = dllPath;
                            manifest.IsManifestMissing = false;
                            manifest.IsManifestInvalid = false;
                            _manifests.Add(manifest);
                        }
                        else
                        {
                            // Deserialized but failed schema shape validation
                            var invalidManifest = new ProviderManifest
                            {
                                ProviderName = folderName,
                                Version = "Invalid Provider JSON Schema",
                                FolderPath = subDir,
                                DllFilePath = dllPath,
                                ManifestFilePath = manifestPath,
                                IsManifestMissing = false,
                                IsManifestInvalid = true,
                                ConfigFilePath = Path.Combine(subDir, $"{folderName}_config.json")
                            };
                            _manifests.Add(invalidManifest);
                        }
                    }
                    catch
                    {
                        // JSON parsing failed (e.g. JsonException for invalid schema or raw array)
                        var invalidManifest = new ProviderManifest
                        {
                            ProviderName = folderName,
                            Version = "Invalid Provider JSON Schema",
                            FolderPath = subDir,
                            DllFilePath = dllPath,
                            ManifestFilePath = manifestPath,
                            IsManifestMissing = false,
                            IsManifestInvalid = true,
                            ConfigFilePath = Path.Combine(subDir, $"{folderName}_config.json")
                        };
                        _manifests.Add(invalidManifest);
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
            TxtProviderDetails.Text = $"No valid provider DLLs found in {providersDir}. Drop provider folders there to begin.";
            PnlVoiceList.Children.Clear();
            PnlDynamicSettings.Children.Clear();
            PnlUnmanifestedBanner.Visibility = Visibility.Collapsed;
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

        if (manifest.IsManifestMissing)
        {
            TxtProviderDetails.Text = $"Folder: {manifest.FolderPath} | DLL: {Path.GetFileName(manifest.DllFilePath)} (Manifest Missing)";
            TxtBannerTitle.Text = "Voice Manifest Missing";
            TxtBannerDesc.Text = "This provider DLL is installed but missing its voice manifest JSON. Click Generate Voice Manifest to build its voice catalog.";
            PnlUnmanifestedBanner.Visibility = Visibility.Visible;

            PnlVoiceList.Children.Clear();
            PnlVoiceList.Children.Add(new TextBlock
            {
                Text = "No voice manifest found. Click 'Generate Voice Manifest' above to scan and generate the voice catalog.",
                FontStyle = FontStyles.Italic,
                Foreground = new SolidColorBrush(Color.FromRgb(220, 38, 38)),
                Margin = new Thickness(0, 8, 0, 0)
            });

            PnlDynamicSettings.Children.Clear();
            PnlDynamicSettings.Children.Add(new TextBlock
            {
                Text = "Configuration schema unavailable until voice manifest is generated.",
                FontStyle = FontStyles.Italic,
                Foreground = new SolidColorBrush(Color.FromRgb(100, 116, 139))
            });
            return;
        }

        if (manifest.IsManifestInvalid)
        {
            TxtProviderDetails.Text = $"Folder: {manifest.FolderPath} | DLL: {Path.GetFileName(manifest.DllFilePath)} (Invalid Provider JSON Schema)";
            TxtBannerTitle.Text = "Invalid Provider JSON Schema";
            TxtBannerDesc.Text = "The voice manifest JSON file in this provider folder has an invalid schema. Click Generate Voice Manifest to re-generate a valid manifest.";
            PnlUnmanifestedBanner.Visibility = Visibility.Visible;

            PnlVoiceList.Children.Clear();
            PnlVoiceList.Children.Add(new TextBlock
            {
                Text = "Invalid provider JSON schema. Click 'Generate Voice Manifest' above to re-generate a valid manifest.",
                FontStyle = FontStyles.Italic,
                Foreground = new SolidColorBrush(Color.FromRgb(220, 38, 38)),
                Margin = new Thickness(0, 8, 0, 0)
            });

            PnlDynamicSettings.Children.Clear();
            PnlDynamicSettings.Children.Add(new TextBlock
            {
                Text = "Configuration schema unavailable due to invalid manifest JSON schema.",
                FontStyle = FontStyles.Italic,
                Foreground = new SolidColorBrush(Color.FromRgb(100, 116, 139))
            });
            return;
        }

        PnlUnmanifestedBanner.Visibility = Visibility.Collapsed;
        TxtProviderDetails.Text = $"Version: {manifest.Version} | Folder: {manifest.FolderPath}";

        UpdateConfigScopeUi();
        LoadActiveScopeConfig();
    }

    private void UpdateConfigScopeUi()
    {
        if (_currentManifest == null) return;

        _isUpdatingConfigScopeUi = true;
        try
        {
            _machineConfigPath = Path.Combine(_currentManifest.FolderPath, $"{_currentManifest.ProviderName}_config.json");
            string localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            _userConfigPath = Path.Combine(localAppData, "ModernSapiAdapter", "Config", $"{_currentManifest.ProviderName}_config.json");

            bool userConfigExists = File.Exists(_userConfigPath);

            CmbConfigScope.Items.Clear();
            CmbConfigScope.Items.Add("Machine-Wide Baseline (Program Files)");
            CmbConfigScope.Items.Add(userConfigExists
                ? "User-Wide Override (LocalAppData)"
                : "User-Wide Override (LocalAppData) [Not Created]");

            BtnCreateUserConfig.Visibility = userConfigExists ? Visibility.Collapsed : Visibility.Visible;

            if (_activeConfigScope == ConfigScope.UserWide && !userConfigExists)
            {
                _activeConfigScope = ConfigScope.MachineWide;
            }

            CmbConfigScope.SelectedIndex = _activeConfigScope == ConfigScope.MachineWide ? 0 : 1;
            TxtActiveConfigPath.Text = _activeConfigScope == ConfigScope.MachineWide ? _machineConfigPath : _userConfigPath;
        }
        finally
        {
            _isUpdatingConfigScopeUi = false;
        }
    }

    private void LoadActiveScopeConfig()
    {
        if (_currentManifest == null) return;

        string targetPath = _activeConfigScope == ConfigScope.MachineWide ? _machineConfigPath : _userConfigPath;

        if (File.Exists(targetPath))
        {
            try
            {
                string json = File.ReadAllText(targetPath);
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

        RenderVoiceList(_currentManifest);
        RenderDynamicSettings(_currentManifest);

        if (ChkFilterProviderVoices != null && ChkFilterProviderVoices.IsChecked == true)
        {
            PopulateTestVoices();
        }
    }

    private void CmbConfigScope_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_isUpdatingConfigScopeUi || _currentManifest == null) return;

        if (CmbConfigScope.SelectedIndex == 1)
        {
            if (!File.Exists(_userConfigPath))
            {
                MessageBox.Show("User-wide configuration override file does not exist yet. Click 'Create User Override' to create it.", "Notice", MessageBoxButton.OK, MessageBoxImage.Information);
                _isUpdatingConfigScopeUi = true;
                CmbConfigScope.SelectedIndex = 0;
                _isUpdatingConfigScopeUi = false;
                return;
            }
            _activeConfigScope = ConfigScope.UserWide;
        }
        else
        {
            _activeConfigScope = ConfigScope.MachineWide;
        }

        TxtActiveConfigPath.Text = _activeConfigScope == ConfigScope.MachineWide ? _machineConfigPath : _userConfigPath;
        LoadActiveScopeConfig();
    }

    private void BtnCreateUserConfig_Click(object sender, RoutedEventArgs e)
    {
        if (_currentManifest == null) return;

        try
        {
            string configDir = Path.GetDirectoryName(_userConfigPath)!;
            if (!Directory.Exists(configDir))
            {
                Directory.CreateDirectory(configDir);
            }

            if (File.Exists(_machineConfigPath))
            {
                File.Copy(_machineConfigPath, _userConfigPath, overwrite: true);
            }
            else
            {
                string emptyJson = JsonSerializer.Serialize(new ProviderUserConfig(), new JsonSerializerOptions { WriteIndented = true });
                File.WriteAllText(_userConfigPath, emptyJson);
            }

            _activeConfigScope = ConfigScope.UserWide;
            UpdateConfigScopeUi();
            LoadActiveScopeConfig();

            MessageBox.Show($"Created user configuration override file at:\n{_userConfigPath}", "User Override Created", MessageBoxButton.OK, MessageBoxImage.Information);
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Failed to create user configuration override:\n{ex.Message}", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
        }
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
            PnlRawJsonEditor.Visibility = Visibility.Collapsed;
            PnlDynamicSettings.Visibility = Visibility.Visible;
            PnlDynamicSettings.Children.Add(new TextBlock
            {
                Text = "This provider does not require any additional configuration parameters.",
                FontStyle = FontStyles.Italic,
                Foreground = new SolidColorBrush(Color.FromRgb(100, 116, 139))
            });
            return;
        }

        if (manifest.ConfigSchema.Any(c => string.Equals(c.Type, "json_block", StringComparison.OrdinalIgnoreCase)))
        {
            PnlDynamicSettings.Visibility = Visibility.Collapsed;
            PnlRawJsonEditor.Visibility = Visibility.Visible;
            
            string targetPath = _activeConfigScope == ConfigScope.MachineWide ? _machineConfigPath : _userConfigPath;
            if (File.Exists(targetPath))
            {
                TxtRawJsonConfig.Text = File.ReadAllText(targetPath);
            }
            else
            {
                TxtRawJsonConfig.Text = "{\n  // Advanced JSON Configuration\n}";
            }
            return;
        }

        PnlRawJsonEditor.Visibility = Visibility.Collapsed;
        PnlDynamicSettings.Visibility = Visibility.Visible;

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

            // Write to target active scope config file
            string targetPath = _activeConfigScope == ConfigScope.MachineWide ? _machineConfigPath : _userConfigPath;
            string targetDir = Path.GetDirectoryName(targetPath)!;
            if (!Directory.Exists(targetDir))
            {
                Directory.CreateDirectory(targetDir);
            }

            if (_currentManifest.ConfigSchema.Any(c => string.Equals(c.Type, "json_block", StringComparison.OrdinalIgnoreCase)))
            {
                File.WriteAllText(targetPath, TxtRawJsonConfig.Text);
            }
            else
            {
                string configJson = JsonSerializer.Serialize(_currentConfig, new JsonSerializerOptions { WriteIndented = true });
                File.WriteAllText(targetPath, configJson);
            }

            string scopeName = _activeConfigScope == ConfigScope.MachineWide ? "Machine-Wide Baseline" : "User-Wide Override";
            MessageBox.Show($"Configuration saved to {scopeName} file and SAPI 5 tokens updated for {_currentManifest.ProviderName}!\n\nTarget File:\n{targetPath}", "Success", MessageBoxButton.OK, MessageBoxImage.Information);

            // Update UI after saving
            UpdateConfigScopeUi();
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Failed to save configuration:\n{ex.Message}", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    #region SAPI 5 Tester Implementation

    private void InitializeSynthesizer()
    {
        try
        {
            _synthesizer = new SpeechSynthesizer();
            _synthesizer.SpeakStarted += Synthesizer_SpeakStarted;
            _synthesizer.SpeakProgress += Synthesizer_SpeakProgress;
            _synthesizer.BookmarkReached += Synthesizer_BookmarkReached;
            _synthesizer.SpeakCompleted += Synthesizer_SpeakCompleted;
            _synthesizer.StateChanged += Synthesizer_StateChanged;

            // Populate Sample Presets
            CmbSampleText.Items.Clear();
            CmbSampleText.Items.Add("Plain Text Hello");
            CmbSampleText.Items.Add("W3C SSML Bookmarks");
            CmbSampleText.Items.Add("Prosody Test");
            CmbSampleText.Items.Add("CJK Asian Text");
            CmbSampleText.SelectedIndex = 0;

            PopulateTestVoices();
        }
        catch (Exception ex)
        {
            AppendTestLog($"[Error] Failed to initialize SAPI SpeechSynthesizer: {ex.Message}");
        }
    }

    private void PopulateTestVoices()
    {
        if (_synthesizer == null) return;

        CmbTestVoice.Items.Clear();
        _installedVoices.Clear();

        try
        {
            var voices = _synthesizer.GetInstalledVoices();
            bool filterByProvider = ChkFilterProviderVoices.IsChecked == true && _currentManifest != null;

            foreach (var voice in voices)
            {
                if (!voice.Enabled) continue;

                string voiceName = voice.VoiceInfo.Name;
                bool isMatch = true;

                if (filterByProvider && _currentManifest != null)
                {
                    isMatch = _currentManifest.Voices.Any(v =>
                        voiceName.Contains(v.SapiAttributes.Name, StringComparison.OrdinalIgnoreCase) ||
                        (_currentConfig.VoicesConfig.TryGetValue(v.VoiceId, out var vc) && vc.CustomAlias != null && voiceName.Contains(vc.CustomAlias, StringComparison.OrdinalIgnoreCase)));
                }

                if (isMatch)
                {
                    _installedVoices.Add(voice);
                    CmbTestVoice.Items.Add($"{voiceName} ({voice.VoiceInfo.Culture.Name})");
                }
            }

            if (CmbTestVoice.Items.Count > 0)
            {
                CmbTestVoice.SelectedIndex = 0;
            }
            else if (filterByProvider)
            {
                CmbTestVoice.Items.Add("No registered SAPI 5 voices found for provider");
                CmbTestVoice.SelectedIndex = 0;
            }
        }
        catch (Exception ex)
        {
            AppendTestLog($"[Error] Voice enumeration failed: {ex.Message}");
        }
    }

    private void CmbTestVoice_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_synthesizer == null || CmbTestVoice.SelectedIndex < 0 || CmbTestVoice.SelectedIndex >= _installedVoices.Count) return;

        var selectedVoice = _installedVoices[CmbTestVoice.SelectedIndex];
        try
        {
            _synthesizer.SelectVoice(selectedVoice.VoiceInfo.Name);
            AppendTestLog($"[Voice Selected] {selectedVoice.VoiceInfo.Name} ({selectedVoice.VoiceInfo.Culture.Name})");
        }
        catch (Exception ex)
        {
            AppendTestLog($"[Error] Could not select voice '{selectedVoice.VoiceInfo.Name}': {ex.Message}");
        }
    }

    private void ChkFilterProviderVoices_Click(object sender, RoutedEventArgs e)
    {
        PopulateTestVoices();
    }

    private void SldRate_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (TxtRateVal != null) TxtRateVal.Text = ((int)e.NewValue).ToString();
        if (_synthesizer != null)
        {
            try { _synthesizer.Rate = (int)e.NewValue; } catch { }
        }
    }

    private void SldVolume_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (TxtVolumeVal != null) TxtVolumeVal.Text = ((int)e.NewValue).ToString();
        if (_synthesizer != null)
        {
            try { _synthesizer.Volume = (int)e.NewValue; } catch { }
        }
    }

    private void SldPitch_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (TxtPitchVal != null) TxtPitchVal.Text = ((int)e.NewValue).ToString();
    }

    private void CmbSampleText_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (TxtTestInput == null) return;

        switch (CmbSampleText.SelectedIndex)
        {
            case 0:
                TxtTestInput.Text = "Hello! This is a SAPI 5 voice synthesis test using ModernSapiAdapter.";
                if (ChkIsSsml != null) ChkIsSsml.IsChecked = false;
                break;
            case 1:
                TxtTestInput.Text = "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='en-US'>First sentence.<mark name='mark1'/> Second sentence.<mark name='mark2'/> Finished.</speak>";
                if (ChkIsSsml != null) ChkIsSsml.IsChecked = true;
                break;
            case 2:
                TxtTestInput.Text = "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='en-US'><prosody pitch='+20%' rate='+30%'>High pitch and fast rate test.</prosody></speak>";
                if (ChkIsSsml != null) ChkIsSsml.IsChecked = true;
                break;
            case 3:
                TxtTestInput.Text = "你好，欢迎使用 ModernSapiAdapter 语音测试。";
                if (ChkIsSsml != null) ChkIsSsml.IsChecked = false;
                break;
        }
    }

    private void BtnTestPlay_Click(object sender, RoutedEventArgs e)
    {
        if (_synthesizer == null) return;

        try
        {
            if (_synthesizer.State == SynthesizerState.Paused)
            {
                _synthesizer.Resume();
                AppendTestLog("[Play] Speech synthesis resumed.");
                return;
            }

            _synthesizer.SpeakAsyncCancelAll();
            _synthesizer.Rate = (int)SldRate.Value;
            _synthesizer.Volume = (int)SldVolume.Value;

            string text = TxtTestInput.Text;
            if (string.IsNullOrWhiteSpace(text)) return;

            if (ChkIsSsml.IsChecked == true)
            {
                AppendTestLog("[Start] Synthesizing SSML/XML...");
                _synthesizer.SpeakSsmlAsync(text);
            }
            else
            {
                AppendTestLog("[Start] Synthesizing plain text...");
                _synthesizer.SpeakAsync(text);
            }
        }
        catch (Exception ex)
        {
            AppendTestLog($"[Error] Speak failed: {ex.Message}");
        }
    }

    private void BtnTestPause_Click(object sender, RoutedEventArgs e)
    {
        if (_synthesizer == null) return;

        try
        {
            if (_synthesizer.State == SynthesizerState.Speaking)
            {
                _synthesizer.Pause();
                AppendTestLog("[Pause] Speech synthesis paused.");
            }
            else if (_synthesizer.State == SynthesizerState.Paused)
            {
                _synthesizer.Resume();
                AppendTestLog("[Resume] Speech synthesis resumed.");
            }
        }
        catch (Exception ex)
        {
            AppendTestLog($"[Error] Pause toggle failed: {ex.Message}");
        }
    }

    private void BtnTestStop_Click(object sender, RoutedEventArgs e)
    {
        if (_synthesizer == null) return;

        try
        {
            _synthesizer.SpeakAsyncCancelAll();
            AppendTestLog("[Stop] Synthesis cancelled.");
        }
        catch (Exception ex)
        {
            AppendTestLog($"[Error] Stop failed: {ex.Message}");
        }
    }

    private void Synthesizer_SpeakStarted(object? sender, SpeakStartedEventArgs e)
    {
        Dispatcher.Invoke(() => AppendTestLog("[Started] SAPI 5 synthesis engine started."));
    }

    private void Synthesizer_SpeakProgress(object? sender, SpeakProgressEventArgs e)
    {
        Dispatcher.Invoke(() => AppendTestLog($"[Progress] Offset {e.CharacterPosition} (Len {e.CharacterCount}): \"{e.Text}\""));
    }

    private void Synthesizer_BookmarkReached(object? sender, BookmarkReachedEventArgs e)
    {
        Dispatcher.Invoke(() => AppendTestLog($"[Bookmark] Reached mark: \"{e.Bookmark}\" (Audio pos: {e.AudioPosition})"));
    }

    private void Synthesizer_SpeakCompleted(object? sender, SpeakCompletedEventArgs e)
    {
        Dispatcher.Invoke(() =>
        {
            if (e.Cancelled)
            {
                AppendTestLog("[Completed] Speech cancelled by user.");
            }
            else if (e.Error != null)
            {
                AppendTestLog($"[Error] Speech error: {e.Error.Message}");
            }
            else
            {
                AppendTestLog("[Completed] Speech completed successfully.");
            }
        });
    }

    private void Synthesizer_StateChanged(object? sender, StateChangedEventArgs e)
    {
        Dispatcher.Invoke(() =>
        {
            if (BtnTestPlay == null || BtnTestPause == null || BtnTestStop == null) return;

            if (e.State == SynthesizerState.Ready)
            {
                BtnTestPlay.Visibility = Visibility.Visible;
                BtnTestPlay.Content = "Speak";
                BtnTestPause.Visibility = Visibility.Collapsed;
                BtnTestStop.Visibility = Visibility.Collapsed;
            }
            else if (e.State == SynthesizerState.Speaking)
            {
                BtnTestPlay.Visibility = Visibility.Collapsed;
                BtnTestPause.Visibility = Visibility.Visible;
                BtnTestPause.Content = "Pause";
                BtnTestStop.Visibility = Visibility.Visible;
            }
            else if (e.State == SynthesizerState.Paused)
            {
                BtnTestPlay.Visibility = Visibility.Visible;
                BtnTestPlay.Content = "Resume";
                BtnTestPause.Visibility = Visibility.Collapsed;
                BtnTestStop.Visibility = Visibility.Visible;
            }
        });
    }

    private void AppendTestLog(string message)
    {
        if (TxtTestLog == null) return;

        string timeStr = DateTime.Now.ToString("HH:mm:ss.fff");
        TxtTestLog.AppendText($"[{timeStr}] {message}\n");
        TxtTestLog.ScrollToEnd();
    }

    #endregion

    private void BtnGenerateManifest_Click(object sender, RoutedEventArgs e)
    {
        if (_currentManifest != null)
        {
            ExecuteGenerateManifest(_currentManifest);
        }
    }

    private void MenuItem_GenerateManifest_Click(object sender, RoutedEventArgs e)
    {
        if (LstProviders.SelectedItem is ProviderManifest manifest)
        {
            ExecuteGenerateManifest(manifest);
        }
    }

    private void MenuItem_OpenFolder_Click(object sender, RoutedEventArgs e)
    {
        if (LstProviders.SelectedItem is ProviderManifest manifest && !string.IsNullOrEmpty(manifest.FolderPath))
        {
            try
            {
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
                {
                    FileName = manifest.FolderPath,
                    UseShellExecute = true
                });
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Failed to open folder:\n{ex.Message}", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
    }

    private void ExecuteGenerateManifest(ProviderManifest manifest)
    {
        if (string.IsNullOrEmpty(manifest.DllFilePath) || !File.Exists(manifest.DllFilePath))
        {
            MessageBox.Show($"Could not locate provider DLL in {manifest.FolderPath}.", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            return;
        }

        try
        {
            bool success = Services.ComRegistrar.GenerateProviderManifest(manifest.DllFilePath, manifest.FolderPath);
            if (success)
            {
                MessageBox.Show($"Voice manifest generated successfully for {manifest.ProviderName}!", "Success", MessageBoxButton.OK, MessageBoxImage.Information);
                ScanProviders();
            }
            else
            {
                MessageBox.Show($"Failed to generate manifest. Ensure {Path.GetFileName(manifest.DllFilePath)} exports ProviderGenerateManifest.", "Generation Failed", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Error calling ProviderGenerateManifest:\n{ex.Message}", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }
}