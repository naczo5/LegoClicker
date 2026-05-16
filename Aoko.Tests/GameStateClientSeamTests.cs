using Aoko.Core;

namespace Aoko.Tests;

public class GameStateClientSeamTests
{
    [Theory]
    [InlineData("26.1", "26.1")]
    [InlineData("26", "26.1")]
    [InlineData("minecraft 26", "26.1")]
    [InlineData("1.21.4", "1.21")]
    [InlineData("1.8.9", "1.8.9")]
    [InlineData("1.8.8", "1.8.9")]
    [InlineData("unknown", null)]
    [InlineData("", null)]
    public void NormalizeDetectedVersion_ReturnsExpectedValue(string input, string? expected)
    {
        string? normalized = GameStateClient.NormalizeDetectedVersion(input);
        Assert.Equal(expected, normalized);
    }

    [Theory]
    [InlineData("Default", 0)]
    [InlineData("Minimal", 1)]
    [InlineData("Outlined", 2)]
    [InlineData("Glass", 3)]
    [InlineData("Bold", 4)]
    [InlineData("unknown-style", 0)]
    [InlineData("", 0)]
    public void ModuleListStyleToIndex_MapsStylesSafely(string style, int expected)
    {
        int index = GameStateClient.ModuleListStyleToIndex(style);
        Assert.Equal(expected, index);
    }
}
