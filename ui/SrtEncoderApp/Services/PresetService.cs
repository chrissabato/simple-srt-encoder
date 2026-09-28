using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using SrtEncoderApp.Models;

namespace SrtEncoderApp.Services;

/// <summary>
/// Loads/saves Preset JSON files under %LOCALAPPDATA%\SrtEncoder\Presets. Preset schema
/// and persistence live entirely here in C# — the native ABI only ever sees plain
/// capture/encode/SRT setting structs, so this schema can evolve independently.
/// </summary>
internal sealed class PresetService
{
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };

    private readonly string _presetsDirectory;

    public PresetService()
    {
        _presetsDirectory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "SrtEncoder", "Presets");
        Directory.CreateDirectory(_presetsDirectory);
    }

    public IReadOnlyList<Preset> ListPresets()
    {
        var presets = new List<Preset>();
        foreach (var file in Directory.EnumerateFiles(_presetsDirectory, "*.json"))
        {
            try
            {
                var json = File.ReadAllText(file);
                var preset = JsonSerializer.Deserialize<Preset>(json, JsonOptions);
                if (preset is not null)
                {
                    presets.Add(preset);
                }
            }
            catch (Exception ex) when (ex is IOException or JsonException)
            {
                // Skip an unreadable/corrupt preset file rather than failing the whole list.
            }
        }
        return presets.OrderBy(p => p.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
    }

    public void Save(Preset preset)
    {
        preset.ModifiedUtc = DateTimeOffset.UtcNow;
        File.WriteAllText(GetPath(preset.Id), JsonSerializer.Serialize(preset, JsonOptions));
    }

    public void Delete(Preset preset)
    {
        var path = GetPath(preset.Id);
        if (File.Exists(path))
        {
            File.Delete(path);
        }
    }

    private string GetPath(string id) => Path.Combine(_presetsDirectory, $"{id}.json");

    /// <summary>Encrypts a plaintext SRT passphrase for storage in PresetSrt.PassphraseProtected (Windows DPAPI, CurrentUser scope — readable only by this Windows account on this machine).</summary>
    public static string? ProtectPassphrase(string? plaintext)
    {
        if (string.IsNullOrEmpty(plaintext))
        {
            return null;
        }
        var protectedBytes = ProtectedData.Protect(Encoding.UTF8.GetBytes(plaintext), optionalEntropy: null, DataProtectionScope.CurrentUser);
        return Convert.ToBase64String(protectedBytes);
    }

    /// <summary>Decrypts a passphrase previously protected by ProtectPassphrase. Call only immediately before CaptureCoreService.StartStream — never log or re-persist the result.</summary>
    public static string UnprotectPassphrase(string? protectedBase64)
    {
        if (string.IsNullOrEmpty(protectedBase64))
        {
            return string.Empty;
        }
        try
        {
            var bytes = ProtectedData.Unprotect(Convert.FromBase64String(protectedBase64), optionalEntropy: null, DataProtectionScope.CurrentUser);
            return Encoding.UTF8.GetString(bytes);
        }
        catch (Exception ex) when (ex is CryptographicException or FormatException)
        {
            // Protected under a different user profile/machine, or corrupted; treat as no passphrase.
            return string.Empty;
        }
    }
}
