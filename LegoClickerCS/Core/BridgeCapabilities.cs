using System;
using System.Collections.Generic;
using System.Text.Json.Nodes;

namespace LegoClickerCS.Core;

public sealed class BridgeCapabilities
{
    private readonly HashSet<string> _modules;
    private readonly HashSet<string> _settings;
    private readonly HashSet<string> _stateFields;

    public int ModuleCount => _modules.Count;
    public int SettingCount => _settings.Count;
    public int StateFieldCount => _stateFields.Count;

    private BridgeCapabilities(HashSet<string> modules, HashSet<string> settings, HashSet<string> stateFields)
    {
        _modules = modules;
        _settings = settings;
        _stateFields = stateFields;
    }

    public static BridgeCapabilities ForVersionFallback(string? injectedVersion)
    {
        bool is121 = !string.IsNullOrWhiteSpace(injectedVersion)
            && injectedVersion.StartsWith("1.21", StringComparison.OrdinalIgnoreCase);
        bool is261 = !string.IsNullOrWhiteSpace(injectedVersion)
            && injectedVersion.StartsWith("26.1", StringComparison.OrdinalIgnoreCase);

        if (is121 || is261)
        {
            return new BridgeCapabilities(
                BuildSet(
                    "autoclicker",
                    "rightclick",
                    "jitter",
                    "clickinchests",
                    "breakblocks",
                    "aimassist",
                    "triggerbot",
                    "gtbhelper",
                    "nametags",
                    "closestplayer",
                    "chestesp",
                    "reach",
                    "velocity"
                ),
                BuildSet(
                    "mincps",
                    "maxcps",
                    "left",
                    "right",
                    "rightmincps",
                    "rightmaxcps",
                    "rightblock",
                    "breakblocks",
                    "jitter",
                    "clickinchests",
                    "aimassistfov",
                    "aimassistrange",
                    "aimassiststrength",
                    "triggerbot",
                    "gtbhint",
                    "gtbcount",
                    "gtbpreview",
                    "nametags",
                    "closestplayerinfo",
                    "nametagshowhealth",
                    "nametagshowarmor",
                    "nametagshowhelditem",
                    "nametagmaxcount",
                    "chestesp",
                    "chestespmaxcount",
                    "reachenabled",
                    "reachmin",
                    "reachmax",
                    "reachchance",
                    "velocityenabled",
                    "velocityhorizontal",
                    "velocityvertical",
                    "velocitychance",
                    "showmodulelist",
                    "moduleliststyle",
                    "showlogo",
                    "guitheme"
                ),
                BuildSet(
                    "actionbar",
                    "lookingatentity",
                    "lookingatentitylatched",
                    "breakingblock",
                    "attackcooldown",
                    "attackcooldownpertick",
                    "statems"
                )
            );
        }

        return new BridgeCapabilities(
            BuildSet(
                "autoclicker",
                "rightclick",
                "jitter",
                "clickinchests",
                "breakblocks",
                "aimassist",
                "gtbhelper",
                "nametags",
                "closestplayer",
                "chestesp",
                "reach",
                "velocity"
            ),
            BuildSet(
                "mincps",
                "maxcps",
                "left",
                "right",
                "rightmincps",
                "rightmaxcps",
                "rightblock",
                "breakblocks",
                "jitter",
                "clickinchests",
                "aimassistfov",
                "aimassistrange",
                "aimassiststrength",
                "nametags",
                "closestplayerinfo",
                "nametagshowhealth",
                "nametagshowarmor",
                "chestesp",
                "reachenabled",
                "reachmin",
                "reachmax",
                "reachchance",
                "velocityenabled",
                "velocityhorizontal",
                "velocityvertical",
                "velocitychance",
                "showmodulelist",
                "moduleliststyle",
                "showlogo",
                "guitheme",
                "keybindautoclicker",
                "keybindnametags",
                "keybindclosestplayer",
                "keybindchestesp"
            ),
            BuildSet(
                "actionbar",
                "holdingblock",
                "lookingatblock",
                "lookingatentity",
                "lookingatentitylatched",
                "breakingblock",
                "attackcooldown",
                "attackcooldownpertick",
                "statems"
            )
        );
    }

    public static BridgeCapabilities FromPayload(JsonNode? node, BridgeCapabilities fallback)
    {
        var modules = ParseStringArray(node?["modules"]);
        var settings = ParseStringArray(node?["settings"]);
        var stateFields = ParseStringArray(node?["state"]);

        if (modules.Count == 0 && settings.Count == 0 && stateFields.Count == 0)
            return fallback;

        if (modules.Count == 0) modules = new HashSet<string>(fallback._modules, StringComparer.OrdinalIgnoreCase);
        if (settings.Count == 0) settings = new HashSet<string>(fallback._settings, StringComparer.OrdinalIgnoreCase);
        if (stateFields.Count == 0) stateFields = new HashSet<string>(fallback._stateFields, StringComparer.OrdinalIgnoreCase);

        return new BridgeCapabilities(modules, settings, stateFields);
    }

    public bool SupportsModule(string moduleId)
        => _modules.Contains(Normalize(moduleId));

    public bool SupportsSetting(string settingName)
        => _settings.Contains(Normalize(settingName));

    public bool SupportsStateField(string fieldName)
        => _stateFields.Contains(Normalize(fieldName));

    private static HashSet<string> BuildSet(params string[] values)
    {
        var set = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (string value in values)
        {
            string normalized = Normalize(value);
            if (!string.IsNullOrWhiteSpace(normalized))
                set.Add(normalized);
        }
        return set;
    }

    private static HashSet<string> ParseStringArray(JsonNode? node)
    {
        var set = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        if (node is not JsonArray arr) return set;

        foreach (JsonNode? item in arr)
        {
            string? raw = item?.GetValue<string>();
            string normalized = Normalize(raw);
            if (!string.IsNullOrWhiteSpace(normalized))
                set.Add(normalized);
        }

        return set;
    }

    private static string Normalize(string? value)
        => string.IsNullOrWhiteSpace(value)
            ? string.Empty
            : value.Trim().ToLowerInvariant();
}
