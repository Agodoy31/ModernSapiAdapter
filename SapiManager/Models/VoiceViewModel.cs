using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace SapiManager.Models;

public class VoiceViewModel : INotifyPropertyChanged
{
    private bool _isRegistered;
    private string _name = string.Empty;
    private string _language = string.Empty;
    private string _gender = string.Empty;
    private string _vendor = string.Empty;

    public string VoiceId { get; set; } = string.Empty;

    public string Name
    {
        get => _name;
        set
        {
            if (_name != value)
            {
                _name = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(FullAccessibilityLabel));
            }
        }
    }

    public string Language
    {
        get => _language;
        set
        {
            if (_language != value)
            {
                _language = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(FullAccessibilityLabel));
            }
        }
    }

    public string Gender
    {
        get => _gender;
        set
        {
            if (_gender != value)
            {
                _gender = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(FullAccessibilityLabel));
            }
        }
    }

    public string Vendor
    {
        get => _vendor;
        set
        {
            if (_vendor != value)
            {
                _vendor = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(FullAccessibilityLabel));
            }
        }
    }

    public bool IsRegistered
    {
        get => _isRegistered;
        set
        {
            if (_isRegistered != value)
            {
                _isRegistered = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(FullAccessibilityLabel));
            }
        }
    }

    /// <summary>
    /// Critical JAWS screen reader override property.
    /// Provides a cohesive single-sentence announcement when ListBoxItem receives focus.
    /// Example: "Microsoft Jenny, English US, Female. Unchecked."
    /// </summary>
    public string FullAccessibilityLabel
    {
        get
        {
            string state = IsRegistered ? "Checked" : "Unchecked";
            return $"{Name}, {Language}, {Gender}. {state}.";
        }
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    protected void OnPropertyChanged([CallerMemberName] string? propertyName = null)
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }
}
