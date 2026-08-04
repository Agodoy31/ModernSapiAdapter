using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Speech.Synthesis;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Input;
using Microsoft.Win32;
using SapiManager.Models;
using SapiManager.Services;

namespace SapiManager;

public partial class MainWindow : Window
{
    private readonly ProviderDiscovery _prober = new();
    private readonly ObservableCollection<ProviderViewModel> _providers = new();

    private SpeechSynthesizer? _synthesizer;
    private readonly List<InstalledVoice> _installedVoices = new();

    public MainWindow()
    {
        InitializeComponent();
        Loaded += MainWindow_Loaded;
    }

    private async void MainWindow_Loaded(object sender, RoutedEventArgs e)
    {
        InitializeSynthesizer();
        await ScanProvidersAsync();
    }

    private async void BtnRefresh_Click(object sender, RoutedEventArgs e)
    {
        await ScanProvidersAsync();
    }

    private async Task ScanProvidersAsync()
    {
        _providers.Clear();
        LstProviders.ItemsSource = _providers;

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
            var subDirs = Directory.GetDirectories(providersDir);
            foreach (var dir in subDirs)
            {
                string[] exeFiles = Directory.GetFiles(dir, "*.exe");
                foreach (var exePath in exeFiles)
                {
                    await TryAddProviderFromExeAsync(exePath);
                }

                if (exeFiles.Length == 0)
                {
                    string[] manifestFiles = Directory.GetFiles(dir, "*_voices.json");
                    foreach (var manifestPath in manifestFiles)
                    {
                        TryAddProviderFromManifest(manifestPath, dir);
                    }
                }
            }
        }

        if (_providers.Count > 0)
        {
            LstProviders.SelectedIndex = 0;
        }
        else
        {
            PnlProviderDetails.IsEnabled = false;
            TxtSelectedProvider.Text = "No Providers Found";
            TxtProviderDetails.Text = $"No active speech providers found in {providersDir}. Drop provider folders there or click 'Add Provider...' to select an executable.";
        }
    }

    private async Task<bool> TryAddProviderFromExeAsync(string exePath)
    {
        if (_providers.Any(p => string.Equals(p.ExePath, exePath, StringComparison.OrdinalIgnoreCase)))
        {
            return false;
        }

        try
        {
            var probeResult = await _prober.ProbeProviderAsync(exePath);
            if (probeResult != null && !string.IsNullOrEmpty(probeResult.ProviderId))
            {
                var providerVm = new ProviderViewModel
                {
                    ProviderId = probeResult.ProviderId,
                    ProviderName = probeResult.Info?.ProviderName ?? probeResult.ProviderId,
                    Version = probeResult.Info?.Version ?? "1.0.0",
                    ExePath = exePath,
                    PipeName = probeResult.ProviderId
                };

                foreach (var v in probeResult.Voices)
                {
                    string tokenName = $"MSA_{providerVm.ProviderId}_{v.Id}";
                    bool isRegistered = RegistryManager.IsVoiceTokenRegistered(tokenName);

                    providerVm.Voices.Add(new VoiceViewModel
                    {
                        VoiceId = v.Id,
                        Name = v.Name,
                        Language = v.Language,
                        Gender = v.Gender,
                        Vendor = v.Vendor,
                        IsRegistered = isRegistered
                    });
                }

                _providers.Add(providerVm);
                return true;
            }
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"Failed probing {exePath}: {ex.Message}");
        }

        // Fallback: Check if manifest file exists in folder
        string dir = Path.GetDirectoryName(exePath)!;
        string[] manifestFiles = Directory.GetFiles(dir, "*_voices.json");
        if (manifestFiles.Length > 0)
        {
            return TryAddProviderFromManifest(manifestFiles[0], dir, exePath);
        }

        return false;
    }

    private bool TryAddProviderFromManifest(string manifestPath, string folderPath, string? exePath = null)
    {
        try
        {
            string json = File.ReadAllText(manifestPath);
            var manifest = JsonSerializer.Deserialize<ProviderManifest>(json);
            if (manifest != null && !string.IsNullOrEmpty(manifest.ProviderName))
            {
                string resolvedExe = exePath ?? Directory.GetFiles(folderPath, "*.exe").FirstOrDefault() ?? string.Empty;
                if (_providers.Any(p => string.Equals(p.ProviderId, manifest.ProviderName, StringComparison.OrdinalIgnoreCase)))
                {
                    return false;
                }

                var providerVm = new ProviderViewModel
                {
                    ProviderId = manifest.ProviderName,
                    ProviderName = manifest.ProviderName,
                    Version = manifest.Version,
                    ExePath = resolvedExe,
                    PipeName = manifest.ProviderName
                };

                foreach (var v in manifest.Voices)
                {
                    string tokenName = $"MSA_{manifest.ProviderName}_{v.VoiceId}";
                    bool isRegistered = RegistryManager.IsVoiceTokenRegistered(tokenName);

                    providerVm.Voices.Add(new VoiceViewModel
                    {
                        VoiceId = v.VoiceId,
                        Name = v.SapiAttributes.Name,
                        Language = v.SapiAttributes.Language,
                        Gender = v.SapiAttributes.Gender,
                        Vendor = v.SapiAttributes.Vendor,
                        IsRegistered = isRegistered
                    });
                }

                _providers.Add(providerVm);
                return true;
            }
        }
        catch { }

        return false;
    }

    private async void BtnAddProvider_Click(object sender, RoutedEventArgs e)
    {
        string providersDir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "providers");
        if (!Directory.Exists(providersDir))
        {
            try
            {
                Directory.CreateDirectory(providersDir);
            }
            catch { }
        }

        var addWindow = new SapiManager.Views.AddProviderWindow(providersDir)
        {
            Owner = this
        };

        ModalOverlay.Visibility = Visibility.Visible;
        bool? dialogResult = addWindow.ShowDialog();
        ModalOverlay.Visibility = Visibility.Collapsed;

        if (dialogResult == true)
        {
            await ScanProvidersAsync();
        }
    }

    private void LstProviders_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (LstProviders.SelectedItem is ProviderViewModel selectedProvider)
        {
            PnlProviderDetails.IsEnabled = true;
            TxtSelectedProvider.Text = selectedProvider.ProviderName;
            TxtProviderDetails.Text = $"Version: {selectedProvider.Version} | Executable: {selectedProvider.ExePath}";

            AutomationProperties.SetName(BtnConfigure, $"Configure {selectedProvider.ProviderName}");

            LstVoices.ItemsSource = selectedProvider.Voices;
        }
        else
        {
            PnlProviderDetails.IsEnabled = false;
            TxtSelectedProvider.Text = "Select a Provider";
            TxtProviderDetails.Text = "Select a provider from the sidebar to manage voices and settings.";
            LstVoices.ItemsSource = null;
            AutomationProperties.SetName(BtnConfigure, "Configure Provider");
        }
    }

    private void BtnConfigure_Click(object sender, RoutedEventArgs e)
    {
        if (LstProviders.SelectedItem is ProviderViewModel selectedProvider)
        {
            if (string.IsNullOrWhiteSpace(selectedProvider.ExePath) || !File.Exists(selectedProvider.ExePath))
            {
                MessageBox.Show($"Provider executable path is invalid or missing:\n'{selectedProvider.ExePath}'", "Configuration Error", MessageBoxButton.OK, MessageBoxImage.Error);
                return;
            }

            try
            {
                Process.Start(new ProcessStartInfo
                {
                    FileName = selectedProvider.ExePath,
                    Arguments = "/config",
                    UseShellExecute = true
                });
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Failed to launch configuration for {selectedProvider.ProviderName}:\n{ex.Message}", "Configuration Launch Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
    }

    private void LstVoices_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Space && LstVoices.SelectedItem is VoiceViewModel voice)
        {
            voice.IsRegistered = !voice.IsRegistered;
            e.Handled = true;
        }
    }

    private void BtnSaveVoices_Click(object sender, RoutedEventArgs e)
    {
        if (LstProviders.SelectedItem is ProviderViewModel selectedProvider)
        {
            int registeredCount = 0;
            int unregisteredCount = 0;
            int failCount = 0;

            foreach (var voice in selectedProvider.Voices)
            {
                string tokenName = $"MSA_{selectedProvider.ProviderId}_{voice.VoiceId}";
                if (voice.IsRegistered)
                {
                    bool success = RegistryManager.RegisterVoiceToken(
                        voiceTokenName: tokenName,
                        voiceName: voice.Name,
                        voiceId: voice.VoiceId,
                        providerExePath: selectedProvider.ExePath,
                        providerPipeName: selectedProvider.PipeName,
                        bcp47Language: voice.Language,
                        gender: voice.Gender,
                        vendor: voice.Vendor
                    );
                    if (success) registeredCount++;
                    else failCount++;
                }
                else
                {
                    bool success = RegistryManager.UnregisterVoiceToken(tokenName);
                    if (success) unregisteredCount++;
                    else failCount++;
                }
            }

            if (failCount == 0)
            {
                MessageBox.Show(
                    $"Successfully updated SAPI 5 voice tokens for '{selectedProvider.ProviderName}'!\n\nRegistered: {registeredCount}\nUnregistered: {unregisteredCount}",
                    "Voices Saved",
                    MessageBoxButton.OK,
                    MessageBoxImage.Information);
            }
            else
            {
                MessageBox.Show(
                    $"Updated SAPI 5 voice tokens with warnings/errors.\n\nRegistered: {registeredCount}\nUnregistered: {unregisteredCount}\nFailed: {failCount}\n\nMake sure SapiManager is running with Administrator privileges to modify HKLM registry.",
                    "Voices Saved with Errors",
                    MessageBoxButton.OK,
                    MessageBoxImage.Warning);
            }

            PopulateTestVoices(reinitialize: true);
        }
    }

    #region SAPI 5 Tester Implementation

    private void InitializeSynthesizer()
    {
        try
        {
            ReinitializeSynthesizer();

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

    private void ReinitializeSynthesizer()
    {
        try
        {
            if (_synthesizer != null)
            {
                _synthesizer.SpeakAsyncCancelAll();
                _synthesizer.SpeakStarted -= Synthesizer_SpeakStarted;
                _synthesizer.SpeakProgress -= Synthesizer_SpeakProgress;
                _synthesizer.BookmarkReached -= Synthesizer_BookmarkReached;
                _synthesizer.SpeakCompleted -= Synthesizer_SpeakCompleted;
                _synthesizer.StateChanged -= Synthesizer_StateChanged;
                _synthesizer.Dispose();
            }

            _synthesizer = new SpeechSynthesizer();
            _synthesizer.SpeakStarted += Synthesizer_SpeakStarted;
            _synthesizer.SpeakProgress += Synthesizer_SpeakProgress;
            _synthesizer.BookmarkReached += Synthesizer_BookmarkReached;
            _synthesizer.SpeakCompleted += Synthesizer_SpeakCompleted;
            _synthesizer.StateChanged += Synthesizer_StateChanged;
        }
        catch (Exception ex)
        {
            AppendTestLog($"[Error] Failed to reinitialize SAPI SpeechSynthesizer: {ex.Message}");
        }
    }

    private void PopulateTestVoices(bool reinitialize = false)
    {
        if (reinitialize)
        {
            ReinitializeSynthesizer();
        }

        if (_synthesizer == null) return;

        CmbTestVoice.Items.Clear();
        _installedVoices.Clear();

        try
        {
            var voices = _synthesizer.GetInstalledVoices();

            foreach (var voice in voices)
            {
                if (!voice.Enabled) continue;

                string voiceName = voice.VoiceInfo.Name;
                _installedVoices.Add(voice);
                CmbTestVoice.Items.Add($"{voiceName} ({voice.VoiceInfo.Culture.Name})");
            }

            if (CmbTestVoice.Items.Count > 0)
            {
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
                AutomationProperties.SetName(BtnTestPlay, "Speak");
                BtnTestPause.Visibility = Visibility.Collapsed;
                BtnTestStop.Visibility = Visibility.Collapsed;
            }
            else if (e.State == SynthesizerState.Speaking)
            {
                BtnTestPlay.Visibility = Visibility.Collapsed;
                BtnTestPause.Visibility = Visibility.Visible;
                BtnTestPause.Content = "Pause";
                AutomationProperties.SetName(BtnTestPause, "Pause");
                BtnTestStop.Visibility = Visibility.Visible;
                AutomationProperties.SetName(BtnTestStop, "Stop");
            }
            else if (e.State == SynthesizerState.Paused)
            {
                BtnTestPlay.Visibility = Visibility.Visible;
                BtnTestPlay.Content = "Resume";
                AutomationProperties.SetName(BtnTestPlay, "Resume");
                BtnTestPause.Visibility = Visibility.Collapsed;
                BtnTestStop.Visibility = Visibility.Visible;
                AutomationProperties.SetName(BtnTestStop, "Stop");
            }
        });
    }

    private void AppendTestLog(string message)
    {
        if (TxtTestLog == null) return;

        string timestamp = DateTime.Now.ToString("HH:mm:ss.fff");
        TxtTestLog.AppendText($"[{timestamp}] {message}\n");
        TxtTestLog.ScrollToEnd();
    }

    #endregion
}