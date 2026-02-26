using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace LegoClickerCS.Core;

public static class GtbWordSolver
{
    private static readonly object Sync = new();
    private static List<string>? _words;
    private static HashSet<string>? _wordSet;
    private static string? _customWordPath;
    private static string _lastMask = "";
    private static IReadOnlyList<string> _lastMatches = Array.Empty<string>();

    public static (string Mask, IReadOnlyList<string> Matches) Solve(string actionBarText, int maxResults = 40)
    {
        string mask = ExtractMask(actionBarText);
        if (string.IsNullOrWhiteSpace(mask))
            return ("", Array.Empty<string>());

        EnsureLoaded();
        var words = _words ?? new List<string>();

        lock (Sync)
        {
            if (mask == _lastMask)
                return (mask, _lastMatches);

            var matches = words
                .Where(w => IsMatch(w, mask))
                .Take(Math.Max(1, maxResults))
                .ToList();

            _lastMask = mask;
            _lastMatches = matches;
            return (mask, matches);
        }
    }

    private static void EnsureLoaded()
    {
        if (_words != null) return;

        lock (Sync)
        {
            if (_words != null) return;
            string baseDir = AppDomain.CurrentDomain.BaseDirectory;
            _customWordPath = Path.Combine(baseDir, "Data", "gtb_wordlist_custom.txt");
            _words = LoadWords(_customWordPath);
            _wordSet = new HashSet<string>(_words, StringComparer.Ordinal);
        }
    }

    public static bool TryLearnSolvedWord(string actionBarText)
    {
        EnsureLoaded();
        string solvedWord = ExtractSolvedWord(actionBarText);
        if (string.IsNullOrWhiteSpace(solvedWord))
            return false;

        lock (Sync)
        {
            if (string.IsNullOrWhiteSpace(_lastMask) || solvedWord.Length != _lastMask.Length || !IsMatch(solvedWord, _lastMask))
                return false;

            var words = _words ?? new List<string>();
            var wordSet = _wordSet ?? new HashSet<string>(StringComparer.Ordinal);
            if (!wordSet.Add(solvedWord))
                return false;

            words.Add(solvedWord);
            _words = words;
            _wordSet = wordSet;
            _lastMask = "";
            _lastMatches = Array.Empty<string>();
            AppendCustomWord(solvedWord);
            return true;
        }
    }

    private static List<string> LoadWords(string customWordPath)
    {
        string baseDir = AppDomain.CurrentDomain.BaseDirectory;
        string[] candidates =
        {
            Path.Combine(baseDir, "Data", "gtb_wordlist.js"),
            Path.Combine(baseDir, "gtb_wordlist.js")
        };

        string? path = candidates.FirstOrDefault(File.Exists);
        if (path == null) return LoadCustomWords(customWordPath);

        string text;
        try
        {
            text = File.ReadAllText(path);
        }
        catch (IOException)
        {
            return LoadCustomWords(customWordPath);
        }
        catch (UnauthorizedAccessException)
        {
            return LoadCustomWords(customWordPath);
        }
        int arrStart = -1;
        int arrEnd = -1;

        int namedIdx = text.IndexOf("wordsData", StringComparison.OrdinalIgnoreCase);
        if (namedIdx >= 0)
        {
            arrStart = text.IndexOf('[', namedIdx);
            arrEnd = arrStart >= 0 ? text.IndexOf("];", arrStart, StringComparison.Ordinal) : -1;
        }

        if (arrStart < 0 || arrEnd <= arrStart)
        {
            int fallbackIdx = text.IndexOf("arrayData", StringComparison.OrdinalIgnoreCase);
            if (fallbackIdx >= 0)
            {
                arrStart = text.IndexOf('[', fallbackIdx);
                arrEnd = arrStart >= 0 ? text.IndexOf("];", arrStart, StringComparison.Ordinal) : -1;
            }
        }

        if (arrStart < 0 || arrEnd <= arrStart)
        {
            arrStart = text.IndexOf('[');
            arrEnd = text.LastIndexOf(']');
        }

        if (arrStart < 0 || arrEnd <= arrStart)
            return LoadCustomWords(customWordPath);

        string arrBody = text.Substring(arrStart, arrEnd - arrStart);
        var matches = Regex.Matches(arrBody, "\"((?:\\\\.|[^\"])*)\"");

        var words = new List<string>(matches.Count);
        foreach (Match m in matches)
        {
            string raw = m.Groups[1].Value;
            string word = NormalizeWord(raw.Replace("\\\"", "\""));
            if (!string.IsNullOrWhiteSpace(word))
                words.Add(word);
        }

        words.AddRange(LoadCustomWords(customWordPath));
        return words.Distinct().ToList();
    }

    private static string ExtractMask(string actionBarText)
    {
        if (string.IsNullOrWhiteSpace(actionBarText)) return "";

        string s = Regex.Replace(actionBarText, "§.", "");
        s = s.Trim();
        if (!s.Contains('_')) return "";

        var themeMatch = Regex.Match(s, @"(?i)\btheme\s+is\s+([A-Za-z_ ]+)");
        if (themeMatch.Success)
            s = themeMatch.Groups[1].Value;
        else
        {
            int colon = s.LastIndexOf(':');
            if (colon >= 0 && colon + 1 < s.Length)
                s = s[(colon + 1)..];
        }

        s = Regex.Replace(s, @"\s*\(\d+\)\s*$", "");
        s = Regex.Replace(s, @"[^A-Za-z_ ]", "");
        s = s.Trim();
        int firstUnderscore = s.IndexOf('_');
        if (firstUnderscore > 0 && s[..firstUnderscore].Contains(' '))
            s = s[firstUnderscore..];

        string decoded = DecodeSpacedMaskIfNeeded(s);
        decoded = Regex.Replace(decoded, @"\s+", " ").Trim().ToLowerInvariant();
        return decoded.Contains('_') ? decoded : "";
    }

    private static string ExtractSolvedWord(string actionBarText)
    {
        if (string.IsNullOrWhiteSpace(actionBarText)) return "";

        string s = Regex.Replace(actionBarText, "§.", "");
        s = s.Trim();
        if (s.Contains('_')) return "";

        var themeMatch = Regex.Match(s, @"(?i)\btheme\s+is\s+([A-Za-z ]+)");
        if (themeMatch.Success)
            s = themeMatch.Groups[1].Value;
        else
        {
            int colon = s.LastIndexOf(':');
            if (colon >= 0 && colon + 1 < s.Length)
                s = s[(colon + 1)..];
        }

        s = Regex.Replace(s, @"\s*\(\d+\)\s*$", "");
        s = Regex.Replace(s, @"[^A-Za-z ]", "");
        return NormalizeWord(s);
    }

    private static string NormalizeWord(string value)
    {
        if (string.IsNullOrWhiteSpace(value)) return "";
        string normalized = Regex.Replace(value, @"[^A-Za-z ]", "");
        normalized = Regex.Replace(normalized, @"\s+", " ").Trim().ToLowerInvariant();
        return normalized.Length >= 3 ? normalized : "";
    }

    private static List<string> LoadCustomWords(string customWordPath)
    {
        if (!File.Exists(customWordPath)) return new List<string>();
        try
        {
            return File.ReadAllLines(customWordPath)
                .Select(NormalizeWord)
                .Where(w => !string.IsNullOrWhiteSpace(w))
                .Distinct()
                .ToList();
        }
        catch (IOException)
        {
            return new List<string>();
        }
        catch (UnauthorizedAccessException)
        {
            return new List<string>();
        }
    }

    private static void AppendCustomWord(string word)
    {
        if (string.IsNullOrWhiteSpace(_customWordPath)) return;
        try
        {
            string? dir = Path.GetDirectoryName(_customWordPath);
            if (!string.IsNullOrWhiteSpace(dir)) Directory.CreateDirectory(dir);
            File.AppendAllText(_customWordPath!, word + Environment.NewLine);
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    private static string DecodeSpacedMaskIfNeeded(string value)
    {
        if (string.IsNullOrWhiteSpace(value)) return value;

        var tokens = value.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        bool looksTokenized =
            tokens.Length >= 4 &&
            tokens.All(t => t.Length == 1 && (t[0] == '_' || char.IsLetter(t[0])));
        if (!looksTokenized) return value;

        var chars = new List<char>(value.Length);
        int i = 0;
        while (i < value.Length)
        {
            char c = value[i];
            if (c == '_' || char.IsLetter(c))
            {
                chars.Add(char.ToLowerInvariant(c));
                int j = i + 1;
                int spaces = 0;
                while (j < value.Length && value[j] == ' ')
                {
                    spaces++;
                    j++;
                }

                if (spaces >= 2 && j < value.Length && chars.Count > 0 && chars[^1] != ' ')
                    chars.Add(' ');
                i = j;
                continue;
            }

            i++;
        }

        return new string(chars.ToArray());
    }

    private static bool IsMatch(string word, string mask)
    {
        if (IsMatchCore(word, mask)) return true;

        string compactWord = word.Replace(" ", "");
        string compactMask = mask.Replace(" ", "");
        if (compactWord == word && compactMask == mask) return false;
        return IsMatchCore(compactWord, compactMask);
    }

    private static bool IsMatchCore(string word, string mask)
    {
        if (word.Length != mask.Length) return false;

        for (int i = 0; i < mask.Length; i++)
        {
            char m = mask[i];
            if (m == '_') continue;
            if (m == ' ')
            {
                if (word[i] != ' ') return false;
                continue;
            }

            if (word[i] != m) return false;
        }

        return true;
    }
}
