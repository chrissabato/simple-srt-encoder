using System;

namespace SimpleSrtEncoder.Services;

/// <summary>
/// Parses a full "srt://host:port?..." URL (as copied from a streaming provider, e.g.
/// FloSports or Hudl) into the discrete fields the SRT settings UI edits individually.
/// Lets users paste one URL into the Host box instead of manually splitting it apart.
/// </summary>
public static class SrtUrlParser
{
    public readonly record struct Result(
        string Host,
        int? Port,
        string? Mode,
        string? StreamId,
        string? Passphrase,
        double? LatencyMs,
        double? PbKeyLen);

    public static bool TryParse(string? text, out Result result)
    {
        result = default;
        if (string.IsNullOrWhiteSpace(text))
        {
            return false;
        }

        text = text.Trim();
        if (!text.StartsWith("srt://", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        if (!Uri.TryCreate(text, UriKind.Absolute, out var uri) ||
            !string.Equals(uri.Scheme, "srt", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        string? mode = null, streamId = null, passphrase = null;
        double? latencyMs = null, pbKeyLen = null;

        var query = uri.Query;
        if (!string.IsNullOrEmpty(query))
        {
            foreach (var pair in query.TrimStart('?').Split('&', StringSplitOptions.RemoveEmptyEntries))
            {
                var kv = pair.Split('=', 2);
                var key = Uri.UnescapeDataString(kv[0]);
                var value = kv.Length > 1 ? Uri.UnescapeDataString(kv[1]) : "";
                switch (key.ToLowerInvariant())
                {
                    case "mode":
                        mode = value;
                        break;
                    case "streamid":
                        streamId = value;
                        break;
                    case "passphrase":
                        passphrase = value;
                        break;
                    case "latency":
                        if (double.TryParse(value, out var lat))
                        {
                            latencyMs = lat;
                        }
                        break;
                    case "pbkeylen":
                        if (double.TryParse(value, out var pbk))
                        {
                            pbKeyLen = pbk;
                        }
                        break;
                }
            }
        }

        result = new Result(
            uri.Host,
            uri.Port > 0 ? uri.Port : null,
            mode,
            streamId,
            passphrase,
            latencyMs,
            pbKeyLen);
        return true;
    }
}
