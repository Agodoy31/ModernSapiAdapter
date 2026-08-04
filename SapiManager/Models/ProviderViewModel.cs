using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace SapiManager.Models;

public class ProviderViewModel : INotifyPropertyChanged
{
    private string _providerId = string.Empty;
    private string _providerName = string.Empty;
    private string _version = "1.0.0";
    private string _exePath = string.Empty;
    private string _pipeName = string.Empty;

    public string ProviderId
    {
        get => _providerId;
        set
        {
            if (_providerId != value)
            {
                _providerId = value;
                OnPropertyChanged();
            }
        }
    }

    public string ProviderName
    {
        get => _providerName;
        set
        {
            if (_providerName != value)
            {
                _providerName = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(ConfigureAutomationName));
            }
        }
    }

    public string Version
    {
        get => _version;
        set
        {
            if (_version != value)
            {
                _version = value;
                OnPropertyChanged();
            }
        }
    }

    public string ExePath
    {
        get => _exePath;
        set
        {
            if (_exePath != value)
            {
                _exePath = value;
                OnPropertyChanged();
            }
        }
    }

    public string PipeName
    {
        get => _pipeName;
        set
        {
            if (_pipeName != value)
            {
                _pipeName = value;
                OnPropertyChanged();
            }
        }
    }

    public ObservableCollection<VoiceViewModel> Voices { get; set; } = new();

    public string ConfigureAutomationName => $"Configure {ProviderName}";

    public event PropertyChangedEventHandler? PropertyChanged;
    protected void OnPropertyChanged([CallerMemberName] string? propertyName = null)
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }
}
