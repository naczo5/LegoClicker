using System.Reflection;
using Aoko.Core;

namespace Aoko.Tests;

public class GtbWordSolverTests
{
    private static readonly BindingFlags NonPublicStatic = BindingFlags.NonPublic | BindingFlags.Static;

    public GtbWordSolverTests()
    {
        ResetSolverState();
        PrepareFreshWordList("cat", "cot", "cut", "dog");
    }

    [Fact]
    public void Solve_ExtractsMask_AndMatchesExpectedWords()
    {
        var result = GtbWordSolver.Solve("The theme is C_T (1)");

        Assert.Equal("c_t", result.Mask);
        Assert.Equal(3, result.Matches.Count);
        Assert.Contains("cat", result.Matches);
        Assert.Contains("cot", result.Matches);
        Assert.Contains("cut", result.Matches);
    }

    [Fact]
    public void Solve_RespectsMaxResultsLimit()
    {
        var result = GtbWordSolver.Solve("Theme is C_T", maxResults: 2);

        Assert.Equal("c_t", result.Mask);
        Assert.Equal(2, result.Matches.Count);
    }

    [Fact]
    public void TryLearnSolvedWord_AddsNewWordForFutureSolves()
    {
        PrepareFreshWordList("cot", "cut", "dog");

        var initial = GtbWordSolver.Solve("The theme is C_T");
        Assert.Equal(2, initial.Matches.Count);
        Assert.DoesNotContain("cat", initial.Matches);

        bool learned = GtbWordSolver.TryLearnSolvedWord("The theme is cat");
        Assert.True(learned);

        var afterLearn = GtbWordSolver.Solve("The theme is C_T");
        Assert.Contains("cat", afterLearn.Matches);
    }

    private static void PrepareFreshWordList(params string[] words)
    {
        string dataDir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Data");
        Directory.CreateDirectory(dataDir);

        string jsArray = string.Join(", ", words.Select(w => $"\"{w}\""));
        string jsPath = Path.Combine(dataDir, "gtb_wordlist.js");
        File.WriteAllText(jsPath, $"const wordsData = [{jsArray}];");

        string customPath = Path.Combine(dataDir, "gtb_wordlist_custom.txt");
        if (File.Exists(customPath))
            File.Delete(customPath);

        ResetSolverState();
    }

    private static void ResetSolverState()
    {
        SetPrivateStaticField("_words", null);
        SetPrivateStaticField("_wordSet", null);
        SetPrivateStaticField("_customWordPath", null);
        SetPrivateStaticField("_lastMask", "");
        SetPrivateStaticField("_lastMatches", Array.Empty<string>());
    }

    private static void SetPrivateStaticField(string fieldName, object? value)
    {
        FieldInfo? field = typeof(GtbWordSolver).GetField(fieldName, NonPublicStatic);
        Assert.NotNull(field);
        field!.SetValue(null, value);
    }
}
