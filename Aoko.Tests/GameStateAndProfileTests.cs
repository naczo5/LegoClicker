using System.Text.Json;
using System.Text.Json.Nodes;
using Aoko.Core;

namespace Aoko.Tests;

public class GameStateAndProfileTests
{
    [Fact]
    public void GameState_DeserializesExpectedContract()
    {
        const string json = """
            {
              "mapped": true,
              "guiOpen": false,
              "screenName": "HUD",
              "actionBar": "The theme is C_T",
              "health": 18.5,
              "entities": [
                { "sx": 120.5, "sy": 44.2, "dist": 3.25, "name": "Steve", "hp": 20.0 }
              ]
            }
            """;

        GameState? state = JsonSerializer.Deserialize<GameState>(json);

        Assert.NotNull(state);
        Assert.True(state.Mapped);
        Assert.False(state.GuiOpen);
        Assert.Equal("HUD", state.ScreenName);
        Assert.Equal("The theme is C_T", state.ActionBar);
        Assert.Equal(18.5f, state.Health);
        Assert.Single(state.Entities);
        Assert.Equal("Steve", state.Entities[0].Name);
        Assert.Equal(3.25, state.Entities[0].Distance, 3);
    }

    [Fact]
    public void Profile_DefaultValuesRemainStable()
    {
        var profile = new Profile();

        Assert.Equal("Default", profile.Name);
        Assert.Equal(8.0f, profile.MinCPS);
        Assert.Equal(12.0f, profile.MaxCPS);
        Assert.True(profile.LeftClickEnabled);
        Assert.False(profile.TriggerbotEnabled);
        Assert.Equal(100, profile.ReachChance);
        Assert.True(profile.ModuleKeys.ContainsKey("autoclicker"));
    }

    [Fact]
    public void Profile_CamelCaseSerialization_UsesExpectedKeys()
    {
        var profile = new Profile
        {
            Name = "PvP",
            MinCPS = 9.5f,
            TriggerbotEnabled = true
        };

        var options = new JsonSerializerOptions
        {
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase
        };

        string json = JsonSerializer.Serialize(profile, options);
        JsonNode? node = JsonNode.Parse(json);
        string? minCpsKey = node?.AsObject().Select(kvp => kvp.Key)
            .FirstOrDefault(k => string.Equals(k, "minCPS", StringComparison.OrdinalIgnoreCase));

        Assert.NotNull(node);
        Assert.NotNull(minCpsKey);
        Assert.Equal("PvP", node!["name"]?.GetValue<string>());
        Assert.Equal(9.5, node[minCpsKey!]!.GetValue<double>(), 3);
        Assert.True(node["triggerbotEnabled"]!.GetValue<bool>());
    }

    [Fact]
    public void ProfileManager_UsesAokoProfileDirectory()
    {
        string appData = Path.Combine(Path.GetTempPath(), Guid.NewGuid().ToString("N"));

        string profilesDir = ProfileManager.GetProfilesDirectory(appData);

        Assert.Equal(Path.Combine(appData, "Aoko", "profiles"), profilesDir);
    }

    [Fact]
    public void ProfileManager_InitializesByCopyingLegacyProfilesWhenNewFolderIsEmpty()
    {
        string appData = Path.Combine(Path.GetTempPath(), Guid.NewGuid().ToString("N"));

        try
        {
            string legacyDir = ProfileManager.GetLegacyProfilesDirectory(appData);
            Directory.CreateDirectory(legacyDir);
            File.WriteAllText(Path.Combine(legacyDir, "config.json"), """{"name":"config"}""");

            string profilesDir = ProfileManager.InitializeProfileDirectory(appData);

            Assert.True(File.Exists(Path.Combine(profilesDir, "config.json")));
            Assert.True(File.Exists(Path.Combine(legacyDir, "config.json")));
        }
        finally
        {
            if (Directory.Exists(appData))
                Directory.Delete(appData, recursive: true);
        }
    }

    [Fact]
    public void ProfileManager_DoesNotOverwriteExistingAokoProfiles()
    {
        string appData = Path.Combine(Path.GetTempPath(), Guid.NewGuid().ToString("N"));

        try
        {
            string profilesDir = ProfileManager.GetProfilesDirectory(appData);
            string legacyDir = ProfileManager.GetLegacyProfilesDirectory(appData);
            Directory.CreateDirectory(profilesDir);
            Directory.CreateDirectory(legacyDir);
            File.WriteAllText(Path.Combine(profilesDir, "config.json"), """{"name":"new"}""");
            File.WriteAllText(Path.Combine(legacyDir, "config.json"), """{"name":"legacy"}""");

            ProfileManager.InitializeProfileDirectory(appData);

            string json = File.ReadAllText(Path.Combine(profilesDir, "config.json"));
            Assert.Contains("\"new\"", json);
        }
        finally
        {
            if (Directory.Exists(appData))
                Directory.Delete(appData, recursive: true);
        }
    }
}
