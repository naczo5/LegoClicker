using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace LegoClickerCS.Core;

public class Profile
{
    public string Name { get; set; } = "Default";
    public string GuiTheme { get; set; } = "Default";
    public string ModuleListStyle { get; set; } = "Default";
    public bool ShowLogo { get; set; } = true;
    public bool IsArmed { get; set; } = false;
    public float MinCPS { get; set; } = 8.0f;
    public float MaxCPS { get; set; } = 12.0f;
    public bool LeftClickEnabled { get; set; } = true;
    
    public bool RightClickEnabled { get; set; } = false;
    public float RightMinCPS { get; set; } = 10.0f;
    public float RightMaxCPS { get; set; } = 14.0f;
    public bool RightClickOnlyBlock { get; set; } = false;
    
    public bool JitterEnabled { get; set; } = true;
    public bool ClickInChests { get; set; } = false;
    public bool AimAssistEnabled { get; set; } = false;
    public float AimAssistFov { get; set; } = 30.0f;
    public float AimAssistRange { get; set; } = 4.5f;
    public int AimAssistStrength { get; set; } = 40;
    public bool TriggerbotEnabled { get; set; } = false;
    public bool TriggerbotOnlyCrosshair { get; set; } = true;
    public bool TriggerbotOnlyIfCanAttack { get; set; } = true;
    public int TriggerbotCooldownThreshold { get; set; } = 92;
    public int TriggerbotHitChance { get; set; } = 100;
    public bool TriggerbotRequireClick { get; set; } = true;
    public bool GtbHelperEnabled { get; set; } = false;
    public bool NametagsEnabled { get; set; } = false;
    public bool ShowModuleList { get; set; } = true;
    public bool ClosestPlayerInfoEnabled { get; set; } = false;
    public bool NametagShowHealth { get; set; } = true;
    public bool NametagShowArmor { get; set; } = true;
    public bool NametagShowHeldItem { get; set; } = true;
    public int NametagMaxCount { get; set; } = 8;

    public bool ChestEspEnabled { get; set; } = false;
    public int ChestEspMaxCount { get; set; } = 5;

    public bool ReachEnabled { get; set; } = false;
    public float ReachMin { get; set; } = 3.0f;
    public float ReachMax { get; set; } = 6.0f;
    public int ReachChance { get; set; } = 100;

    public bool VelocityEnabled { get; set; } = false;
    public int VelocityHorizontal { get; set; } = 100;
    public int VelocityVertical { get; set; } = 100;
    public int VelocityChance { get; set; } = 100;

    public Dictionary<string, int> ModuleKeys { get; set; } = new()
    {
        ["autoclicker"]   = 0xC0,
        ["rightclick"]    = 0,
        ["jitter"]        = 0,
        ["clickinchests"] = 0,
        ["breakblocks"]   = 0,
        ["aimassist"]     = 0,
        ["triggerbot"]    = 0,
        ["gtbhelper"]     = 0,
        ["nametags"]      = 0,
        ["closestplayer"] = 0,
        ["chestesp"]      = 0,
        ["reach"]         = 0,
        ["velocity"]      = 0,
    };
    public string Theme { get; set; } = "Dark";
}

public static class ProfileManager
{
    private static readonly string ProfilesDir;
    private static readonly JsonSerializerOptions JsonOptions;
    
    static ProfileManager()
    {
        string appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
        ProfilesDir = Path.Combine(appData, "LegoClicker", "profiles");
        Directory.CreateDirectory(ProfilesDir);
        
        JsonOptions = new JsonSerializerOptions
        {
            WriteIndented = true,
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase
        };
    }
    
    public static List<string> GetProfileNames()
    {
        var names = new List<string>();
        
        if (Directory.Exists(ProfilesDir))
        {
            foreach (var file in Directory.GetFiles(ProfilesDir, "*.json"))
            {
                names.Add(Path.GetFileNameWithoutExtension(file));
            }
        }
        
        if (names.Count == 0)
            names.Add("Default");
        
        return names;
    }
    
    public static void SaveProfile(Profile profile)
    {
        string filePath = Path.Combine(ProfilesDir, $"{profile.Name}.json");
        string json = JsonSerializer.Serialize(profile, JsonOptions);
        File.WriteAllText(filePath, json);
    }
    
    public static Profile? LoadProfile(string name)
    {
        string filePath = Path.Combine(ProfilesDir, $"{name}.json");
        
        if (!File.Exists(filePath))
            return null;
        
        try
        {
            string json = File.ReadAllText(filePath);
            return JsonSerializer.Deserialize<Profile>(json, JsonOptions);
        }
        catch
        {
            return null;
        }
    }
    
    public static void DeleteProfile(string name)
    {
        string filePath = Path.Combine(ProfilesDir, $"{name}.json");
        if (File.Exists(filePath))
            File.Delete(filePath);
    }
    
    public static Profile CreateFromClicker()
    {
        var clicker = Clicker.Instance;
        return new Profile
        {
            IsArmed = clicker.IsArmed,
            MinCPS = clicker.MinCPS,
            MaxCPS = clicker.MaxCPS,
            LeftClickEnabled = clicker.LeftClickEnabled,
            GuiTheme = clicker.GuiTheme,
            ModuleListStyle = clicker.ModuleListStyle,
            ShowLogo = clicker.ShowLogo,
            
            RightClickEnabled = clicker.RightClickEnabled,
            RightMinCPS = clicker.RightMinCPS,
            RightMaxCPS = clicker.RightMaxCPS,
            RightClickOnlyBlock = clicker.RightClickOnlyBlock,
            
            JitterEnabled = clicker.JitterEnabled,
            ClickInChests = clicker.ClickInChests,
            AimAssistEnabled = clicker.AimAssistEnabled,
            AimAssistFov = clicker.AimAssistFov,
            AimAssistRange = clicker.AimAssistRange,
            AimAssistStrength = clicker.AimAssistStrength,
            TriggerbotEnabled = clicker.TriggerbotEnabled,
            TriggerbotOnlyCrosshair = clicker.TriggerbotOnlyCrosshair,
            TriggerbotOnlyIfCanAttack = clicker.TriggerbotOnlyIfCanAttack,
            TriggerbotCooldownThreshold = clicker.TriggerbotCooldownThreshold,
            TriggerbotHitChance = clicker.TriggerbotHitChance,
            TriggerbotRequireClick = clicker.TriggerbotRequireClick,
            GtbHelperEnabled = clicker.GtbHelperEnabled,
            NametagsEnabled = clicker.NametagsEnabled,
            ShowModuleList = clicker.ShowModuleList,
            ClosestPlayerInfoEnabled = clicker.ClosestPlayerInfoEnabled,
            NametagShowHealth = clicker.NametagShowHealth,
            NametagShowArmor = clicker.NametagShowArmor,
            NametagShowHeldItem = clicker.NametagShowHeldItem,
            NametagMaxCount = clicker.NametagMaxCount,
            ChestEspEnabled = clicker.ChestEspEnabled,
            ChestEspMaxCount = clicker.ChestEspMaxCount,

            ReachEnabled = clicker.ReachEnabled,
            ReachMin = clicker.ReachMin,
            ReachMax = clicker.ReachMax,
            ReachChance = clicker.ReachChance,

            VelocityEnabled = clicker.VelocityEnabled,
            VelocityHorizontal = clicker.VelocityHorizontal,
            VelocityVertical = clicker.VelocityVertical,
            VelocityChance = clicker.VelocityChance,

            ModuleKeys = new Dictionary<string, int>(InputHooks.ModuleKeys),
            Theme = ThemeManager.CurrentTheme
        };
    }
    
    public static void ApplyToClicker(Profile profile)
    {
        var clicker = Clicker.Instance;

        if (profile.IsArmed)
            clicker.Arm();
        else
            clicker.Disarm();

        clicker.MinCPS = profile.MinCPS;
        clicker.MaxCPS = profile.MaxCPS;
        clicker.LeftClickEnabled = profile.LeftClickEnabled;
        if (profile.GuiTheme != null) clicker.GuiTheme = profile.GuiTheme;
        if (profile.ModuleListStyle != null) clicker.ModuleListStyle = profile.ModuleListStyle;
        clicker.ShowLogo = profile.ShowLogo;
        
        clicker.RightClickEnabled = profile.RightClickEnabled;
        clicker.RightMinCPS = profile.RightMinCPS;
        clicker.RightMaxCPS = profile.RightMaxCPS;
        clicker.RightClickOnlyBlock = profile.RightClickOnlyBlock;
        
        clicker.JitterEnabled = profile.JitterEnabled;
        clicker.ClickInChests = profile.ClickInChests;
        clicker.AimAssistEnabled = profile.AimAssistEnabled;
        clicker.AimAssistFov = profile.AimAssistFov;
        clicker.AimAssistRange = profile.AimAssistRange;
        clicker.AimAssistStrength = profile.AimAssistStrength;
        clicker.TriggerbotEnabled = profile.TriggerbotEnabled;
        clicker.TriggerbotOnlyCrosshair = profile.TriggerbotOnlyCrosshair;
        clicker.TriggerbotOnlyIfCanAttack = profile.TriggerbotOnlyIfCanAttack;
        clicker.TriggerbotCooldownThreshold = profile.TriggerbotCooldownThreshold;
        clicker.TriggerbotHitChance = profile.TriggerbotHitChance;
        clicker.TriggerbotRequireClick = profile.TriggerbotRequireClick;
        clicker.GtbHelperEnabled = profile.GtbHelperEnabled;
        clicker.NametagsEnabled = profile.NametagsEnabled;
        clicker.ShowModuleList = profile.ShowModuleList;
        clicker.ClosestPlayerInfoEnabled = profile.ClosestPlayerInfoEnabled;
        clicker.NametagShowHealth = profile.NametagShowHealth;
        clicker.NametagShowArmor = profile.NametagShowArmor;
        clicker.NametagShowHeldItem = profile.NametagShowHeldItem;
        clicker.NametagMaxCount = profile.NametagMaxCount;
        clicker.ChestEspEnabled = profile.ChestEspEnabled;
        clicker.ChestEspMaxCount = profile.ChestEspMaxCount;

        clicker.ReachEnabled = profile.ReachEnabled;
        clicker.ReachMin = profile.ReachMin;
        clicker.ReachMax = profile.ReachMax;
        clicker.ReachChance = profile.ReachChance;

        clicker.VelocityEnabled = profile.VelocityEnabled;
        clicker.VelocityHorizontal = profile.VelocityHorizontal;
        clicker.VelocityVertical = profile.VelocityVertical;
        clicker.VelocityChance = profile.VelocityChance;

        foreach (var kvp in profile.ModuleKeys)
            InputHooks.SetModuleKey(kvp.Key, kvp.Value);
        ThemeManager.ApplyTheme(profile.Theme);
    }
}
