using System;
using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace LegoClickerCS.Core;

/// <summary>
/// Represents the game state received from the injected Java agent.
/// Deserialized from JSON sent over TCP from the in-game agent.
/// </summary>
public class GameState
{
    [JsonPropertyName("mapped")]
    public bool Mapped { get; set; }

    [JsonPropertyName("guiOpen")]
    public bool GuiOpen { get; set; }

    [JsonPropertyName("screenName")]
    public string ScreenName { get; set; } = "unknown";

    [JsonPropertyName("health")]
    public float Health { get; set; } = -1;

    [JsonPropertyName("holdingBlock")]
    public bool HoldingBlock { get; set; }

    [JsonPropertyName("lookingAtBlock")]
    public bool LookingAtBlock { get; set; }

    [JsonPropertyName("breakingBlock")]
    public bool BreakingBlock { get; set; }

    [JsonPropertyName("posX")]
    public double PosX { get; set; }

    [JsonPropertyName("posY")]
    public double PosY { get; set; }

    [JsonPropertyName("posZ")]
    public double PosZ { get; set; }

    [JsonPropertyName("entities")]
    public List<EntityInfo> Entities { get; set; } = new();

    /// <summary>
    /// Whether the agent is connected and sending data.
    /// </summary>
    [JsonIgnore]
    public bool IsConnected { get; set; }

    /// <summary>
    /// Timestamp of the last received update.
    /// </summary>
    [JsonIgnore]
    public DateTime LastUpdate { get; set; } = DateTime.MinValue;
}

public class EntityInfo
{
    [JsonPropertyName("sx")]
    public float Sx { get; set; }

    [JsonPropertyName("sy")]
    public float Sy { get; set; }

    [JsonPropertyName("dist")]
    public double Dist { get; set; }

    [JsonPropertyName("name")]
    public string Name { get; set; } = ""; // Was missing in original? No, it wasn't there?

    [JsonPropertyName("hp")]
    public float Hp { get; set; }

    // Legacy / Debug fields (optional)
    [JsonPropertyName("x")]
    public double X { get; set; }
    [JsonPropertyName("y")]
    public double Y { get; set; }
    [JsonPropertyName("z")]
    public double Z { get; set; }

    [JsonIgnore]
    public double Distance => Dist; // Helper alias
}
