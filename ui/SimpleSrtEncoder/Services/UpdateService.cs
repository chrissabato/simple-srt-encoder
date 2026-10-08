using Velopack;
using Velopack.Sources;

namespace SimpleSrtEncoder.Services;

/// <summary>
/// Thin wrapper around Velopack's UpdateManager. IsAvailable is false (and every other
/// member is a no-op) whenever this isn't a Velopack-installed copy of the app — e.g. a
/// plain `dotnet run`/debug launch, or the MSIX debug-identity path used for F5 — so the
/// rest of the app can call this unconditionally instead of checking first, same
/// convention as CaptureCoreService.IsAvailable.
/// </summary>
internal sealed class UpdateService
{
    // Public repo, so no access token is needed here — see GithubSource's accessToken
    // param below, intentionally left empty. Releases get published via release.ps1's
    // `vpk upload github` step.
    private const string GithubRepoUrl = "https://github.com/chrissabato/srt-encoder";

    private readonly UpdateManager _manager = new(new GithubSource(GithubRepoUrl, accessToken: "", prerelease: false));

    public bool IsAvailable => _manager.IsInstalled;

    public async Task<UpdateInfo?> CheckForUpdatesAsync()
    {
        if (!IsAvailable)
        {
            return null;
        }
        try
        {
            return await _manager.CheckForUpdatesAsync();
        }
        catch
        {
            // Feed unreachable (offline, DNS, server down, etc.) — treat exactly like "no
            // update available" rather than surfacing a background check as an error.
            return null;
        }
    }

    // ApplyUpdatesAndRestart exits the current process itself once the new version is
    // staged; nothing after this call runs on success.
    public async Task DownloadAndApplyAsync(UpdateInfo update)
    {
        await _manager.DownloadUpdatesAsync(update);
        _manager.ApplyUpdatesAndRestart(update);
    }
}
