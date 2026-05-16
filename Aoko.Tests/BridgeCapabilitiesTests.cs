using System.Text.Json.Nodes;
using Aoko.Core;

namespace Aoko.Tests;

public class BridgeCapabilitiesTests
{
    [Fact]
    public void ModernFallback_ExposesModernFeatures()
    {
        BridgeCapabilities caps = BridgeCapabilities.ForVersionFallback("26.1");

        Assert.True(caps.SupportsModule("triggerbot"));
        Assert.True(caps.SupportsSetting("nametagshowhelditem"));
        Assert.True(caps.SupportsStateField("attackcooldown"));
    }

    [Fact]
    public void LegacyFallback_DoesNotExposeTriggerbot()
    {
        BridgeCapabilities caps = BridgeCapabilities.ForVersionFallback("1.8.9");

        Assert.False(caps.SupportsModule("triggerbot"));
        Assert.False(caps.SupportsSetting("triggerbot"));
        Assert.True(caps.SupportsStateField("holdingblock"));
    }

    [Fact]
    public void FromPayload_UsesFallbackWhenArraysMissing()
    {
        BridgeCapabilities fallback = BridgeCapabilities.ForVersionFallback("1.8.9");
        var payload = new JsonObject { ["type"] = "capabilities" };

        BridgeCapabilities parsed = BridgeCapabilities.FromPayload(payload, fallback);

        Assert.Equal(fallback.ModuleCount, parsed.ModuleCount);
        Assert.True(parsed.SupportsModule("autoclicker"));
        Assert.False(parsed.SupportsModule("triggerbot"));
    }

    [Fact]
    public void FromPayload_MergesAndNormalizesPayloadData()
    {
        BridgeCapabilities fallback = BridgeCapabilities.ForVersionFallback("1.8.9");
        var payload = new JsonObject
        {
            ["modules"] = new JsonArray("TRIGGERBOT"),
            ["state"] = new JsonArray("ActionBar")
        };

        BridgeCapabilities parsed = BridgeCapabilities.FromPayload(payload, fallback);

        Assert.True(parsed.SupportsModule("triggerbot"));
        Assert.True(parsed.SupportsStateField("actionbar"));
        Assert.True(parsed.SupportsSetting("mincps"));
    }
}
